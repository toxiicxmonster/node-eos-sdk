# Building a Node/Electron binding for Epic Online Services

Instructions for `eos.js` — a native Node addon wrapping the EOS C SDK, so
**The Frontier** can run cross-store multiplayer between Steam and Epic Games
Store players.

Written 2026-09-20. Companion to the transport seam in `src/net/transport.js`.

---

## 1. Why this exists

The game's netcode is deterministic lockstep: every peer simulates the whole
world and only orders cross the wire (`src/net/lockstep.js`). That part is
finished and tested. What is missing is a way for two machines to reach each
other.

Steam's own networking cannot do it. `steamworks.js` exposes
`networking.sendP2PPacket(steamId64, sendType, data)` — it addresses Steam
accounts and nothing else. An Epic Games Store player has no Steam ID, so a
Steam-native transport can never carry a cross-store match. It would be
throwaway work.

Epic Online Services can. It is free, has no revenue share and no
concurrent-user billing, and Epic deliberately built it store-agnostic: it runs
on Steam, on the Epic store, and standalone. It federates identity, so a Steam
buyer and an Epic buyer land in the same lobby with the same kind of account
handle.

The blocker is that **EOS ships no Node or Electron binding.** Official SDKs are
C/C++, C#, Unity and Unreal. The community has produced Java (JNA) and Go (cgo)
bindings; nothing for Node.

The REST API is not a way around this. EOS publishes REST endpoints for
identity and backend concerns, but **real-time P2P and Lobbies require the C
SDK's binary protocol** and are not available over REST. Those are the two
interfaces this game actually needs.

So: a native addon. This document is how to build it.

---

## 2. Scope

Do not wrap the SDK. Wrap the four interfaces this game needs and stop.

| Interface | Why | Rough call count |
|---|---|---|
| Platform | Init, tick, shutdown | 5 |
| Connect | Steam or Epic login to a Product User ID | 3 |
| Lobby | Create, join, invite, members, attributes | 8 |
| P2P | Send, receive, accept, close | 5 |

Around twenty entry points. Everything else in EOS — Achievements, Stats,
Leaderboards, Voice, Anti-Cheat, Player Data Storage — is deliberately out of
scope. The game already has its own saves and has no use for the rest. Adding
them later is additive and costs nothing now.

**Non-goal:** a general-purpose EOS binding for the ecosystem. If it turns out
well, publishing it is a nice side effect, but designing for other people's use
cases up front will triple the work.

---

## 3. Prerequisites

### Accounts and registration

1. Create an Epic Games developer account at <https://dev.epicgames.com>.
2. Create a **Product** in the Developer Portal. Free, no application review
   needed to start, and **you do not need to ship on the Epic Games Store.**
3. From the portal, collect the five values the SDK needs to initialise:
   - Product ID
   - Sandbox ID
   - Deployment ID
   - Client ID
   - Client Secret
4. In **Product Settings → Clients**, give the client a policy that permits
   Connect login, Lobby, and P2P. The default policies are restrictive; a
   `PeerToPeer` or equivalent policy is what you want. Getting this wrong
   produces authorisation failures at login that look like SDK bugs.
5. To let Steam users authenticate, configure Steam as an identity provider in
   the portal and supply the Steam App ID and a Steam Web API key.

Note: registering the Product is free. Shipping on Steam still costs the usual
one-time $100 Steamworks fee, but that is unrelated to EOS.

### Tooling

- The EOS SDK for C, downloaded from <https://onlineservices.epicgames.com/sdk>.
  Take the "C SDK" package, not the Unity or Unreal plugin.
- Node.js matching the Electron version's ABI. The project is on Electron 44
  (`package.json`), so build against that, not the system Node.
- `node-gyp`, plus a C++ toolchain:
  - **Windows:** Visual Studio Build Tools with the C++ workload
  - **macOS:** Xcode Command Line Tools
  - **Linux:** `build-essential`
- `prebuildify` and `node-gyp-build` for shipping prebuilt binaries — see §7 M6.

### Read first

The Java binding at <https://github.com/AN3Orik/eossdk> is the single most
useful reference. It wraps the same C SDK across the same interfaces and shows
which calls are sync, which are callback-driven, and how the structs are laid
out. Read its Lobby and P2P wrappers before writing any C++.

---

## 4. Project layout

Build it as a sibling repository, not inside the game. It has its own toolchain,
its own release cadence and its own platform matrix, and vendoring it into the
game would drag a C++ build into a project that currently needs nothing but
`npm install`.

