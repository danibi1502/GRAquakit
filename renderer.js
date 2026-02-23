const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

let port;

document.addEventListener('DOMContentLoaded', () => {
  // --- Pump Elements ---
  const filterOnBtn = document.getElementById('filterOnBtn');
  const filterOffBtn = document.getElementById('filterOffBtn');
  const startFilterScheduleBtn = document.getElementById('startFilterScheduleBtn');
  const stopFilterScheduleBtn = document.getElementById('stopFilterScheduleBtn');
  const filterDuration = document.getElementById('filterDuration');
  const filterInterval = document.getElementById('filterInterval');
  const filterUnit = document.getElementById('filterUnit');

  const airOnBtn = document.getElementById('airOnBtn');
  const airOffBtn = document.getElementById('airOffBtn');
  const startAirScheduleBtn = document.getElementById('startAirScheduleBtn');
  const stopAirScheduleBtn = document.getElementById('stopAirScheduleBtn');
  const airDuration = document.getElementById('airDuration');
  const airInterval = document.getElementById('airInterval');
  const airUnit = document.getElementById('airUnit');

  // --- LED Elements ---
  const ledOnBtn = document.getElementById('ledOnBtn');
  const ledOffBtn = document.getElementById('ledOffBtn');
  const ledBrightness = document.getElementById('ledBrightness');
  const ledColor = document.getElementById('ledColor');
  const ledR = document.getElementById('ledR');
  const ledG = document.getElementById('ledG');
  const ledB = document.getElementById('ledB');
  const ledIndices = document.getElementById('ledIndices');
  const ledStartTime = document.getElementById('ledStartTime');
  const ledEndTime = document.getElementById('ledEndTime');
  const startLedScheduleBtn = document.getElementById('startLedScheduleBtn');
  const stopLedScheduleBtn = document.getElementById('stopLedScheduleBtn');
  const manualLedSetBtn = document.getElementById('manualLedSetBtn'); // <--- NEW
  const manualLedR = document.getElementById('manualLedR');
  const manualLedG = document.getElementById('manualLedG');
  const manualLedB = document.getElementById('manualLedB');
  const manualLedIndices = document.getElementById('manualLedIndices');

  // Disable all buttons until port is ready
  const allButtons = [
    filterOnBtn, filterOffBtn, startFilterScheduleBtn, stopFilterScheduleBtn,
    airOnBtn, airOffBtn, startAirScheduleBtn, stopAirScheduleBtn,
    ledOnBtn, ledOffBtn, startLedScheduleBtn, stopLedScheduleBtn,
    manualLedSetBtn // <--- include new button here
  ];
  allButtons.forEach(btn => btn.disabled = true);

  // --- LED Controls ---
  ledOnBtn.addEventListener('click', () => {
    sendLedCommand('LED_ON');
  });

  ledOffBtn.addEventListener('click', () => {
    sendLedCommand('LED_OFF');
  });

  ledBrightness.addEventListener('input', () => {
    sendLedCommand(`LED_BRIGHTNESS:${ledBrightness.value}`);
  });

  // Helper: convert hex (#rrggbb) to [r, g, b]
  function hexToRgb(hex) {
    const bigint = parseInt(hex.slice(1), 16);
    const r = (bigint >> 16) & 255;
    const g = (bigint >> 8) & 255;
    const b = bigint & 255;
    return [r, g, b];
  }

  ledColor.addEventListener('input', () => {
    const color = ledColor.value; // hex format
    sendLedCommand(`LED_COLOR_HEX:${color}`);

    // Convert to RGB and sync both sets of inputs
    const [r, g, b] = hexToRgb(color);
    ledR.value = r;
    ledG.value = g;
    ledB.value = b;

    manualLedR.value = r;
    manualLedG.value = g;
    manualLedB.value = b;
  });

  [ledR, ledG, ledB].forEach(input => {
    input.addEventListener('input', () => {
      sendLedCommand(`LED_COLOR_RGB:${ledR.value},${ledG.value},${ledB.value}`);
    });
  });

  startLedScheduleBtn.addEventListener('click', () => {
    const command = `LED_SCHEDULE:${ledStartTime.value}:${ledEndTime.value}:${ledBrightness.value}:${ledColor.value}:${ledIndices.value}`;
    sendLedCommand(command);
  });

  stopLedScheduleBtn.addEventListener('click', () => {
    sendLedCommand('STOP_LED_SCHEDULE');
  });

  // --- NEW: Manual LED Set ---
  manualLedSetBtn.addEventListener('click', () => {
    const r = parseInt(manualLedR.value) || 0;
    const g = parseInt(manualLedG.value) || 0;
    const b = parseInt(manualLedB.value) || 0;
    const indices = manualLedIndices.value.trim() || 'all';
    setSpecificLeds(r, g, b, indices);
  });

  function sendLedCommand(cmd) {
    if (port && port.isOpen) {
      port.write(cmd + '\n');
      console.log('Sent:', cmd);
    }
  }

  function setSpecificLeds(r, g, b, indices) {
    const cmd = `LED_SET:${r},${g},${b}:${indices}`;
    if (port && port.isOpen) {
      port.write(cmd + '\n');
      console.log('Sent:', cmd);
    }
  }

  // --- Pump Controls ---
  filterOnBtn.addEventListener('click', () => { if (port.isOpen) port.write('FILTER_PUMP_ON\n'); });
  filterOffBtn.addEventListener('click', () => { if (port.isOpen) port.write('FILTER_PUMP_OFF\n'); });
  startFilterScheduleBtn.addEventListener('click', () => {
    const cmd = `FILTER_SCHEDULE:${filterDuration.value}:${filterInterval.value}:${filterUnit.value}`;
    port.write(cmd + '\n'); console.log('Sent:', cmd);
  });
  stopFilterScheduleBtn.addEventListener('click', () => { if (port.isOpen) port.write('STOP_FILTER_SCHEDULE\n'); });

  airOnBtn.addEventListener('click', () => { if (port.isOpen) port.write('AIR_PUMP_ON\n'); });
  airOffBtn.addEventListener('click', () => { if (port.isOpen) port.write('AIR_PUMP_OFF\n'); });
  startAirScheduleBtn.addEventListener('click', () => {
    const cmd = `AIR_SCHEDULE:${airDuration.value}:${airInterval.value}:${airUnit.value}`;
    port.write(cmd + '\n'); console.log('Sent:', cmd);
  });
  stopAirScheduleBtn.addEventListener('click', () => { if (port.isOpen) port.write('STOP_AIR_SCHEDULE\n'); });

  // --- Serial Port Detection ---
  detectDevicePort()
    .then((portPath) => {
      console.log('Device detected on port:', portPath);
      port = new SerialPort({ path: portPath, baudRate: 9600 });
      const parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));

      parser.on('data', (data) => console.log('Received from device:', data.trim()));

      port.on('open', () => {
        console.log('Serial port opened successfully');
        allButtons.forEach(btn => btn.disabled = false);
      });

      port.on('error', (err) => console.error('Serial port error:', err.message));
    })
    .catch((err) => console.error('Device port not found:', err));
});

async function detectDevicePort() {
  const ports = await SerialPort.list();

  const devicePort = ports.find((p) => {
    const name = `${p.manufacturer || ''} ${p.path || ''} ${p.friendlyName || ''}`.toLowerCase();

    return (
      name.includes('arduino') ||
      name.includes('wch') ||
      name.includes('ch340') ||
      name.includes('usb-serial') ||
      name.includes('1a86') || // CH340 vendor ID
      name.includes('stmicroelectronics')
    );
  });

  if (!devicePort) {
    console.error('Available ports:', ports);
    throw new Error('No compatible device found');
  }

  console.log('✅ Found device on:', devicePort.path);
  return devicePort.path;
}
