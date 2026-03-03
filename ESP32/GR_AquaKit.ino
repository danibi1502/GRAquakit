//ESP32
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <ESP32Servo.h> 
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "RTClib.h"

// BLE Setup
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

BLECharacteristic*pRxCharacteristic;

// Forward declaration so the BLE Callback can see this function
void handleIncomingByte(byte b);

// Global buffer for partial commands
byte packetBuffer[5];
int packetIndex = 0;

// --- Pin Definitions ---
#define LED_PIN         21
#define FOOD_SERVO_PIN  17
#define ECHO_PIN        22  // Ultrasonic
#define TRIG_PIN        23  // Ultrasonic
#define SDA_PIN         25
#define SCL_PIN         26
#define FILTER_PUMP_PIN 18
#define WATER_CHG_PIN   13

#define NUM_LEDS 24
#define MAX_CMDS 50
#define CMD_SIZE 5
#define EEPROM_ADDR 0x50 

// Device identifiers
#define DEVICE_LIGHTS         1
#define DEVICE_FILTER_PUMP    2
#define DEVICE_FOOD_DISPENSER 3
#define DEVICE_WATER_CHANGE   4

// Opcodes
#define OP_SET_LED_COLOR            0x05
#define OP_SET_LED_BRIGHTNESS       0x06
#define OP_SET_LED_PIXEL_COLOR      0x07
#define OP_SET_LED_PIXEL_BRIGHTNESS 0x08
#define OP_SET_LED_PIXEL_ONOFF      0x09
#define OP_CLEAR_PIXELS             0x0A
#define OP_REPEAT_N_TIMES           0x0B
#define OP_LOOP_END                 0x0C 
#define OP_IF_CONDITION             0x20
#define OP_ELSE                     0x21
#define OP_ENDIF                    0x22
#define OP_SET_VARIABLE             0x40
#define OP_CHANGE_VARIABLE          0x41

struct LightState { bool on; byte r, g, b; byte brightness; };
struct PixelState { bool on; bool overrideColor; byte r, g, b; byte brightness; };
struct Command { byte op; byte data[4]; };

PixelState pixels[NUM_LEDS];
LightState lights = { false, 255, 255, 255, 100 };
Command program[MAX_CMDS];

RTC_DS3231 rtc; // or RTC_DS1307 rtc; depending on your module

int programLength = 0;
bool loopProgram = false;
int loopStartIndex = 0;
bool receivingProgram = false;
bool runningFromEEPROM = false;
int currentCmd = 0;
int setColours[72] = {0};
int setPixels[24] = {0};
int previousOp = 0;
int currentOp = 0;
int16_t variables[20] = {0};   // supports 20 variables

unsigned long lastReport = 0;

struct LoopFrame {
  int startIndex;    
  int remaining;
};

LoopFrame loopStack[5]; 
int stackPtr = -1;      

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
Servo foodServo;

// BLE Callback - Fixed std::string error
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String value = pCharacteristic->getValue(); // Fixed to use String
      for (int i = 0; i < value.length(); i++) {
        handleIncomingByte((byte)value[i]);
      }
    }
};

// --- External EEPROM Helper Functions ---
void extEEPROM_write(unsigned int addr, byte data) {
  Wire.beginTransmission(EEPROM_ADDR);
  Wire.write((int)(addr >> 8));   
  Wire.write((int)(addr & 0xFF)); 
  Wire.write(data);
  Wire.endTransmission();
  delay(5); 
}

byte extEEPROM_read(unsigned int addr) {
  byte rdata = 0xFF;
  Wire.beginTransmission(EEPROM_ADDR);
  Wire.write((int)(addr >> 8));
  Wire.write((int)(addr & 0xFF));
  Wire.endTransmission();
  Wire.requestFrom(EEPROM_ADDR, 1);
  if (Wire.available()) rdata = Wire.read();
  return rdata;
}


bool isEEPROMConnected() {
  Wire.beginTransmission(EEPROM_ADDR);
  byte error = Wire.endTransmission();
  return (error == 0); // Returns true if the chip responded
}

