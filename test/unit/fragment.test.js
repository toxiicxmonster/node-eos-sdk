'use strict';

/**
 * Fragmentation and reassembly are pure JavaScript, so unlike the rest of the
 * P2P path they can be tested properly with no SDK and no network. Given that
 * the native layer cannot be exercised here, this is the one part of packet
 * handling that gets real coverage -- so it gets thorough coverage.
 */

const assert = require('node:assert/strict');
const { test } = require('node:test');

const {
  fragment,
  Reassembler,
  FRAGMENT_HEADER_BYTES,
  MAX_FRAGMENT_PAYLOAD,
  MAX_PACKET_SIZE,
} = require('../../index.js');

const deliver = (reassembler, peer, pieces) => {
  let result = null;
  for (const piece of pieces) {
    const out = reassembler.accept(peer, piece);
    if (out !== null) result = out;
  }
  return result;
};

test('a fragment never exceeds the EOS wire limit', () => {
  assert.equal(MAX_FRAGMENT_PAYLOAD + FRAGMENT_HEADER_BYTES, MAX_PACKET_SIZE);

  const pieces = fragment(Buffer.alloc(100_000, 0xab), 7);
  for (const piece of pieces) {
    assert.ok(
      piece.length <= MAX_PACKET_SIZE,
      `fragment of ${piece.length} bytes exceeds ${MAX_PACKET_SIZE}`,
    );
  }
});

test('round-trips payloads across the interesting sizes', () => {
  const sizes = [
    0,
    1,
    MAX_FRAGMENT_PAYLOAD - 1,
    MAX_FRAGMENT_PAYLOAD,
    MAX_FRAGMENT_PAYLOAD + 1,
    MAX_FRAGMENT_PAYLOAD * 3,
    MAX_FRAGMENT_PAYLOAD * 3 + 17,
    250_000,
  ];

  for (const size of sizes) {
    const original = Buffer.alloc(size);
    for (let i = 0; i < size; i += 1) original[i] = (i * 31) % 256;

    const reassembler = new Reassembler();
    const rebuilt = deliver(reassembler, 'peer-1', fragment(original, 42));

    assert.notEqual(rebuilt, null, `size ${size} never completed`);
    assert.equal(rebuilt.length, size, `size ${size} wrong length`);
    assert.ok(rebuilt.equals(original), `size ${size} corrupted`);
    assert.equal(reassembler.pendingBytes, 0, `size ${size} leaked`);
  }
});

test('an empty payload is delivered, not dropped', () => {
  const pieces = fragment(Buffer.alloc(0), 1);
  assert.equal(pieces.length, 1);
  const rebuilt = new Reassembler().accept('peer-1', pieces[0]);
  assert.notEqual(rebuilt, null);
  assert.equal(rebuilt.length, 0);
});

test('reassembles out of order', () => {
  const original = Buffer.alloc(MAX_FRAGMENT_PAYLOAD * 4 + 9, 0x5a);
  const pieces = fragment(original, 99);
  assert.ok(pieces.length > 4);

  // reliableOrdered makes in-order arrival likely, not guaranteed -- and the
  // other reliability modes make no promise at all.
  const shuffled = [...pieces].reverse();
  const rebuilt = deliver(new Reassembler(), 'peer-1', shuffled);
  assert.ok(rebuilt.equals(original));
});

test('ignores duplicate fragments', () => {
  const original = Buffer.alloc(MAX_FRAGMENT_PAYLOAD * 2, 0x11);
  const pieces = fragment(original, 5);
  const reassembler = new Reassembler();

  const withDupes = [pieces[0], pieces[0], pieces[0], ...pieces.slice(1)];
  const rebuilt = deliver(reassembler, 'peer-1', withDupes);

  assert.ok(rebuilt.equals(original));
  assert.equal(reassembler.pendingBytes, 0);
});

