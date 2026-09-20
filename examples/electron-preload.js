'use strict';

/**
 * The renderer's whole view of the network.
 *
 * Everything here is an explicit, named call. The renderer never sees the
 * addon, an ipcRenderer handle, or anything it could use to reach something
 * that was not deliberately exposed -- which is the entire reason for keeping
 * contextIsolation on.
 */

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('net', {
  login: (credentials) => ipcRenderer.invoke('net:login', credentials),

  createLobby: (options) => ipcRenderer.invoke('net:createLobby', options),
  joinLobby: (lobbyId) => ipcRenderer.invoke('net:joinLobby', lobbyId),
  leave: () => ipcRenderer.invoke('net:leave'),

  getMembers: () => ipcRenderer.invoke('net:getMembers'),
  getSetup: () => ipcRenderer.invoke('net:getSetup'),
  setSetup: (attributes) => ipcRenderer.invoke('net:setSetup', attributes),
  setMemberState: (attributes) =>
    ipcRenderer.invoke('net:setMemberState', attributes),

  send: (peerId, payload) => ipcRenderer.invoke('net:send', { peerId, payload }),

  /**
   * Subscribe to network events. Returns an unsubscribe function, so a UI that
   * mounts and unmounts does not accumulate listeners.
   */
  on: (handler) => {
    const listener = (_event, message) => handler(message.event, message.payload);
    ipcRenderer.on('net:event', listener);
    return () => ipcRenderer.removeListener('net:event', listener);
  },
});
