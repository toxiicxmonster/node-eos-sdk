'use strict';

/**
 * The smallest useful program: initialise, log in, print a Product User ID,
 * shut down.
 *
 *   node examples/basic-login.js
 *
 * Uses a device-id account, which needs no store and no external credential.
 * That makes it the right first thing to run: if this works, your Developer
 * Portal client policy permits Connect login, which is the single most common
 * thing to have wrong.
 */

const eos = require('node-eos-sdk');

async function main() {
  eos.on('log', (entry) => console.log(`[${entry.category}] ${entry.message}`));

  eos.init({
    productId: process.env.EOS_PRODUCT_ID,
    sandboxId: process.env.EOS_SANDBOX_ID,
    deploymentId: process.env.EOS_DEPLOYMENT_ID,
    clientId: process.env.EOS_CLIENT_ID,
    clientSecret: process.env.EOS_CLIENT_SECRET,
    encryptionKey: process.env.EOS_ENCRYPTION_KEY,
    debug: true,
  });

  await eos.connect.createDeviceId('example');
  const productUserId = await eos.connect.login({
    type: eos.CredentialType.DEVICE_ID_ACCESS_TOKEN,
    displayName: 'example-player',
  });

  console.log('Product User ID:', productUserId);

  // Nothing in EOS progresses without ticking, and init() owns the tick
  // interval. That interval also keeps the process alive, so shutdown() is how
  // this program ends.
  eos.shutdown();
}

main().catch((error) => {
  console.error(error.message);
  if (error.code) console.error('EOS result:', error.code);
  process.exit(1);
});
