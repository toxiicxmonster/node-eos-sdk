#!/usr/bin/env node
'use strict';

/**
 * Verifies that the EOS SDK has been unpacked into vendor/eos/ before anyone
 * spends time on a build that cannot succeed.
 *
 * The SDK is not redistributable, so it is not in this repository and never
 * will be. This script is the difference between "you have not downloaded the
 * SDK" and forty lines of node-gyp output about a missing header.
 */

const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, '..');
const vendor = path.join(root, 'vendor', 'eos');

const PLATFORM_LIBRARIES = {
  win32: [
    'lib/win64/EOSSDK-Win64-Shipping.lib',
    'lib/win64/EOSSDK-Win64-Shipping.dll',
  ],
  darwin: ['lib/osx/libEOSSDK-Mac-Shipping.dylib'],
  linux: ['lib/linux/libEOSSDK-Linux-Shipping.so'],
};

const REQUIRED_HEADERS = [
  'include/eos_sdk.h',
  'include/eos_common.h',
  'include/eos_logging.h',
  'include/eos_connect.h',
  'include/eos_lobby.h',
  'include/eos_p2p.h',
];

function exists(relative) {
  return fs.existsSync(path.join(vendor, relative));
}

function main() {
  const libraries = PLATFORM_LIBRARIES[process.platform];
  if (!libraries) {
    console.error(
      `node-eos-sdk: unsupported platform '${process.platform}'. ` +
        'Epic ships the C SDK for Windows, macOS and Linux only.',
    );
    process.exit(1);
  }

  const missing = [...REQUIRED_HEADERS, ...libraries].filter((f) => !exists(f));

  if (missing.length === 0) {
    console.log('node-eos-sdk: EOS SDK found in vendor/eos. Ready to build.');
    return;
  }

  console.error('node-eos-sdk: the EOS SDK is missing from vendor/eos.\n');
  console.error('Missing:');
  for (const file of missing) console.error(`  vendor/eos/${file}`);
  console.error(
    '\nEpic does not permit redistributing the SDK, so this package cannot\n' +
      'ship it. To build:\n\n' +
      '  1. Download the C SDK from https://onlineservices.epicgames.com/sdk\n' +
      '     (the "C SDK" package -- not the Unity or Unreal plugin)\n' +
      '  2. Copy SDK/Include    -> vendor/eos/include\n' +
      '  3. Copy SDK/Bin + Lib  -> vendor/eos/lib/<platform>\n' +
      '  4. Re-run this check, then `npm run build`\n\n' +
      'Full instructions: docs/sdk-setup.md',
  );
  process.exit(1);
}

main();
