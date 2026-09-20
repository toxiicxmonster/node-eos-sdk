#!/usr/bin/env node
'use strict';

/**
 * Runs the unit suite on any supported Node version.
 *
 * `node --test` has changed its mind more than once about what a positional
 * argument means -- a directory, a glob, a literal file path -- and the forms
 * it accepts differ by release. An unquoted shell glob in the npm script would
 * paper over that on Unix and fail on Windows, where cmd does not expand globs.
 *
 * Explicit file paths are the one form every version has always accepted, so
 * this finds them and passes them through. Fifteen lines is cheaper than
 * re-learning the argument rules on each Node upgrade.
 */

const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const unitDir = path.join(__dirname, '..', 'test', 'unit');

function findTests(directory) {
  const found = [];
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const full = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      found.push(...findTests(full));
    } else if (/\.test\.(?:c|m)?js$/.test(entry.name)) {
      found.push(full);
    }
  }
  return found;
}

if (!fs.existsSync(unitDir)) {
  console.error(`No unit test directory at ${unitDir}`);
  process.exit(1);
}

const files = findTests(unitDir).sort();
if (files.length === 0) {
  // An empty run reports success, which would quietly turn CI into a no-op.
  console.error(`No *.test.js files found under ${unitDir}`);
  process.exit(1);
}

const result = spawnSync(process.execPath, ['--test', ...files], {
  stdio: 'inherit',
});

if (result.error) {
  console.error(result.error.message);
  process.exit(1);
}
process.exit(result.status === null ? 1 : result.status);
