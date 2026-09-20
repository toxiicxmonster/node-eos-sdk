#!/usr/bin/env node
'use strict';

/**
 * M1 + M2 smoke test: initialise, tick, log in, print a Product User ID, shut
 * down cleanly.
 *
 * Requires a real deployment -- see test/credentials.js. Run it before
 * anything else: a Developer Portal client policy that does not permit Connect
 * login fails here, and it fails in a way that looks exactly like a code bug if
 * you discover it halfway through the Lobby work.
 *
 *   node test/smoke.js
 */

const eos = require('../index.js');
const { credentials } = require('./credentials.js');

const TIMEOUT_MS = 30_000;

async function main() {
  const options = credentials();

  eos.on('log', (entry) => {
    console.log(`[eos:${entry.category}] ${entry.message}`);
  });
  eos.on('error', (error) => {
    console.error('[eos:error]', error);
  });

  console.log('init...');
  eos.init(options);
  console.log('ok: platform created, ticking at 20 Hz');

  // Device-id login needs no store and no external account, so the smoke test
  // stays runnable in CI. A Steam or Epic login is the same call with a
  // different credential type.
  console.log('creating device id...');
  await eos.connect.createDeviceId('node-eos-sdk-smoke');

  console.log('logging in...');
  const productUserId = await eos.connect.login({
    type: 'DEVICE_ID_ACCESS_TOKEN',
    displayName: 'smoke-test',
  });

  if (!productUserId) throw new Error('login resolved without a PUID');
  console.log(`ok: product user id ${productUserId}`);

  const users = eos.connect.getLoggedInUsers();
  console.log(`ok: ${users.length} logged-in user(s)`);

  // Tick for a few seconds with nothing to do. The point is that it survives
  // it: no crash, no leak, clean exit.
  await new Promise((resolve) => setTimeout(resolve, 3000));

  console.log('shutting down...');
  eos.shutdown();
  console.log('PASS');
}

const timer = setTimeout(() => {
  console.error(`FAIL: smoke test exceeded ${TIMEOUT_MS} ms`);
  process.exit(1);
}, TIMEOUT_MS);
timer.unref();

main().catch((error) => {
  console.error('FAIL:', error.message);
  if (error.code) console.error('  EOS result:', error.code);
  try {
    eos.shutdown();
  } catch {
    // Already down, or never came up. Either way the exit code is what matters.
  }
  process.exit(1);
});
