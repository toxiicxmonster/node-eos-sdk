#!/usr/bin/env node
'use strict';

/**
 * M3 + M4 loopback test: two processes on one machine, one lobby, a thousand
 * packets, checked for order and loss.
 *
 *   node test/p2p-loopback.js
 *
 * Run with no argument it orchestrates; it re-spawns itself as `host` and
 * `peer`. This catches API misuse, not networking -- both ends are behind the
 * same NAT. The tests that matter for NAT traversal need two machines on
 * different networks; see docs/testing.md.
 *
 * Note on identity: both processes log in with a device-id account, pointed at
 * separate cache directories so that they get separate Product User IDs. If
 * your SDK version hands both processes the same PUID, the test says so
 * explicitly rather than failing somewhere confusing -- you then need two real
 * accounts (one Steam, one Epic) to run it.
 */

const { spawn } = require('node:child_process');
const os = require('node:os');
const path = require('node:path');
const fs = require('node:fs');

const PACKET_COUNT = 1000;
const SOCKET_NAME = 'loopback';
const TIMEOUT_MS = 120_000;

const role = process.argv[2];

// ---------------------------------------------------------------------------
// Orchestrator
// ---------------------------------------------------------------------------

function runChild(childRole, args, onLine) {
  const child = spawn(
    process.execPath,
    [__filename, childRole, ...args],
    { stdio: ['ignore', 'pipe', 'inherit'] },
  );

  let buffer = '';
  child.stdout.on('data', (chunk) => {
    buffer += chunk.toString();
    let index;
    while ((index = buffer.indexOf('\n')) >= 0) {
      const line = buffer.slice(0, index).trimEnd();
      buffer = buffer.slice(index + 1);
      console.log(`[${childRole}] ${line}`);
      onLine(line, child);
    }
  });
  return child;
}

function orchestrate() {
  const timer = setTimeout(() => {
    console.error(`FAIL: loopback exceeded ${TIMEOUT_MS} ms`);
    process.exit(1);
  }, TIMEOUT_MS);

  let peer = null;
  const exits = [];

  const host = runChild('host', [], (line) => {
    // The host announces the lobby id; only then can the peer join it.
    if (line.startsWith('LOBBY:') && peer === null) {
      const lobbyId = line.slice('LOBBY:'.length);
      peer = runChild('peer', [lobbyId], () => {});
      peer.on('exit', (code) => finish('peer', code));
    }
  });
  host.on('exit', (code) => finish('host', code));

  function finish(which, code) {
    exits.push({ which, code });
    if (exits.length < 2) return;
    clearTimeout(timer);
    const failed = exits.filter((e) => e.code !== 0);
    if (failed.length > 0) {
      console.error(
        `FAIL: ${failed.map((e) => `${e.which} exited ${e.code}`).join(', ')}`,
      );
      process.exit(1);
    }
    console.log(`PASS: ${PACKET_COUNT} packets, in order, none lost`);
    process.exit(0);
  }
}

// ---------------------------------------------------------------------------
// Shared child setup
// ---------------------------------------------------------------------------

async function startClient(cacheName, displayName) {
  const eos = require('../index.js');
  const { credentials } = require('./credentials.js');

  // Separate cache directories are what give the two processes separate
  // device-id accounts, and therefore separate PUIDs.
  const cacheDirectory = path.join(
    os.tmpdir(),
    `node-eos-sdk-loopback-${cacheName}`,
  );
  fs.mkdirSync(cacheDirectory, { recursive: true });

  eos.init(credentials({ cacheDirectory }));
  eos.on('error', (error) => {
    console.error('error:', error.message);
    process.exit(1);
  });

  await eos.connect.createDeviceId(`loopback-${cacheName}`);
  const productUserId = await eos.connect.login({
    type: 'DEVICE_ID_ACCESS_TOKEN',
    displayName,
  });
  console.log(`puid ${productUserId}`);

  eos.p2p.configure({ localUserId: productUserId, socketName: SOCKET_NAME });
  return { eos, productUserId };
}

function fail(message) {
  console.error(message);
  process.exit(1);
}

// ---------------------------------------------------------------------------
// Host: create the lobby, receive and verify
// ---------------------------------------------------------------------------

async function runHost() {
  const { eos, productUserId } = await startClient('host', 'loopback-host');

  const lobby = await eos.lobby.create({
    localUserId: productUserId,
    maxMembers: 2,
    bucketId: 'loopback',
    permissionLevel: 'publicAdvertised',
  });
  await lobby.setData({ test: 'p2p-loopback', packets: PACKET_COUNT });

  // The peer reads the lobby to find this PUID, so the id must be published
  // only once the lobby is actually joinable.
  console.log(`LOBBY:${lobby.id}`);

  let expected = 0;
  eos.on('p2p:packet', ({ peerId, data }) => {
    const sequence = data.readUInt32LE(0);
    if (sequence !== expected) {
      fail(`out of order: expected ${expected}, got ${sequence}`);
    }
    expected += 1;

    if (expected === PACKET_COUNT) {
      console.log(`received ${PACKET_COUNT} packets in order`);
      // Tell the peer it can stop; otherwise it waits out the timeout.
      eos.p2p.send({ remoteUserId: peerId, data: Buffer.from('DONE') });
      setTimeout(async () => {
        await lobby.leave();
        eos.shutdown();
        process.exit(0);
      }, 500);
    }
  });

  eos.on('p2p:connected', ({ remoteUserId, connectionType }) => {
    console.log(`peer connected: ${remoteUserId} (type ${connectionType})`);
  });
}

// ---------------------------------------------------------------------------
// Peer: join the lobby, send
// ---------------------------------------------------------------------------

async function runPeer(lobbyId) {
  const { eos, productUserId } = await startClient('peer', 'loopback-peer');

  const lobby = await eos.lobby.join({ localUserId: productUserId, lobbyId });
  const members = lobby.getMembers();
  console.log(`joined ${lobby.id}, ${members.length} members`);

  const hostId = lobby.getOwner();
  if (!hostId) fail('lobby has no owner');
  if (hostId === productUserId) {
    fail(
      'host and peer share a Product User ID. Both processes resolved to the ' +
        'same device-id account, so there is nothing to send between. Run the ' +
        'two ends with distinct accounts instead.',
    );
  }
  if (!members.includes(hostId) || !members.includes(productUserId)) {
    fail(`membership wrong: ${members.join(', ')}`);
  }

  eos.on('p2p:packet', ({ data }) => {
    if (data.toString() === 'DONE') {
      console.log('host acknowledged');
      lobby.leave().then(() => {
        eos.shutdown();
        process.exit(0);
      });
    }
  });

  // Reliable-ordered, so this is a correctness test of ordering as much as of
  // delivery. Send flat out: the send queue is the SDK's problem, not ours.
  for (let sequence = 0; sequence < PACKET_COUNT; sequence += 1) {
    const payload = Buffer.alloc(16);
    payload.writeUInt32LE(sequence, 0);
    eos.p2p.send({ remoteUserId: hostId, data: payload });
  }
  console.log(`sent ${PACKET_COUNT} packets`);
}

// ---------------------------------------------------------------------------

const entry =
  role === 'host'
    ? runHost()
    : role === 'peer'
      ? runPeer(process.argv[3])
      : Promise.resolve(orchestrate());

entry.catch((error) => {
  console.error('FAIL:', error.message);
  if (error.code) console.error('  EOS result:', error.code);
  process.exit(1);
});
