# Testing

The awkward part of a P2P binding is that the interesting failures need two
machines and real NATs. Everything cheap is worth doing first, but none of it
substitutes for the last two rows.

| Level | Command | Needs | Catches |
|---|---|---|---|
| Unit | `npm test` | nothing | JS surface regressions |
| Smoke | `node test/smoke.js` | deployment credentials | portal policy, init, login |
| Loopback | `node test/p2p-loopback.js` | credentials | API misuse, ordering, lobby membership |
| Same LAN | two machines | two machines | the NAT layer, for the first time |
| Different networks | two machines, one on a hotspot | as above | the failures players actually hit |
| Cross-store | a Steam build and an Epic build | both stores | the entire point |

---

## Unit

```
npm test
```

Runs against the JavaScript only — no SDK, no compiler, no credentials. This is
what CI runs on a pull request from a fork, and what a new contributor can run
before they have registered an Epic product.

## Smoke

```
node test/smoke.js
```

Initialises, ticks, logs in with a device-id account, prints a Product User ID,
shuts down clean. Run this **before anything else you write**. A Developer
Portal client policy that does not permit Connect login fails here, and it fails
in a way that looks exactly like a code bug if you find it halfway through the
Lobby work.

Credentials come from the environment or a `.env` file; see
[sdk-setup.md](sdk-setup.md).

## Loopback

```
node test/p2p-loopback.js
```

Two processes on one machine: one lobby, a thousand packets, checked for order
and loss. It orchestrates itself — it re-spawns as `host` and `peer`, the host
publishes the lobby id, the peer joins it, reads the host's Product User ID out
of the lobby membership and sends.

This catches API misuse, not networking. Both ends are behind the same NAT and
EOS will happily connect them locally.

The two processes get distinct Product User IDs by pointing at separate SDK
cache directories, so each registers its own device-id account. If your SDK
version hands both the same PUID, the test says so in as many words rather than
failing somewhere confusing; you then need two real accounts to run it.

## Two machines, same LAN

The first test of anything real. Run the host on one machine, pass the lobby id
to the other. Most of the way this can fail is configuration, not code.

## Two machines, different networks

**The one that matters.** A phone hotspot for the second machine is the cheapest
way to get a genuinely different NAT.

Test **symmetric NAT** specifically — it is what forces EOS onto its relay, and
that path needs exercising before players find it. Watch the
`connectionType` field on the `p2p:connected` event: it tells you whether you
got a direct connection or a relayed one. A test suite that only ever sees
direct connections has not tested the interesting half.

## Cross-store

A Steam build and an Epic build in one match.

Do not defer this to the end. A policy misconfiguration in the Developer Portal
can make it impossible, and it is much better to learn that in week one than
after everything else is built on the assumption that it works.

---

## Using your own load generator

If you already have a test harness that drives two peers through a real session
in-process, swapping its in-process link for an `EosTransport` (see
[`examples/lockstep-transport.js`](../examples/lockstep-transport.js)) turns it
into an end-to-end network test for nearly free. For a deterministic simulation
this is the highest-value test available: it already knows how to check
agreement, stalling, recovery and desync detection, and it will now do all of
that over a real NAT.