```
eos.js/
  binding.gyp             node-gyp build definition
  package.json            main: index.js, install: node-gyp-build
  index.js                JS surface; loads the prebuilt via node-gyp-build
  index.d.ts              types, mirroring steamworks.js's shape
  src/
    addon.cc              N-API entry, exports the namespaces
    platform.cc/.h        init, tick, shutdown
    connect.cc/.h         login, Product User ID
    lobby.cc/.h           create/join/invite/members/attributes
    p2p.cc/.h             send/receive/accept
    callbacks.cc/.h       the SDK callback -> JS bridge (see §6)
  vendor/eos/
    include/              SDK headers
    lib/win64/            EOSSDK-Win64-Shipping.dll + .lib
    lib/osx/              libEOSSDK-Mac-Shipping.dylib
    lib/linux/            libEOSSDK-Linux-Shipping.so
  prebuilds/              built per platform by prebuildify
  test/
    smoke.js              init + login against a real deployment
    p2p-loopback.js       two processes, one packet
```

**Do not commit the SDK binaries.** Epic's SDK licence does not permit
redistributing the SDK itself in a public repository. Either keep the repo
private, or add a `postinstall` step that requires the developer to drop the
SDK into `vendor/eos/` themselves. Note this prominently in the README; it is
the first thing that will trip up anyone else building it.

---

## 5. Build setup

`binding.gyp`, roughly:

```python
{
  "targets": [{
    "target_name": "eos",
    "sources": [
      "src/addon.cc", "src/platform.cc", "src/connect.cc",
      "src/lobby.cc", "src/p2p.cc", "src/callbacks.cc"
    ],
    "include_dirs": [
      "<!@(node -p \"require('node-addon-api').include\")",
      "vendor/eos/include"
    ],
    "defines": ["NAPI_VERSION=8", "NAPI_DISABLE_CPP_EXCEPTIONS"],
    "conditions": [
      ["OS=='win'", {
        "libraries": ["../vendor/eos/lib/win64/EOSSDK-Win64-Shipping.lib"]
      }],
      ["OS=='mac'", {
        "libraries": ["../vendor/eos/lib/osx/libEOSSDK-Mac-Shipping.dylib"],
        "xcode_settings": { "OTHER_LDFLAGS": ["-Wl,-rpath,@loader_path"] }
      }],
      ["OS=='linux'", {
        "libraries": ["../vendor/eos/lib/linux/libEOSSDK-Linux-Shipping.so"],
        "ldflags": ["-Wl,-rpath,$ORIGIN"]
      }]
    ]
  }]
}
```

Use **node-addon-api** (C++ N-API), not raw NAN or V8 headers. N-API is ABI
stable, which means one prebuilt binary works across Node and Electron versions
and you are not rebuilding every time Electron bumps.

The `rpath` settings matter: the EOS shared library has to be found at runtime
next to the addon, and on macOS and Linux the default search path will not look
there. Getting this wrong produces a `dlopen` failure that only shows up in the
packaged build, never in development. (In a real `binding.gyp` the Linux
`$ORIGIN` needs escaping as `'$$ORIGIN'` so gyp passes it through literally.)

---

## 6. The hard part: the callback and tick model

This deserves its own section because it is where a naive binding goes wrong.

EOS is a **polled, callback-driven C library**:

- `EOS_Platform_Tick()` must be called regularly — roughly every frame, and at
  minimum a few times a second. Nothing progresses if it is not: no login
  completes, no lobby updates arrive, no packets are delivered.
- Results come back through C function pointers that fire **inside the
  `EOS_Platform_Tick()` call**, on whichever thread called it.

Node is single-threaded with an event loop and JS callbacks that may only be
invoked from the main thread. Three ways to bridge, in ascending order of
sanity:

**Option A — tick on the JS main thread via a timer.** A `setInterval` in the
addon or in JS calls `EOS_Platform_Tick()`. Callbacks fire synchronously inside
that call, already on the main thread, so they can invoke JS directly.

- Simplest by a wide margin. No threads, no locks, no `ThreadSafeFunction`.
- Tick rate is at the mercy of the Node event loop. A long synchronous
  operation stalls EOS.
- **Recommended.** Start here.

**Option B — tick on the Electron renderer's animation frame.** Same as A but
driven from the game's own frame loop, so EOS ticks exactly as often as the
simulation does.

- Only workable if the addon is loaded in the renderer, which it should not be:
  the renderer runs with `nodeIntegration: false` and `contextIsolation: true`
  (see `electron/preload.js`), and that posture is worth keeping.
- Rejected for this project.

