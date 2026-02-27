const { contextBridge } = require('electron');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

contextBridge.exposeInMainWorld('api', {
  SerialPort,
  ReadlineParser
});