'use strict';

/**
 * Electron integration (M5).
 *
 * The addon lives in the MAIN process only. The renderer reaches it through a
 * narrow contextBridge -- see electron-preload.js. Do not enable
 * nodeIntegration to shortcut this: a renderer that can require() native code
 * is a renderer that can be turned into one by any script it loads.
 *
 * Payloads cross as JSON over IPC. For a lockstep game they are tiny -- a
 * two-minute four-player match is on the order of 10 KB of commands total --
 * so there is no case for a binary channel or shared memory.
 *
 * Packaging matters here too: electron-builder must ship the EOS shared library
 * unpacked. A native library inside an asar archive cannot be dlopen'd, and
 * that failure appears only in the packaged build. See docs/packaging.md.
 */

const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('node:path');
const eos = require('node-eos-sdk');

let window = null;
let localUserId = null;
let lobby = null;

/** Forward an EOS event to the renderer. */
function forward(event, payload) {
  if (window && !window.isDestroyed()) {
    window.webContents.send('net:event', { event, payload });
  }
}

function wireEvents() {
  for (const event of [
    'lobby:updated',
    'lobby:member-updated',
    'lobby:member-status',
    'lobby:invite-accepted',
    'p2p:connected',
    'p2p:disconnected',
  ]) {
    eos.on(event, (payload) => forward(event, payload));
  }

  // Packets carry Buffers, which do not survive structured cloning to the
  // renderer as usefully as a plain string does. Decode at the boundary.
  eos.on('p2p:packet', ({ peerId, data }) => {
    forward('p2p:packet', { peerId, payload: JSON.parse(data.toString()) });
  });

  eos.on('error', (error) => forward('error', { message: error.message }));
}

function registerHandlers() {
  ipcMain.handle('net:login', async (_event, credentials) => {
    localUserId = await eos.connect.login(credentials);
    eos.p2p.configure({ localUserId, socketName: 'frontier' });
    return localUserId;
  });

  ipcMain.handle('net:createLobby', async (_event, options) => {
    lobby = await eos.lobby.create({ localUserId, ...options });
    return { lobbyId: lobby.id, members: lobby.getMembers() };
  });

  ipcMain.handle('net:joinLobby', async (_event, lobbyId) => {
    lobby = await eos.lobby.join({ localUserId, lobbyId });
    return { lobbyId: lobby.id, members: lobby.getMembers() };
  });

  ipcMain.handle('net:getMembers', () => (lobby ? lobby.getMembers() : []));
  ipcMain.handle('net:getSetup', () => (lobby ? lobby.getFullData() : {}));
  ipcMain.handle('net:setSetup', (_event, attributes) =>
    lobby ? lobby.setData(attributes) : undefined,
  );
  ipcMain.handle('net:setMemberState', (_event, attributes) =>
    lobby ? lobby.setMemberData(attributes) : undefined,
  );

  ipcMain.handle('net:send', (_event, { peerId, payload }) => {
    eos.p2p.send({
      remoteUserId: peerId,
      data: Buffer.from(JSON.stringify(payload)),
    });
  });

  ipcMain.handle('net:leave', async () => {
    if (lobby) await lobby.leave();
    lobby = null;
  });
}

app.whenReady().then(() => {
  eos.init({
    productId: process.env.EOS_PRODUCT_ID,
    sandboxId: process.env.EOS_SANDBOX_ID,
    deploymentId: process.env.EOS_DEPLOYMENT_ID,
    clientId: process.env.EOS_CLIENT_ID,
    clientSecret: process.env.EOS_CLIENT_SECRET,
    encryptionKey: process.env.EOS_ENCRYPTION_KEY,
    debug: !app.isPackaged,
  });

  wireEvents();
  registerHandlers();

  window = new BrowserWindow({
    width: 1280,
    height: 720,
    webPreferences: {
      preload: path.join(__dirname, 'electron-preload.js'),
      nodeIntegration: false,
      contextIsolation: true,
    },
  });
  window.loadFile('index.html');
});

// EOS cannot be re-initialised after shutdown in the same process, so this is
// the last thing that happens.
app.on('will-quit', () => {
  eos.shutdown();
});