**Option C — a dedicated tick thread with `Napi::ThreadSafeFunction`.** A native
thread ticks EOS; callbacks marshal to JS via a thread-safe function.

- Immune to event-loop stalls, and the correct answer for a production-grade
  general binding.
- Substantially more code, and every callback becomes an async hop with
  lifetime questions attached.
- Do this only if Option A's tick jitter proves to be a real problem in testing.

**Decision: Option A.** Tick from the Electron **main process** on a 50 ms
interval — 20 Hz, comfortably above the SDK's needs and far cheaper than the
frame rate. The lockstep turn is 200 ms (`ticksPerTurn: 6` at 30 Hz), so a 50 ms
tick gives four opportunities per turn for a packet to land. That is ample.

### Callback lifetime

Every async EOS call takes a `ClientData` void pointer that comes back in the
callback. The pattern:

1. Heap-allocate a small struct holding a `Napi::FunctionReference` (or a
   `Promise::Deferred`) for the JS continuation.
2. Pass its pointer as `ClientData`.
3. In the C callback, cast it back, resolve the promise, `delete` the struct.

Get this wrong and you have either a leak on every lobby operation or a
use-after-free that crashes minutes into a match. Write it once in
`callbacks.cc` as a template and use it everywhere. Do not hand-roll it per
call site.

---

## 7. Implementation order

Six milestones. Each one is independently verifiable — do not start the next
until the current one prints the thing it is supposed to print.

### M1 — Platform lifecycle

```
EOS_Initialize(EOS_InitializeOptions)
EOS_Platform_Create(EOS_Platform_Options)   -> EOS_HPlatform
EOS_Platform_Tick(handle)                    called on the 50 ms interval
EOS_Platform_Release(handle)
EOS_Shutdown()
```

JS surface:

```js
const eos = require('eos.js');
eos.init({
  productId, sandboxId, deploymentId,
  clientId, clientSecret,
  encryptionKey,        // 64 hex chars; required, even unused
  isServer: false,
});
// internal: setInterval(() => native.tick(), 50)
eos.shutdown();
```

**Done when:** the process starts, ticks for ten seconds and exits cleanly with
no crash and no leak. Enable `EOS_Logging_SetCallback` early and route it to
`console.log` behind a `debug` flag — the SDK is talkative and will tell you
exactly why init failed.

Gotcha: `EOS_Initialize` may only be called once per process, and
`EOS_Shutdown()` cannot be followed by another `EOS_Initialize`. Guard both in
JS. Electron reloads during development will hit this.

### M2 — Connect login (the cross-store piece)

This is the milestone that makes crossplay real, so get the concept straight
before coding. EOS has two distinct identity systems:

- **Auth** — Epic Games accounts proper. Epic store players.
- **Connect** — a **Product User ID (PUID)**, a per-product identity that any
  external account can map onto.

**Use Connect, not Auth.** The PUID is the handle Lobby and P2P address, and it
is what makes a Steam player and an Epic player addressable by the same call.
Auth is only needed if you want Epic social features such as the friends list.

```
EOS_Connect_Login(options, clientData, callback) -> EOS_ProductUserId
EOS_Connect_CreateUser(...)                         on first login for an account
EOS_ProductUserId_ToString / FromString             for passing across the bridge
```

Two credential paths:

**Steam player.** Get a Steam session ticket from `steamworks.js`, which the
game already depends on:

```js
const ticket = await steamworks.auth.getSessionTicketWithSteamId(steamId);
await eos.connect.login({
  type: 'STEAM_SESSION_TICKET',
  token: ticket.getBytes().toString('hex'),
});
```

Use `EOS_ECT_STEAM_SESSION_TICKET`. `EOS_ECT_STEAM_APP_TICKET` is deprecated.
An Epic Games account is created under the hood and linked to the Steam
account, with no email or password prompt for the player.

**Known gotcha:** Steam session tickets expire after about 8 hours of
continuous play, after which `EOS_Connect_Login` starts returning "invalid
session ticket". Refresh the ticket periodically, or at minimum handle the
failure by re-acquiring rather than dropping the player out of a match. Epic
have a support article on precisely this.

**Epic player.** `EOS_ECT_EPIC` with a token obtained from
`EOS_Auth_Login` — either the Epic Games Launcher's exchange code (passed on
the command line when launched from the launcher) or an account portal prompt.

**Done when:** both a Steam login and an Epic login return a PUID string, and
the two PUIDs differ. Print them.

### M3 — Lobby

