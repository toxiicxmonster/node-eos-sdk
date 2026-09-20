# Contributing

Thanks for looking. This is a small, deliberately narrow binding, and the most
useful contributions are correspondingly specific.

## What is most wanted

1. **SDK-version fixes.** The native layer is written against Epic's published C
   API, not a pinned copy of the headers — the SDK cannot be redistributed, so
   this repository has nothing to check against. If a struct field or an
   `EOS_*_API_LATEST` constant has moved and your build fails on it, the fix is
   small, local, and exactly what this project needs. Say which SDK version in
   the pull request.
2. **Build reports.** "Built clean on macOS 15 / arm64 / SDK 1.17.0" is worth
   opening an issue for. So is the opposite.
3. **New interfaces.** Achievements, Stats, Leaderboards, Player Data Storage —
   all deliberately out of scope for now, all additive. Follow the existing
   pattern (see below) and they fit cleanly.
4. **Documentation of things that cost you a day.** Particularly Developer
   Portal configuration, which is where most time gets lost.

## Setup

```sh
git clone https://github.com/toxiicxmonster/node-eos-sdk.git
cd node-eos-sdk
npm install

# download the C SDK into vendor/eos -- see docs/sdk-setup.md
npm run check-sdk
npm run build
npm test
```

`npm test` runs with no SDK and no credentials, so you can work on the
JavaScript surface before you have registered an Epic product.

## The patterns to follow

**Native stays close to the C API.** Anything convenient — promises, the
EventEmitter, the `Lobby` object — belongs in `index.js`, where it is cheap to
change. The `.cc` files wrap calls and translate values, and that is all.

**Every async call uses the `AsyncOp` template in `src/callbacks.h`.** Do not
hand-roll a `ClientData` struct per call site. Callback lifetime is the one
place in this codebase where a mistake produces an intermittent crash minutes
into a match rather than a compile error, so it is written once and reused. Read
the comment at the top of that file before adding a call.

**Every copied SDK handle gets released.** Lobby details handles in particular;
`ScopedLobbyDetails` in `src/lobby.cc` exists so that no read path can forget.

**Notification callbacks emit events, they do not resolve promises.** They fire
repeatedly; an `AsyncOp` fires once and deletes itself.

**Add the TypeScript types.** `index.d.ts` is hand-written and is part of the
public surface. A new call without types is an incomplete change.

## Commit and pull request

- One concern per pull request.
- Say which platform and SDK version you tested on. "Untested on macOS" is
  useful and honest; silence is not.
- If it touches the JS surface, add or update a test in `test/unit/`.
- If it touches P2P or Lobby, say whether you ran `test/p2p-loopback.js`.

## Reporting a bug

Include the SDK version, the platform, the `error.code` (the `EOS_EResult`
name), and the SDK log with `debug: true` — the SDK is talkative and usually
states the real cause outright.

Before filing an authorisation failure, run `node test/smoke.js`. A Developer
Portal client policy that does not permit Connect login produces failures that
look exactly like binding bugs, and that check separates the two in a minute.

## Code of conduct

By participating you agree to abide by the
[Code of Conduct](CODE_OF_CONDUCT.md).
