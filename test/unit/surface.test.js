'use strict';

/**
 * Tests that run with no SDK, no credentials and no compiler -- which is what
 * CI has on a pull request from a fork, and what a contributor has before they
 * have registered an Epic product.
 *
 * They cover the JS surface only. Anything that touches the network lives in
 * test/smoke.js and test/p2p-loopback.js.
 */

const assert = require('node:assert/strict');
const { test } = require('node:test');
const { EventEmitter } = require('node:events');

const eos = require('../../index.js');

test('requiring the package does not load the native addon', () => {
  // Loading is lazy on purpose: the module must be requirable for tooling,
  // type generation and unit tests on a machine with no SDK.
  assert.ok(eos instanceof EventEmitter);
  assert.equal(typeof eos.init, 'function');
  assert.equal(typeof eos.shutdown, 'function');
});

test('namespaces expose the documented surface', () => {
  assert.deepEqual(Object.keys(eos.connect).sort(), [
    'createDeviceId',
    'getLoggedInUsers',
    'login',
  ]);
  assert.deepEqual(Object.keys(eos.lobby).sort(), ['attach', 'create', 'join']);
  assert.deepEqual(Object.keys(eos.p2p).sort(), [
    'acceptConnection',
    'closeConnection',
    'configure',
    'send',
  ]);
});

test('constants match the SDK values they mirror', () => {
  assert.equal(eos.MAX_PACKET_SIZE, 1170);
  assert.equal(eos.PacketReliability.RELIABLE_ORDERED, 'reliableOrdered');
  assert.equal(eos.LobbyPermissionLevel.INVITE_ONLY, 'inviteOnly');
  assert.equal(eos.CredentialType.STEAM_SESSION_TICKET, 'STEAM_SESSION_TICKET');
  assert.equal(eos.LogLevel.VERBOSE, 500);

  // STEAM_APP_TICKET is deprecated by Epic and deliberately not offered.
  assert.equal(eos.CredentialType.STEAM_APP_TICKET, undefined);
});

test('constants are frozen', () => {
  assert.ok(Object.isFrozen(eos.CredentialType));
  assert.ok(Object.isFrozen(eos.PacketReliability));
  assert.ok(Object.isFrozen(eos.LogLevel));
});

test('a missing addon fails with an actionable error, not a stack trace', () => {
  // On a machine where the addon did build, init() rejects the empty options
  // instead. Either way the contract is: throw, do not half-initialise.
  assert.throws(
    () => eos.init({}),
    (error) => {
      if (error.code === 'EOS_ADDON_NOT_LOADED') {
        assert.match(error.message, /vendor\/eos/);
        assert.match(error.message, /docs\/sdk-setup\.md/);
        return true;
      }
      // Built addon: the options validation is what fired.
      assert.match(error.message, /productId|encryptionKey/);
      return true;
    },
  );
});

test('Lobby mirrors the steamworks.js matchmaking.Lobby surface', () => {
  const { Lobby } = require('../../index.js');
  assert.equal(typeof Lobby, 'function');

  // A lobby UI written against steamworks.js calls these names. Renaming one
  // breaks that compatibility silently, so it is pinned here.
  for (const method of [
    'getMembers',
    'getOwner',
    'getInfo',
    'getFullData',
    'getData',
    'setData',
    'getMemberData',
    'setMemberData',
    'sendInvite',
    'leave',
  ]) {
    assert.equal(
      typeof Lobby.prototype[method],
      'function',
      `Lobby.prototype.${method} is missing`,
    );
  }

  const lobby = new Lobby(eos, 'lobby-1', 'puid-1');
  assert.equal(lobby.id, 'lobby-1');
  assert.equal(lobby.localUserId, 'puid-1');
});
