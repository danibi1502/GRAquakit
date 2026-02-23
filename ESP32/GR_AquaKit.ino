//ESP32
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <ESP32Servo.h> 
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// BLE Setup
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
BLECharacteristic *pRxCharacteristic;

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

struct LightState { bool on; byte r, g, b; byte brightness; };
struct PixelState { bool on; bool overrideColor; byte r, g, b; byte brightness; };
struct Command { byte op; byte data[4]; };

PixelState pixels[NUM_LEDS];
LightState lights = { false, 255, 255, 255, 100 };
Command program[MAX_CMDS];

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

void setup() {
  Serial.begin(115200); 

  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait < 3000)) {
    delay(10);
  }
  
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100); 
  
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
                        BLECharacteristic::PROPERTY_WRITE
                      );

  pRxCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println(">>> BLE Wireless Ready!");

  strip.show();

  loadProgramFromEEPROM();
  if (programLength > 0) {
    runningFromEEPROM = true;
    Serial.printf("Loaded %d steps from External EEPROM\n", programLength);
  }
}

void loop() {
  while (Serial.available() > 0) {
    handleIncomingByte(Serial.read());
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
  if (extEEPROM_read(0) != (byte)programLength) needsUpdate = true;
  if (!needsUpdate) {
    for (int i = 0; i < programLength; i++) {
      int a = 1 + i * CMD_SIZE;
      if (extEEPROM_read(a) != program[i].op) { needsUpdate = true; break; }
    }
  }
  if (needsUpdate) {
    extEEPROM_write(0, (byte)programLength);
    for (int i = 0; i < programLength; i++) {
      int a = 1 + i * CMD_SIZE;
      extEEPROM_write(a, program[i].op);
      for (int j = 0; j < 4; j++) extEEPROM_write(a + 1 + j, program[i].data[j]);
    }
    extEEPROM_write(1 + programLength * CMD_SIZE, (byte)loopStartIndex);
    extEEPROM_write(2 + programLength * CMD_SIZE, (byte)loopProgram);
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
  if (b == 0xFF) {
    saveProgramToEEPROM();
    receivingProgram = false;
    runningFromEEPROM = true;
    currentCmd = 0;
    packetIndex = 0;
    Serial.println(">>> End of Program Received");
    return;
  }

  if (!receivingProgram) {
    programLength = 0;
    receivingProgram = true;
    runningFromEEPROM = false;
    packetIndex = 0;
  }

  packetBuffer[packetIndex++] = b;

  if (packetIndex == 5) {
    byte op = packetBuffer[0];
    byte d1 = packetBuffer[1];
    byte d2 = packetBuffer[2];
    byte d3 = packetBuffer[3];
    byte d4 = packetBuffer[4];

    if (op == 0x00) { storeCommand(op, d1, d2, 0, 0); controlDevice(d1, d2); }
    else if (op == 0x02) { storeCommand(op, d1, d2, d3, d4); }
    else if (op == 0x03) { loopProgram = true; loopStartIndex = programLength; storeCommand(op, 0, 0, 0, 0); }
    else if (op == OP_SET_LED_COLOR) { storeCommand(op, d1, d2, d3, 0); }
    else if (op == OP_SET_LED_BRIGHTNESS) { storeCommand(op, d1, 0, 0, 0); }
    else if (op == OP_SET_LED_PIXEL_COLOR) { storeCommand(op, d1, d2, d3, d4); }
    else if (op == OP_SET_LED_PIXEL_BRIGHTNESS) { storeCommand(op, d1, d2, 0, 0); }
    else if (op == OP_SET_LED_PIXEL_ONOFF) { storeCommand(op, d1, d2, 0, 0); }
    else if (op == OP_CLEAR_PIXELS) { storeCommand(op, 0, 0, 0, 0); }
    else if (op == OP_REPEAT_N_TIMES) { storeCommand(op, d1, 0, 0, 0); }
    else if (op == OP_LOOP_END) { storeCommand(op, 0, 0, 0, 0); }

    packetIndex = 0; 
  }
}