void setup() {
  Serial.begin(115200); 

  // unsigned long startWait = millis();
  // while (!Serial && (millis() - startWait < 3000)) {
  //   delay(10);
  // }

  pinMode(ECHO_PIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100); 

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
  }
  rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); // Sets to compile time
  
  for (int i = 0; i < NUM_LEDS; i++) {
    pixels[i] = { true, false, 255, 255, 255, 100 };
  }

  pinMode(FILTER_PUMP_PIN, OUTPUT);
  pinMode(WATER_CHG_PIN, OUTPUT);
  
  ESP32PWM::allocateTimer(0);
  foodServo.setPeriodHertz(50);
  foodServo.attach(FOOD_SERVO_PIN, 500, 2400); 
  foodServo.write(90);

  strip.begin();

  // --- BLE Initialization ---
  BLEDevice::init("AquaKit-Hybrid"); 
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  pRxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_RX,
                        BLECharacteristic::PROPERTY_WRITE |
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  // Add the Descriptor so the phone/computer knows it can listen
  pRxCharacteristic->addDescriptor(new BLE2902());

  pRxCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println(">>> BLE Wireless Ready!");

  strip.show();

  // --- Conditional EEPROM Load ---
  if (isEEPROMConnected()) {
    Serial.println("EEPROM detected. Loading program...");
    loadProgramFromEEPROM();
    if (programLength > 0) {
      runningFromEEPROM = true;
      Serial.printf("Loaded %d steps from External EEPROM\n", programLength);
    }
  } else {
    Serial.println("EEPROM not found. Skipping load.");
    runningFromEEPROM = false;
  }
}

void loop() {
  while (Serial.available() > 0) {
    handleIncomingByte(Serial.read());
  }

  // Report every 500ms so the UI feels responsive
  if (millis() - lastReport > 500) {
    float depth = getWaterDepth();
    DateTime now = rtc.now();
    
    // Format: HH:MM
    char timeStr[6];
    sprintf(timeStr, "%02d:%02d", now.hour(), now.minute());

    // Send formatted string to UI
    String msg = "VALUE:DEPTH:" + String(depth) + "|TIME:" + String(timeStr) + "\n";
    
    // Send to USB
    Serial.print(msg);
    
    // Send to Bluetooth
    if (pRxCharacteristic) {
        pRxCharacteristic->setValue(msg.c_str());
        pRxCharacteristic->notify();
    }

    lastReport = millis();
  }

  if (runningFromEEPROM && programLength > 0) {
    executeProgram();
  }
}

void renderLights() {
  uint8_t globalB = map(lights.brightness, 0, 100, 0, 255);
  for (int i = 0; i < NUM_LEDS; i++) {
    if (!lights.on || !pixels[i].on) {
      strip.setPixelColor(i, 0);
      continue;
    }
    byte r, g, b;
    if (pixels[i].overrideColor) {
      r = pixels[i].r; g = pixels[i].g; b = pixels[i].b;
    } else {
      r = lights.r; g = lights.g; b = lights.b;
    }
    uint8_t pixelB = map(pixels[i].brightness, 0, 100, 0, 255);
    uint16_t finalB = (pixelB * globalB) / 255;
    strip.setPixelColor(i, (r * finalB) / 255, (g * finalB) / 255, (b * finalB) / 255);
  }
  strip.show();
}

void renderSetPixels() {
  for (int i = 0; i < 24; i++) {
    if (setPixels[i]) {
      pixels[i].on = true;
      pixels[i].overrideColor = true;
      pixels[i].r = setColours[i*3]; pixels[i].g = setColours[i*3+1]; pixels[i].b = setColours[i*3+2];
    }
  }
  renderLights();
}

void controlDevice(byte device, byte state) {
  switch (device) {
    case DEVICE_LIGHTS:
      lights.on = state;
      renderLights();
      break;
    case DEVICE_FILTER_PUMP:
      digitalWrite(FILTER_PUMP_PIN, state);
      break;
    case DEVICE_WATER_CHANGE:
      digitalWrite(WATER_CHG_PIN, state);
      break;
    case DEVICE_FOOD_DISPENSER:
      foodServo.write(state ? 80 : 90);
      break;
  }
}

unsigned long toULong(byte b1, byte b2, byte b3, byte b4) {
  return (unsigned long)b1 | ((unsigned long)b2 << 8) |
         ((unsigned long)b3 << 16) | ((unsigned long)b4 << 24);
}

bool userCondition(byte condType, byte value) {
  // condType is cmd.data[0] (sent as 1 for true, 0 for false from Blockly)
  // value is cmd.data[1] (unused currently)
  
  if (condType == 1) {
    return true;  
  } else {
    return false; 
  }
}