```
EOS_Lobby_CreateLobby(...)      bucket id, max members, permission level
EOS_Lobby_JoinLobby(...)
EOS_Lobby_LeaveLobby(...)
EOS_Lobby_CopyLobbyDetailsHandle(...)
EOS_LobbyDetails_GetMemberCount / GetMemberByIndex
EOS_Lobby_UpdateLobbyModification + AddAttribute   for map/seed/faction/ready
EOS_Lobby_AddNotifyLobbyUpdateReceived             for change notifications
EOS_Lobby_AddNotifyLobbyInviteAccepted             for invite handling
```

Mirror the shape of `steamworks.js`'s `matchmaking.Lobby` class deliberately —
`getMembers()`, `getOwner()`, `setData()`, `getFullData()`, `openInviteDialog()`.
The game's lobby UI is written against that shape, so matching it means the UI
does not care which backend is underneath.

Lobby attributes carry the match setup: map size, seed, time of day, fog, and
each member's faction, team and ready state. The host owns the authoritative
copy; members set only their own attributes.

**Done when:** two processes on one machine create and join a lobby, and each
sees the other in `getMembers()`.

### M4 — P2P

```
EOS_P2P_SendPacket(options)              socket id, channel, reliability
EOS_P2P_ReceivePacket(options)           poll; drain in a loop until empty
EOS_P2P_GetNextReceivedPacketSize(...)
EOS_P2P_AddNotifyPeerConnectionRequest   must accept, or nothing arrives
EOS_P2P_AcceptConnection(...)
EOS_P2P_CloseConnection(...)
```

Reliability: use **`EOS_PR_ReliableOrdered`**. `lockstep.js` is explicit that the
transport must be reliable and ordered, and explains why — a lost turn is a
permanently stalled match, so head-of-line blocking is the right trade. A pause
is recoverable; divergence is not.

A "socket" here is just a named channel, e.g. `{ SocketName: "frontier" }`. Both
peers must use the same name.

Drain received packets on the same 50 ms tick as the platform, in a `while` loop
until `GetNextReceivedPacketSize` reports nothing — several turns' packets can
arrive between ticks, and stopping after one leaves a backlog that grows.

**Done when:** two processes exchange a thousand packets in order with none
lost, and `p2p-loopback.js` passes.

### M5 — Electron main-process integration

The addon lives in the **main process only.** The renderer reaches it through
the existing narrow `contextBridge` in `electron/preload.js`, which currently
exposes settings, fullscreen, quit and Steam. Add a `net` namespace beside
those. Do not enable `nodeIntegration` in the renderer to shortcut this; the
comment at the top of `preload.js` explains why that posture is worth keeping,
and it is right.

Payloads cross as JSON over IPC. They are tiny — the lockstep test measured
10 KB of commands across a two-minute match, 0.08 KB/s per peer — so there is
no case for a binary channel or shared memory.

### M6 — Packaging

- Run `prebuildify --napi --strip` per platform; commit or release the
  `prebuilds/` output so consumers never need a compiler.
- `electron-builder` must ship the EOS shared library as an unpacked file.
  Add to `electron-builder.yml`:
  ```yaml
  asarUnpack:
    - "**/node_modules/eos.js/prebuilds/**"
    - "**/node_modules/eos.js/vendor/eos/lib/**"
  ```
  A native library inside an asar archive cannot be `dlopen`ed. This will work
  perfectly in development and fail only in the packaged build, which is the
  worst way to find out.
- macOS: the dylib must be signed and listed in the hardened-runtime
  entitlements, or notarisation rejects the app.

---

## 8. How it plugs into the game

The game side is already prepared. `src/net/transport.js` defines the seam:

```js
{
  async connect(),              // establish; resolve when ready
  send(payload),                // hand a turn's commands to the wire
  onPayload(fn),                // deliver an arriving payload
  onPeers(fn),                  // membership changed
  disconnect(),
  localId,                      // this peer's seat
  peerIds,                      // every seat supplying commands
}
```

`Lockstep` takes `send` and is fed by `receive` (see `src/net/lockstep.js`), and
knows nothing else about the network. Any transport satisfying the interface
above works unchanged.

Topology is **star**: the host relays. Every peer still simulates everything —
that is lockstep and does not change — but packets go peer → host → peers
rather than all-to-all. The reason is NAT: a four-player mesh needs six
traversals to all succeed, a star needs three. The host is also the natural
arbiter for seats, readiness and drop detection.

Relaying costs nothing here. Four peers at 0.08 KB/s is not a load.

An EOS transport is therefore a new file, `src/net/transport-eos.js`,
implementing the same interface against the `window.desktop.net` bridge. Nothing
in `lockstep.js`, `commands.js`, `checksum.js` or the lobby UI changes.

