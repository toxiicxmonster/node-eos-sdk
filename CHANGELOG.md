# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