void executeProgram() {
  static bool waiting = false;
  static unsigned long delayStart, delayDuration;

  if (waiting) {
    if (millis() - delayStart >= delayDuration) {
      waiting = false;
      currentCmd++;
    }
    return;
  }
  
  if (currentCmd >= programLength) {
    if (loopProgram) {
      currentCmd = loopStartIndex;
      stackPtr = -1; 
      Serial.println(">> Program Looping...");
    } else {
      runningFromEEPROM = false;
      Serial.println(">> Program Finished.");
      return;
    }
  }

  Command &cmd = program[currentCmd];
  currentOp = cmd.op;

  if (cmd.op == OP_IF_CONDITION) {

    bool result = userCondition(cmd.data[0], cmd.data[1]);

    if (!result) {
      // Skip commands until ENDIF
      int depth = 1;
      while (depth > 0 && currentCmd < programLength - 1) {
        currentCmd++;
        if (program[currentCmd].op == OP_IF_CONDITION) depth++;
        else if (program[currentCmd].op == OP_ENDIF) depth--;
        else if (program[currentCmd].op == OP_ELSE && depth == 1) {
          currentCmd++;  
          return;
        }
      }
      return;
    }
    currentCmd++;
    return;
  }

  else if (cmd.op == OP_ENDIF) {
    currentCmd++;
    return;
  }
  else if (cmd.op == OP_ELSE) {
  // IF was true → skip ELSE block
  int depth = 1;
  while (depth > 0 && currentCmd < programLength - 1) {
    currentCmd++;

    if (program[currentCmd].op == OP_IF_CONDITION) depth++;
    else if (program[currentCmd].op == OP_ENDIF) depth--;
  }
  currentCmd++;
  return;
  }
  else if (cmd.op == OP_ENDIF) {
    currentCmd++;
    return;
  }

  if (cmd.op == OP_REPEAT_N_TIMES) {
    if (stackPtr < 4) { 
      stackPtr++;
      loopStack[stackPtr].startIndex = currentCmd + 1; 
      loopStack[stackPtr].remaining = cmd.data[0] - 1;
    }
    currentCmd++;
    return;
  }
  else if (cmd.op == OP_LOOP_END) {
    if (stackPtr >= 0) {
      if (loopStack[stackPtr].remaining > 0) {
        loopStack[stackPtr].remaining--;
        currentCmd = loopStack[stackPtr].startIndex; 
      } else {
        stackPtr--; 
        currentCmd++;
      }
    } else {
      currentCmd++; 
    }
    return;
  }

  if (cmd.op == 0x00) {
    controlDevice(cmd.data[0], cmd.data[1]);
    currentCmd++;
  }
  else if (cmd.op == 0x02) {
    delayDuration = toULong(cmd.data[0], cmd.data[1], cmd.data[2], cmd.data[3]);
    delayStart = millis();
    waiting = true;
  }
  else if (cmd.op == OP_SET_LED_COLOR) {
    lights.r = cmd.data[0]; lights.g = cmd.data[1]; lights.b = cmd.data[2];
    for(int i=0; i<NUM_LEDS; i++) pixels[i].overrideColor = false;
    renderLights();
    currentCmd++;
  }
  else if (cmd.op == OP_SET_LED_BRIGHTNESS) {
    lights.brightness = min((byte)cmd.data[0], (byte)100);
    renderLights();
    currentCmd++;
  }
  else if (cmd.op == OP_SET_LED_PIXEL_COLOR) {
    byte i = cmd.data[0];
    setPixels[i] = 1;
    setColours[i*3] = cmd.data[1];
    setColours[i*3+1] = cmd.data[2];
    setColours[i*3+2] = cmd.data[3];
    currentCmd++;
  }
  else if (cmd.op == OP_SET_LED_PIXEL_BRIGHTNESS) {
    pixels[cmd.data[0]].brightness = min((byte)cmd.data[1], (byte)100);
    renderLights();
    currentCmd++;
  }
  else if (cmd.op == OP_SET_LED_PIXEL_ONOFF) {
    pixels[cmd.data[0]].on = cmd.data[1];
    renderLights();
    currentCmd++;
  }
  else if (cmd.op == OP_CLEAR_PIXELS) {
    for (int i = 0; i < NUM_LEDS; i++) {
      pixels[i].on = false;
      pixels[i].overrideColor = false;
    }
    renderLights();
    currentCmd++;
  }
  else {
    currentCmd++;
  }
  
  if (previousOp == OP_SET_LED_PIXEL_COLOR && currentOp != OP_SET_LED_PIXEL_COLOR) {
    renderSetPixels();
  }
  previousOp = cmd.op;
}

