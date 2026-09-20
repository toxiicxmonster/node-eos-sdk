# node-eos-sdk

A Node.js and Electron binding for the **Epic Online Services C SDK** — Platform,
Connect, Lobby and P2P. Enough to run cross-store multiplayer between Steam,
Epic Games Store and standalone players, and deliberately no more.

[![CI](https://github.com/toxiicxmonster/node-eos-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/toxiicxmonster/node-eos-sdk/actions/workflows/ci.yml)
[![npm](https://img.shields.io/npm/v/node-eos-sdk.svg)](https://www.npmjs.com/package/node-eos-sdk)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

> **Status: alpha (0.1.0).** The JavaScript surface, the build definition and
> the tests are complete and exercised. The native layer is written against
> Epic's published C API rather than a pinned copy of the headers — the SDK is
> not redistributable, so this repository cannot contain one to check against.
> **It has not yet been compiled against a real SDK.** Expect to fix a struct
> field name or an `EOS_*_API_LATEST` constant on first build; the compiler
> names them exactly. If you hit one, a pull request fixing it is the most
> useful thing you can send. See [CONTRIBUTING.md](CONTRIBUTING.md).

---

## Why this exists

Epic Online Services is free — no revenue share, no concurrent-user billing —
and Epic built it store-agnostic on purpose: it runs on Steam, on the Epic
store, and standalone, and it federates identity so that a Steam buyer and an
Epic buyer land in the same lobby with the same kind of account handle.

Steam's own networking cannot do that. `steamworks.js` gives you
`networking.sendP2PPacket(steamId64, ...)`, which addresses Steam accounts and
nothing else. An Epic Games Store player has no Steam ID, so a Steam-native
transport can never carry a cross-store match.

EOS ships no Node or Electron binding. The official SDKs are C/C++, C#, Unity
and Unreal; the community has produced Java and Go bindings and nothing for
Node. The REST API is not a way around it: EOS publishes REST endpoints for
identity and backend concerns, but **real-time P2P and Lobbies require the C
SDK's binary protocol** and are not available over REST.

Hence a native addon.

## Scope

Four interfaces, around twenty entry points.

| Interface | What you get |
|---|---|
| **Platform** | init, tick, shutdown, logging |
| **Connect** | Steam / Epic / device-id login to a Product User ID |
| **Lobby** | create, join, invite, members, attributes, notifications |
| **P2P** | send, receive, accept, close, connection events |

Achievements, Stats, Leaderboards, Voice, Anti-Cheat and Player Data Storage are
out of scope. Adding an interface later is additive and costs nothing now; pull
requests that wrap one following the existing pattern are welcome.

---

## Requirements

- **Node.js 22+**, or Electron. N-API 8.
- A C++17 toolchain:
  - Windows — Visual Studio Build Tools with the C++ workload
  - macOS — Xcode Command Line Tools
  - Linux — `build-essential`
- **The EOS C SDK**, which you download yourself (see below).
- An Epic Developer Portal Product. Free, no review, and you do **not** need to
  ship on the Epic Games Store.

## Install

```sh
npm install node-eos-sdk
```

Then supply the SDK. **This package cannot ship it** — Epic's licence does not
permit redistributing the SDK, so `vendor/eos/` is empty in this repository and
there are no usable prebuilt binaries on npm:

```sh
# after downloading the C SDK from https://onlineservices.epicgames.com/sdk
#   SDK/Include/  ->  vendor/eos/include/
#   SDK/Lib/ Bin/ ->  vendor/eos/lib/<platform>/

npm run check-sdk   # names anything missing
npm run build
```

[**docs/sdk-setup.md**](docs/sdk-setup.md) has the full procedure, including the
Developer Portal client policy — which is the single most common thing to get
wrong, and which presents as authorisation failures that look like code bugs.

---

## Quick start

Requiring this package always succeeds, even where the SDK was never vendored —
it has to, so the module stays requirable for tooling and tests. Ask before you
offer multiplayer:

```js
const eos = require('node-eos-sdk');

if (!eos.isAvailable()) {
  console.warn('EOS unavailable:', eos.loadError.message);
  // hide the multiplayer menu rather than throwing at first click
}
```

`isInitialized` does **not** answer this question — it is false both when the
addon is missing and when it simply has not been initialised yet.

```js
const eos = require('node-eos-sdk');

eos.init({
  productId, sandboxId, deploymentId,
  clientId, clientSecret,
  encryptionKey,        // 64 hex chars; required even if unused
});

// A Product User ID is the handle Lobby and P2P address. Steam and Epic
// players both get one, which is what makes crossplay work.
const me = await eos.connect.login({
  type: 'STEAM_SESSION_TICKET',
  token: steamSessionTicket.toString('hex'),
});

eos.p2p.configure({ localUserId: me, socketName: 'my-game' });

const lobby = await eos.lobby.create({ localUserId: me, maxMembers: 4 });
await lobby.setData({ map: 'ridge', seed: 20260920 });

eos.on('p2p:packet', ({ peerId, data }) => {
  console.log(peerId, data.toString());
});

for (const peer of lobby.getMembers()) {
  if (peer !== me) eos.p2p.send({ remoteUserId: peer, data: Buffer.from('hi') });
}
```

More in [`examples/`](examples/): a minimal login, Steam crossplay with ticket
refresh, a full lobby-and-P2P session, Electron main/preload wiring, and a
lockstep transport.

---

## How it ticks

EOS is a **polled, callback-driven C library**. `EOS_Platform_Tick()` must run
regularly or nothing progresses — no login completes, no lobby update arrives,
no packet is delivered. Results come back through C function pointers that fire
*inside* that tick call, on whichever thread made it.

`init()` starts a **50 ms interval (20 Hz)** on the JS main thread and ticks
there. Every SDK callback therefore arrives already on the main thread and can
call into JavaScript directly — no worker thread, no `ThreadSafeFunction`, no
async hop with lifetime questions attached.

The trade is that a long synchronous operation on your event loop stalls EOS.
For a game whose main process is doing IPC and not much else, that is a good
deal. If it ever stops being one, a dedicated tick thread is the alternative,
and [`docs/design.md`](docs/design.md) sets out what it would cost.

Two consequences worth knowing:

- `EOS_Initialize` may be called **once per process**, and there is no way back
  after `shutdown()`. A second `init()` throws rather than failing opaquely.
  Electron reloads during development hit this.
- The tick interval keeps the process alive. `shutdown()` is how a script ends.

---

## API

### `eos.init(options)` / `eos.shutdown()`

| Option | | |
|---|---|---|
| `productId` `sandboxId` `deploymentId` `clientId` `clientSecret` | required | Developer Portal → Product Settings |
| `encryptionKey` | required | exactly 64 hex characters |
| `debug` | `false` | route SDK logging to the `log` event |
| `tickIntervalMs` | `50` | |
| `isServer` | `false` | |
| `productName` `productVersion` `cacheDirectory` `flags` | | passed through |

### `eos.connect`

```js
await eos.connect.login({ type, token, displayName })  // -> Product User ID
await eos.connect.createDeviceId(deviceModel)          // no store needed
eos.connect.getLoggedInUsers()                         // -> string[]
```

`type` is one of `STEAM_SESSION_TICKET`, `EPIC`, `DEVICE_ID_ACCESS_TOKEN`,
`OPENID_ACCESS_TOKEN`, `DISCORD_ACCESS_TOKEN`, `GOG_SESSION_TICKET`,
`APPLE_ID_TOKEN`, `GOOGLE_ID_TOKEN`, `ITCHIO_JWT`, `ITCHIO_KEY`,
`AMAZON_ACCESS_TOKEN`, `XBL_XSTS_TOKEN`, `PSN_ID_TOKEN`,
`NINTENDO_NSA_ID_TOKEN`.

A first login for an account needs `EOS_Connect_CreateUser`; that is handled
internally, so callers see one promise either way.

> EOS has two identity systems. **Auth** is Epic Games accounts proper;
> **Connect** is a per-product identity that any external account maps onto.
> This binding wraps Connect, because the Product User ID is what Lobby and P2P
> address. You need Auth only for Epic social features such as the friends list,
> and to obtain the token for an `EPIC` login.

### `eos.lobby`

```js
const lobby = await eos.lobby.create({ localUserId, maxMembers, bucketId, permissionLevel });
const lobby = await eos.lobby.join({ localUserId, lobbyId });
const lobby = eos.lobby.attach(lobbyId, localUserId);   // no round trip
```

The returned `Lobby` deliberately mirrors `steamworks.js`'s
`matchmaking.Lobby`, so a lobby UI written against that does not care which
backend is underneath:

```js
lobby.getMembers()                 // string[] of Product User IDs
lobby.getOwner()                   // the host
lobby.getInfo()                    // { maxMembers, availableSlots, ... }

lobby.getFullData()                // lobby-wide attributes
await lobby.setData({ map, seed }) // host only
lobby.getMemberData(userId)        // a member's own attributes
await lobby.setMemberData({ faction, team, ready })

await lobby.sendInvite(targetUserId)
await lobby.leave()
```

Attribute values may be strings, numbers or booleans. `permissionLevel` is
`'publicAdvertised'`, `'joinViaPresence'` or `'inviteOnly'` (the default).

### `eos.p2p`

```js
eos.p2p.configure({ localUserId, socketName, autoAccept, maxPacketsPerTick });
eos.p2p.send({ remoteUserId, data, channel, reliability });      // <= 1170 bytes
eos.p2p.sendLarge({ remoteUserId, data, reliability });          // any size
eos.p2p.acceptConnection({ remoteUserId });
eos.p2p.closeConnection({ remoteUserId });
```

- **`configure()` is required.** Nothing is delivered until the tick loop knows
  which user to drain for.
- A socket is just a named channel. Both peers must use the same name.
- An incoming connection must be *accepted* or nothing arrives. `autoAccept`
  defaults to `true`; set it to `false` to vet peers from the
  `p2p:connection-request` event.
- `reliability` defaults to `'reliableOrdered'`. For deterministic lockstep that
  is the right choice, not a conservative one: a lost turn stalls the match
  permanently, so head-of-line blocking beats the alternative.
- `eos.MAX_PACKET_SIZE` is 1170 bytes. Above that, use `sendLarge()`.

#### Payloads larger than one packet

`sendLarge()` fragments, reassembles on the far end, and delivers the whole
payload as a single `p2p:message` event. Both ends must be this package.

```js
eos.p2p.sendLarge({ remoteUserId: peer, data: Buffer.from(JSON.stringify(world)) });

eos.on('p2p:message', ({ peerId, data }) => {
  const world = JSON.parse(data.toString());
});
```

**Do not hand-roll this by slicing JSON.** The obvious version — cut the JSON
string into pieces and put each piece in an envelope field — re-escapes the
piece when the envelope is serialised. Quotes, backslashes and newlines become
two characters each, and multi-byte UTF-8 can expand six-fold under `\uXXXX`
escaping, so escape-heavy content can more than double in size with no fixed
upper bound. A slice sized to fit in testing then stops fitting in production,
and you get a runtime failure that depends on the data.

If you must implement it yourself, prefix a binary header rather than nesting
text in text. `sendLarge()` uses 8 bytes — message id `uint32le`, index
`uint16le`, total `uint16le` — which costs exactly 8 bytes regardless of
content. `fragment()` and `Reassembler` are exported if you want the halves
separately, or need to write the other end in another language.

Fragments travel on a reserved channel (255 by default, `fragmentChannel` in
`configure()`), are consumed by reassembly, and never surface as `p2p:packet`.
Partial messages are bounded in bytes and age, so a peer that starts a large
message and goes quiet cannot hold memory indefinitely.

### Events

```js
eos.on('p2p:packet', ({ peerId, socketName, channel, data }) => {});
```

| Event | Payload |
|---|---|
| `log` | `{ category, message, level }` — only when `debug` |
| `error` | an `Error` thrown by the tick |
| `connect:auth-expiring` | `{ productUserId }` — **re-acquire the ticket** |
| `lobby:updated` | `{ lobbyId }` |
| `lobby:member-updated` | `{ lobbyId, targetUserId }` |
| `lobby:member-status` | `{ lobbyId, targetUserId, status, statusName }` |
| `lobby:invite-accepted` | `{ inviteId, lobbyId, localUserId, targetUserId }` |
| `p2p:connection-request` | `{ remoteUserId, socketName, accepted }` |
| `p2p:connected` | `{ remoteUserId, socketName, connectionType, networkType }` |
| `p2p:disconnected` | `{ remoteUserId, socketName, reason }` |
| `p2p:packet` | `{ peerId, socketName, channel, data }` |
| `p2p:message` | `{ peerId, socketName, data }` — a reassembled `sendLarge()` payload |
| `p2p:backlog` | `{ maxPacketsPerTick }` — the tick is not keeping up |

`lobby:member-status` has **three** outcomes, not two. Alongside `joined` and
`left` (and `disconnected`, `kicked`, `closed`), `promoted` fires when an
existing member becomes the owner — nobody arrived and nobody departed. Code
that treats every status as an arrival-or-departure will silently mis-handle
host migration, and the bug stays invisible until a host actually leaves. Branch
on `statusName` rather than assuming.

**`connect:auth-expiring` is not optional to handle if you use Steam.** Steam
session tickets expire after about 8 hours of continuous play, after which logins
start failing. Refresh on this event rather than dropping a player mid-match.

### Errors

Every rejection and throw carries the EOS result, so you can branch on it:

```js
try {
  await eos.lobby.join({ localUserId: me, lobbyId });
} catch (error) {
  error.code;       // 'EOS_NotFound'
  error.operation;  // 'EOS_LobbySearch_Find'
}
```

---

## Electron

Load the addon in the **main process** only, and reach it from the renderer
through a narrow `contextBridge` with `nodeIntegration: false` and
`contextIsolation: true`. [`examples/electron-main.js`](examples/electron-main.js)
and [`examples/electron-preload.js`](examples/electron-preload.js) are a working
pair.

Packaging has two failure modes that appear *only* in the packaged build: the
EOS shared library must be unpacked from the asar archive, and on macOS it must
be signed and covered by the hardened-runtime entitlements.
[**docs/packaging.md**](docs/packaging.md) covers both, plus the rpath settings.

## Testing

```sh
npm test                    # JS surface; no SDK, no credentials, no compiler
node test/smoke.js          # init, login, print a PUID  (real deployment)
node test/p2p-loopback.js   # two processes, one lobby, 1000 packets
```

The tests that actually matter need two machines on different networks, and one
of them needs to be behind a symmetric NAT — that is the path that forces EOS
onto its relay, and players will find it whether or not you tested it.
[**docs/testing.md**](docs/testing.md) lays out the ladder.

## Documentation

- [**docs/sdk-setup.md**](docs/sdk-setup.md) — SDK, Developer Portal, client policy
- [**docs/packaging.md**](docs/packaging.md) — prebuilds, asar, rpath, notarisation
- [**docs/testing.md**](docs/testing.md) — the testing ladder
- [**docs/design.md**](docs/design.md) — the original design note: why these four
  interfaces, why this tick model, what the alternatives cost

## Contributing

Yes, please — particularly SDK-version fixes, additional interfaces following
the existing pattern, and platform build reports. See
[CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

[MIT](LICENSE) for the code in this repository. The Epic Online Services SDK is
**not** included here and is licensed separately by Epic Games under its own
terms.

Not affiliated with or endorsed by Epic Games.
