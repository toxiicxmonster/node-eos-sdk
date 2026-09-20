# Examples

All of these read credentials from the environment. Copy `.env.example` to
`.env` at the repository root, fill it in, and export it — or set the variables
however you normally do.

They `require('node-eos-sdk')`. Running them from inside this repository, use
`require('../index.js')` instead, or `npm link` the package.

| File | What it shows |
|---|---|
| [`basic-login.js`](basic-login.js) | The smallest useful program: init, log in, print a Product User ID, shut down. **Run this first** — if it works, your Developer Portal client policy is right. |
| [`steam-crossplay.js`](steam-crossplay.js) | The point of the whole exercise: a Steam player and an Epic player reaching the same lobby. Includes session-ticket refresh, which Steam sessions need after ~8 hours. |
| [`lobby-and-p2p.js`](lobby-and-p2p.js) | A full session — lobby attributes for match setup, member attributes for per-player state, packets once everyone is in. |
| [`electron-main.js`](electron-main.js) + [`electron-preload.js`](electron-preload.js) | Main-process integration with a narrow `contextBridge`. The renderer keeps `nodeIntegration: false`. |
| [`lockstep-transport.js`](lockstep-transport.js) | A transport for deterministic lockstep: reliable-ordered delivery, star topology with the host relaying, stable seat ordering. |

## Running the pair

```sh
node examples/lobby-and-p2p.js host
# prints a lobby id

node examples/lobby-and-p2p.js join <lobbyId>
```

On one machine this only exercises the API. The failures that matter need two
machines on different networks — see [docs/testing.md](../docs/testing.md).