**Host migration**, if wanted later, is unusually cheap in this architecture:
every peer already holds a bit-identical world, so a survivor becomes the new
relay and everyone resumes on an agreed turn. No state transfer, because there
is no state to transfer. Not needed now; worth not designing it out.

---

## 9. Testing

The awkward part of this project is that the interesting failures need two
machines and real NATs.

1. **Unit** — `test/smoke.js` inits, logs in, prints a PUID, shuts down. Run in
   CI with credentials from secrets.
2. **Loopback** — two Node processes on one machine, one lobby, a thousand
   packets. Catches API misuse, not networking.
3. **Two machines, same LAN** — first real test of the NAT layer.
4. **Two machines, different networks** — the one that matters. A phone
   hotspot for the second machine is the cheapest way to get a genuinely
   different NAT. Test symmetric NAT specifically; it is what forces EOS onto
   its relay, and that path needs exercising before players find it.
5. **Cross-store** — a Steam build and an Epic build in one match. The whole
   point. Do not defer this to the end; a policy misconfiguration in the
   Developer Portal can make it impossible, and it is better to learn that in
   week one.

Reuse `tools/lockstep-test.mjs` as the load generator. It already drives two
peers through a real match with AI on both sides and checks agreement,
stalling, recovery and desync detection. Swapping its in-process `Link` for a
real EOS transport turns it into an end-to-end network test for free — that is
the highest-value test in this document and it is nearly written already.

---

## 10. Effort and risk

Honest estimate for someone comfortable with C++ and N-API:

| Milestone | Estimate |
|---|---|
| M1 Platform | 1-2 days |
| M2 Connect (both paths) | 2-4 days |
| M3 Lobby | 3-5 days |
| M4 P2P | 2-3 days |
| M5 Electron wiring | 1-2 days |
| M6 Packaging, 3 platforms | 2-4 days |
| **Total** | **~2-4 weeks** |

Add meaningful time if C++ is unfamiliar. The callback-lifetime work in §6 is
the part most likely to overrun, because its failures are intermittent crashes
rather than compile errors.

**Risks, most likely first:**

1. **Developer Portal policy misconfiguration.** Presents as authorisation
   failures that look like code bugs. Mitigation: get M2 working against a real
   deployment before writing Lobby or P2P.
2. **Callback lifetime bugs.** Intermittent crashes under load. Mitigation: one
   templated helper, used everywhere, reviewed once, carefully.
3. **Packaging.** The asar and rpath problems only appear in packaged builds.
   Mitigation: package once, early, at the end of M1 — before there is anything
   complicated to debug alongside it.
4. **Steam ticket expiry.** Long sessions break. Mitigation: handle it in M2
   rather than discovering it in a playtest.

**Fallback.** If this proves too much, there is no option that keeps crossplay
with no infrastructure at all — but a self-hosted relay is unusually cheap here.
At 0.08 KB/s per peer, a $5/month VPS relays hundreds of concurrent matches, and
it is a WebSocket server of maybe 200 lines against the same transport seam. It
costs the "no servers" goal and nothing else. Keep it in the back pocket.

---

## 11. Reference

- EOS SDK download — <https://onlineservices.epicgames.com/sdk>
- EOS overview — <https://dev.epicgames.com/docs/epic-online-services/eos-overview>
- P2P interface reference — <https://dev.epicgames.com/docs/epic-online-services/multiplayer/nat-p2p-interface/p2p-reference>
- Free PC crossplay tools — <https://onlineservices.epicgames.com/news/epic-online-services-release-free-pc-crossplay-tools>
- Crossplay overlay, PC and console — <https://onlineservices.epicgames.com/news/epic-online-services-expands-free-crossplay-overlay-from-pc-to-consoles>
- Java binding, best structural reference — <https://github.com/AN3Orik/eossdk>
- Steam session ticket expiry note — <https://eoshelp.epicgames.com/s/article/Why-is-EOS-Connect-Login-returning-Invalid-session-ticket-on-Steam-after-8-hours-of-gameplay>
- node-addon-api — <https://github.com/nodejs/node-addon-api>
- prebuildify — <https://github.com/prebuild/prebuildify>

**A note on the API signatures in this document:** they are written from the
SDK's documented shape and from the community bindings, not from a local copy
of the headers. Verify each against `vendor/eos/include/` once the SDK is
downloaded — Epic revise the C API between versions, and the exact option-struct
field names and `ApiVersion` constants are the details most likely to have
moved.
