'use strict';

/**
 * Loads deployment credentials for the integration tests, from the environment
 * or from a .env file at the repository root.
 *
 * Deliberately dependency-free: a test helper is not worth a dotenv install,
 * and the parsing needed here is three lines.
 *
 * Never commit a .env -- .gitignore excludes it. The client secret in
 * particular is a credential, not configuration.
 */

const fs = require('node:fs');
const path = require('node:path');

const REQUIRED = [
  'EOS_PRODUCT_ID',
  'EOS_SANDBOX_ID',
  'EOS_DEPLOYMENT_ID',
  'EOS_CLIENT_ID',
  'EOS_CLIENT_SECRET',
  'EOS_ENCRYPTION_KEY',
];

function loadDotEnv() {
  const file = path.join(__dirname, '..', '.env');
  if (!fs.existsSync(file)) return;

  for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const match = /^\s*([A-Z0-9_]+)\s*=\s*(.*)$/.exec(line);
    if (!match) continue;
    const value = match[2].trim().replace(/^["']|["']$/g, '');
    if (process.env[match[1]] === undefined) process.env[match[1]] = value;
  }
}

/**
 * @returns {import('../index.js').InitOptions}
 * @throws if any credential is missing, naming all of them at once.
 */
function credentials(overrides = {}) {
  loadDotEnv();

  const missing = REQUIRED.filter((key) => !process.env[key]);
  if (missing.length > 0) {
    throw new Error(
      `Missing EOS credentials: ${missing.join(', ')}.\n` +
        'Copy .env.example to .env and fill in the five values from the Epic ' +
        'Developer Portal, or set them in the environment. See ' +
        'docs/sdk-setup.md.',
    );
  }

  return {
    productId: process.env.EOS_PRODUCT_ID,
    sandboxId: process.env.EOS_SANDBOX_ID,
    deploymentId: process.env.EOS_DEPLOYMENT_ID,
    clientId: process.env.EOS_CLIENT_ID,
    clientSecret: process.env.EOS_CLIENT_SECRET,
    encryptionKey: process.env.EOS_ENCRYPTION_KEY,
    debug: process.env.EOS_DEBUG === '1',
    ...overrides,
  };
}

module.exports = { credentials, REQUIRED };
