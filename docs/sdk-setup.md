# Getting the SDK and a deployment

Two things have to exist before this package can do anything: the Epic Online
Services C SDK on disk, and a registered Product in the Epic Developer Portal.
Neither can be shipped for you. This page is both, start to finish.

---

## 1. Why the SDK is not in this repository

Epic's SDK licence does not permit redistributing the SDK. That applies to the
headers and the shared libraries alike, so `vendor/eos/` is empty here and
always will be, and `.gitignore` is written to keep it that way.

The practical consequence: **there are no usable prebuilt binaries on npm for
this package.** Installing it gets you the JavaScript, the types and the build
definition. You supply the SDK and build once.

If that is a problem for your distribution model, the answer is to build your
own prebuilds privately and ship them inside your application bundle, which the
licence does allow. See [packaging.md](packaging.md).

---

## 2. Download and unpack the SDK

1. Sign in at <https://onlineservices.epicgames.com/sdk>.
2. Download the **C SDK** package. Not the Unity plugin, not the Unreal plugin —
   those wrap the same library in engine-specific scaffolding you do not want
   here.
3. Unpack it and copy the pieces into `vendor/eos/`:

   | From the SDK archive | To |
   |---|---|
   | `SDK/Include/` | `vendor/eos/include/` |
   | `SDK/Lib/EOSSDK-Win64-Shipping.lib` | `vendor/eos/lib/win64/` |
   | `SDK/Bin/EOSSDK-Win64-Shipping.dll` | `vendor/eos/lib/win64/` |
   | `SDK/Bin/libEOSSDK-Mac-Shipping.dylib` | `vendor/eos/lib/osx/` |
   | `SDK/Bin/libEOSSDK-Linux-Shipping.so` | `vendor/eos/lib/linux/` |

   You only need the libraries for platforms you intend to build on.

4. Check it:

   ```
   npm run check-sdk
   ```

   It names every missing file rather than letting `node-gyp` fail forty lines
   later about a header.

### Which SDK version

This binding was written against the **EOS SDK 1.17.x** C API. Epic revise the C
API between versions: option-struct field names and `EOS_*_API_LATEST` constants
are the details most likely to have moved. A mismatch is a compile error naming
the exact field, not a runtime surprise — if the build fails on an unknown
struct member after an SDK upgrade, that is what happened, and the fix is
local to the `.cc` file the compiler names.

---

## 3. Register a Product

1. Create a developer account at <https://dev.epicgames.com>.
2. Create a **Product** in the Developer Portal. This is free, needs no
   application review to start, and **does not require shipping on the Epic
   Games Store.**
3. Collect five values from Product Settings:

   - Product ID
   - Sandbox ID
   - Deployment ID
   - Client ID
   - Client Secret

4. Generate a 256-bit encryption key — 64 hexadecimal characters. The SDK
   requires one at init even when nothing uses it:

   ```
   node -e "console.log(require('crypto').randomBytes(32).toString('hex'))"
   ```

5. Copy `.env.example` to `.env` and fill it in. `.env` is gitignored; the
   client secret is a credential, not configuration, and does not belong in
   source control or in a renderer process.

---

## 4. Client policy — the part that goes wrong

In **Product Settings → Clients**, the client needs a policy that permits
**Connect login, Lobby, and P2P.** The default policies are restrictive. A
`PeerToPeer` policy, or an equivalent custom one, is what you want.

Getting this wrong produces authorisation failures at login that look exactly
like SDK bugs or like a mistake in your own code. It is the single most common
cause of "the binding is broken".

The way to not lose a day to it: run `node test/smoke.js` before writing
anything of your own. It initialises, logs in and prints a Product User ID. If
that works, the policy is right, and every later failure is genuinely yours.

---

## 5. Steam as an identity provider

To let Steam users authenticate:

1. In the Developer Portal, configure **Steam** as an identity provider.
2. Supply your Steam App ID and a Steam Web API key.

Then log in with a Steam session ticket obtained from `steamworks.js` — see
[`examples/steam-crossplay.js`](../examples/steam-crossplay.js). An Epic account
is created and linked under the hood, with no email or password prompt for the
player.

Use `STEAM_SESSION_TICKET`. `STEAM_APP_TICKET` is deprecated by Epic and this
binding does not offer it.

**Known gotcha:** Steam session tickets expire after about 8 hours of continuous
play, after which `EOS_Connect_Login` starts returning "invalid session ticket".
Listen for the `connect:auth-expiring` event and re-acquire, rather than dropping
a player out of a match. Epic have [a support article on precisely
this](https://eoshelp.epicgames.com/s/article/Why-is-EOS-Connect-Login-returning-Invalid-session-ticket-on-Steam-after-8-hours-of-gameplay).

Registering the Product is free. Shipping on Steam still costs the usual
one-time Steamworks fee, but that is unrelated to EOS: EOS itself has no revenue
share and no concurrent-user billing.

---

## 6. Build

```
npm install
npm run check-sdk
npm run build
npm test            # unit tests, no SDK or credentials needed
node test/smoke.js  # real deployment
```

Toolchain requirements are in the [README](../README.md#requirements).
