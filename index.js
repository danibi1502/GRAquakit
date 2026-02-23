// const { app, BrowserWindow } = require('electron');
// const path = require('path');

// function createWindow () {
//   const win = new BrowserWindow({
//     width: 500,
//     height: 400,
//     webPreferences: {
//       preload: path.join(__dirname, 'renderer.js'),
//       nodeIntegration: true,
//       contextIsolation: false,
//     }
//   });

//   win.loadFile('index.html');
// }

// app.whenReady().then(createWindow);

// app.on('window-all-closed', () => {
//   if (process.platform !== 'darwin') {
//     app.quit();
//   }
// });

const { app, BrowserWindow } = require('electron');
const path = require('path');

function createWindow() {
  const win = new BrowserWindow({
    width: 1000, // Increased width to fit Blockly comfortably
    height: 800,
    webPreferences: {
      nodeIntegration: true,
      contextIsolation: false,
    }
  });

  // --- SERIAL PORT PERMISSIONS HANDLERS ---

  // 1. Grant permission for the serial API
  win.webContents.session.setPermissionCheckHandler((webContents, permission) => {
    if (permission === 'serial') return true;
    return false;
  });

  win.webContents.session.setDevicePermissionHandler((details) => {
    if (details.deviceType === 'serial') return true;
    return false;
  });

  // 2. Automatically select the port (Fixes the "No port selected" error)
  win.webContents.session.on('select-serial-port', (event, portList, webContents, callback) => {
    event.preventDefault();
    if (portList && portList.length > 0) {
      // Picks the first available device (ESP32/Arduino) automatically
      callback(portList[0].portId);
    } else {
      callback(''); // No device found
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