test('keeps two peers separate', () => {
  const a = Buffer.alloc(MAX_FRAGMENT_PAYLOAD * 2, 0xaa);
  const b = Buffer.alloc(MAX_FRAGMENT_PAYLOAD * 2, 0xbb);

  // Same message id from both peers: the table must key on peer as well, or
  // the two messages interleave into corruption.
  const pa = fragment(a, 1);
  const pb = fragment(b, 1);
  const reassembler = new Reassembler();

  assert.equal(reassembler.accept('peer-a', pa[0]), null);
  assert.equal(reassembler.accept('peer-b', pb[0]), null);
  const outA = reassembler.accept('peer-a', pa[1]);
  const outB = reassembler.accept('peer-b', pb[1]);

  assert.ok(outA.equals(a));
  assert.ok(outB.equals(b));
});

test('a truncated or foreign packet is ignored, not thrown on', () => {
  const reassembler = new Reassembler();
  // A peer speaking a different protocol on this channel must not be able to
  // crash the receiver.
  assert.equal(reassembler.accept('peer-1', Buffer.alloc(0)), null);
  assert.equal(reassembler.accept('peer-1', Buffer.alloc(3)), null);
  assert.equal(reassembler.accept('peer-1', 'not a buffer'), null);

  const bogus = Buffer.alloc(FRAGMENT_HEADER_BYTES + 4);
  bogus.writeUInt16LE(9, 4); // index 9
  bogus.writeUInt16LE(2, 6); // of 2 -- impossible
  assert.equal(reassembler.accept('peer-1', bogus), null);

  const zeroTotal = Buffer.alloc(FRAGMENT_HEADER_BYTES + 4);
  zeroTotal.writeUInt16LE(0, 6);
  assert.equal(reassembler.accept('peer-1', zeroTotal), null);
});

test('a peer cannot hold memory indefinitely', () => {
  const reassembler = new Reassembler({ maxPendingBytes: 64 * 1024, ttlMs: 1000 });

  // Open many large messages and never finish any of them.
  for (let id = 1; id <= 200; id += 1) {
    const pieces = fragment(Buffer.alloc(MAX_FRAGMENT_PAYLOAD * 4, 0x7f), id);
    reassembler.accept('hostile', pieces[0]);
  }

  assert.ok(
    reassembler.pendingBytes <= 64 * 1024,
    `held ${reassembler.pendingBytes} bytes, over the 65536 budget`,
  );
});

test('stale partials are evicted on age', () => {
  const reassembler = new Reassembler({ ttlMs: 1000 });
  const pieces = fragment(Buffer.alloc(MAX_FRAGMENT_PAYLOAD * 2, 1), 1);

  reassembler.accept('peer-1', pieces[0], 0);
  assert.equal(reassembler.pendingMessages, 1);

  // A later fragment from a different message, well past the TTL, sweeps it.
  const other = fragment(Buffer.alloc(MAX_FRAGMENT_PAYLOAD * 2, 2), 2);
  reassembler.accept('peer-1', other[0], 5000);

  assert.equal(reassembler.pendingMessages, 1);
  assert.equal(reassembler.pendingBytes, MAX_FRAGMENT_PAYLOAD);
});

test('a reused message id with a different total does not mix messages', () => {
  const reassembler = new Reassembler();
  const first = fragment(Buffer.alloc(MAX_FRAGMENT_PAYLOAD * 3, 0xc1), 77);
  const second = fragment(Buffer.alloc(MAX_FRAGMENT_PAYLOAD * 2, 0xc2), 77);

  reassembler.accept('peer-1', first[0]);
  const rebuilt = deliver(reassembler, 'peer-1', second);

  assert.notEqual(rebuilt, null);
  assert.equal(rebuilt.length, MAX_FRAGMENT_PAYLOAD * 2);
  assert.ok(rebuilt.every((b) => b === 0xc2));
});

test('refuses a payload too large for the header to address', () => {
  // 65535 fragments is the ceiling; the check must fire before the loop tries
  // to build 65536 buffers.
  assert.throws(
    () => fragment(Buffer.alloc(16), 1, 0),
    /maxPayload must be at least 1/,
  );
  assert.throws(
    () => fragment(Buffer.alloc(70_000), 1, 1),
    /over the 65535/,
  );
});

test('rejects a non-Buffer payload rather than sending nonsense', () => {
  assert.throws(() => fragment('a string', 1), TypeError);
});
