# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `eos.isAvailable()` and `eos.loadError`. Requiring the package succeeds even
  where the SDK was never vendored, so integrators had no supported way to ask
  whether the addon would load before offering a multiplayer menu — the only
  option was to re-implement `loadNative` from outside via `node-gyp-build`.
  `isInitialized` does not answer it: it is false for a missing addon and an
  uninitialised one alike.
- `p2p.sendLarge()`, the `p2p:message` event, and the exported `fragment()` and
  `Reassembler`. Payloads over 1170 bytes needed splitting, and the obvious way
  to split them is wrong: slicing JSON into an envelope field re-escapes the
  slice, so escape-heavy content can more than double with no fixed bound. The
  8-byte binary header costs 8 bytes whatever the content. Partial messages are
  bounded in bytes and age so a silent peer cannot hold memory.

### Changed

- **Minimum Node version is now 22.** `node-gyp` 13 requires
  `^22.22.2 || ^24.15.0 || >=26`, and Node 18 and 20 are both past end of life,
  so the declared floor now matches what can actually build. The CI matrix
  tests 22 and 24.
- Bumped `node-gyp` to `^13.0.2`, `actions/checkout` to v7 and
  `actions/setup-node` to v7.

### Fixed

- `npm test` failed on some Node versions. `node --test` accepts different
  positional argument forms across releases, so `scripts/test-unit.js` now
  resolves explicit file paths, which every version accepts.

## [0.1.0] - 2026-09-20

Initial release. Alpha.

### Added

- **Platform** — `init`, a 50 ms tick interval on the JS main thread,
  `shutdown`, SDK logging routed to a `log` event, and guards on the
  once-per-process `EOS_Initialize` / `EOS_Shutdown` lifecycle.
- **Connect** — `login` for 14 external credential types, with first-login
  `EOS_Connect_CreateUser` handled internally; `createDeviceId` for accounts
  that need no store; `getLoggedInUsers`; and a `connect:auth-expiring` event
  for Steam session ticket refresh.
- **Lobby** — create, join by id (via search), leave, members, info, lobby and
  member attributes, invites, and four notification events. The `Lobby` object
  mirrors the shape of `steamworks.js`'s `matchmaking.Lobby`.
- **P2P** — send with selectable reliability, per-tick receive drain, automatic
  or manual connection acceptance, close, and connection lifecycle events.
- Hand-written TypeScript definitions in `index.d.ts`.
- `scripts/check-sdk.js`, which names every missing SDK file rather than letting
  the build fail on a header.
- Tests: a unit suite that needs no SDK, a smoke test against a real deployment,
  and a self-orchestrating two-process P2P loopback test.
- Examples: basic login, Steam crossplay with ticket refresh, a full
  lobby-and-P2P session, Electron main/preload wiring, and a lockstep transport.
- Documentation: SDK and Developer Portal setup, packaging, testing, and the
  original design note.

### Known limitations

- The native layer has not yet been compiled against a real EOS SDK. It is
  written to Epic's published C API; field names and `EOS_*_API_LATEST`
  constants may need adjusting for your SDK version, and the compiler will name
  them.
- No prebuilt binaries are published, because the SDK cannot be redistributed.
- Auth (Epic account login proper) is not wrapped. Connect is what Lobby and P2P
  address; Auth is needed only to obtain a token for an `EPIC` login and for
  Epic social features.

[Unreleased]: https://github.com/toxiicxmonster/node-eos-sdk/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/toxiicxmonster/node-eos-sdk/releases/tag/v0.1.0
