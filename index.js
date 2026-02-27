const { app, BrowserWindow } = require('electron');
const path = require('path');

// These switches are vital for Web Bluetooth to work in Electron's browser engine
app.commandLine.appendSwitch('enable-experimental-web-platform-features');
app.commandLine.appendSwitch('enable-web-bluetooth');

function createWindow() {
  const win = new BrowserWindow({
    width: 1000, 
    height: 800,
    webPreferences: {
      nodeIntegration: true,
      contextIsolation: false,
    }
  });

  // --- SERIAL PORT PERMISSIONS HANDLERS ---
  win.webContents.session.setPermissionCheckHandler((webContents, permission) => {
    // Grant both serial and bluetooth permissions
    if (permission === 'serial' || permission === 'bluetooth') return true;
    return false;
  });

  win.webContents.session.setDevicePermissionHandler((details) => {
    if (details.deviceType === 'serial' || details.deviceType === 'bluetooth') return true;
    return false;
  });

  // Automatically select the Serial port
  win.webContents.session.on('select-serial-port', (event, portList, webContents, callback) => {
    event.preventDefault();
    if (portList && portList.length > 0) {
      callback(portList[0].portId);
    } else {
      callback('');
    }
  });

  // --- BLUETOOTH DEVICE PICKER (Move to webContents) ---
  win.webContents.on('select-bluetooth-device', (event, deviceList, callback) => {
    event.preventDefault(); // Stop Electron from looking for a default UI
    
    console.log('Scanning... Found these devices:', deviceList.map(d => d.deviceName));

    // Look for our AquaKit
    const result = deviceList.find((device) => {
      return device.deviceName.includes('AquaKit');
    });

    if (result) {
      console.log('✅ AquaKit found! Connecting with ID:', result.deviceId);
      callback(result.deviceId);
    } else {
      // If not found in this batch, do nothing. 
      // Electron will call this event again as more devices appear.
      // DO NOT call callback('') or it will cancel the scan.
    }
  });

  win.loadFile('index.html');
}

app.whenReady().then(createWindow);

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit();
  }
});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createWindow();
  }
});