void storeCommand(byte op, byte d1, byte d2, byte d3, byte d4) {
  if (programLength >= MAX_CMDS) return;
  program[programLength++] = { op, { d1, d2, d3, d4 } };
}

void saveProgramToEEPROM() {
  bool needsUpdate = false;
  Serial.println(">>> EEPROM Check: Comparing incoming program with stored memory...");
  if (extEEPROM_read(0) != (byte)programLength) {
    needsUpdate = true;
    Serial.println("    - Length mismatch detected.");
  }
  if (!needsUpdate) {
    for (int i = 0; i < programLength; i++) {
      int a = 1 + i * CMD_SIZE;
      if (extEEPROM_read(a) != program[i].op) {
        needsUpdate = true;
        Serial.printf("    - Command %d Opcode mismatch.\n", i);
        break;
      }
      for (int j = 0; j < 4; j++) {
        if (extEEPROM_read(a + 1 + j) != program[i].data[j]) {
          needsUpdate = true;
          Serial.printf("    - Command %d Data byte %d mismatch.\n", i, j);
          break;
        }
      }
      if (needsUpdate) break;
    }
  }
  if (needsUpdate) {
    Serial.print(">>> EEPROM Writing: Saving new program... ");
    extEEPROM_write(0, (byte)programLength);
    for (int i = 0; i < programLength; i++) {
      int a = 1 + i * CMD_SIZE;
      extEEPROM_write(a, program[i].op);
      for (int j = 0; j < 4; j++) extEEPROM_write(a + 1 + j, program[i].data[j]);
    }
    extEEPROM_write(1 + programLength * CMD_SIZE, (byte)loopStartIndex);
    extEEPROM_write(2 + programLength * CMD_SIZE, (byte)loopProgram);
    Serial.println("DONE.");
  } else {
    Serial.println(">>> EEPROM Skipped: Current program matches stored memory. No rewrite needed.");
  }
}

void loadProgramFromEEPROM() {
  programLength = extEEPROM_read(0);
  if (programLength > MAX_CMDS) { programLength = 0; return; }
  for (int i = 0; i < programLength; i++) {
    int a = 1 + i * CMD_SIZE;
    program[i].op = extEEPROM_read(a);
    for (int j = 0; j < 4; j++) program[i].data[j] = extEEPROM_read(a + 1 + j);
  }
  loopStartIndex = extEEPROM_read(1 + programLength * CMD_SIZE);
  loopProgram = extEEPROM_read(2 + programLength * CMD_SIZE);
}

void handleIncomingByte(byte b) {
  // 1. End of program check
  if (b == 0xFF) {
    if (programLength > 0) {
      saveProgramToEEPROM();
      runningFromEEPROM = true;
    }
    receivingProgram = false;
    currentCmd = 0;
    packetIndex = 0;
    while(Serial.available() > 0) { Serial.read(); } // Clear trailing bytes
    Serial.println("\n>>> End of Program Received");
    return;
  }

  // 2. New stream initialization
  if (!receivingProgram) {
    programLength = 0;
    receivingProgram = true;
    runningFromEEPROM = false;
    packetIndex = 0;
    loopProgram = false; // Reset loop flag for new program
    Serial.println("\n>>> New Stream Started. Clearing RAM program.");
  }

  // 3. Buffer the packet
  packetBuffer[packetIndex++] = b;

  // 4. Process full 5-byte command
  if (packetIndex == 5) {
    byte op = packetBuffer[0];
    byte d1 = packetBuffer[1];
    byte d2 = packetBuffer[2];
    byte d3 = packetBuffer[3];
    byte d4 = packetBuffer[4];

    Serial.printf("    CMD Assembled: [Op:0x%02X, D1:0x%02X, D2:0x%02X, D3:0x%02X, D4:0x%02X]\n", op, d1, d2, d3, d4);

    // Immediate Hardware Trigger (Interactive feedback)
    if (op == 0x00) { 
        controlDevice(d1, d2); 
    } else if (op == 0x03) { 
        loopProgram = true; 
        loopStartIndex = programLength; 
    }

    // STORE ONCE: Save to RAM program array
    storeCommand(op, d1, d2, d3, d4);
    
    // Reset for next command
    packetIndex = 0; 
  }
}

float getWaterDepth() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  long duration = pulseIn(ECHO_PIN, HIGH);
  float distance = duration * 0.034 / 2;
  return (distance > 400 || distance <= 0) ? 0 : distance;
}