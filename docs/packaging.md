# Packaging

Everything on this page fails only in the packaged build. It works perfectly in
development right up until you ship, which is the worst way to find out. Package
once, early — ideally as soon as login works — rather than discovering all of it
at the end alongside something complicated.

---

## Prebuilt binaries

`node-gyp-build` looks for a prebuilt binary in `prebuilds/` before it considers
compiling. Build them per platform:

```
npm run bundle      # prebuildify --napi --strip
```

The binding uses **N-API** (via `node-addon-api`), which is ABI-stable. One
prebuilt binary works across Node and Electron versions, so an Electron bump
does not mean a rebuild.

Because the EOS SDK cannot be redistributed, `prebuilds/` is gitignored here and
this package publishes no binaries. Build them in your own application's
pipeline and ship them inside your bundle, which Epic's licence does allow.

---

## The shared library has to sit next to the addon

The addon links against `EOSSDK-*-Shipping`, and at runtime the dynamic loader
has to find it. `binding.gyp` handles this:

- **macOS:** `-Wl,-rpath,@loader_path`
- **Linux:** `-Wl,-rpath,'$$ORIGIN'` — the doubled `$` is gyp escaping, so that
  a literal `$ORIGIN` reaches the linker. A single `$` silently expands to
  nothing and you get a `dlopen` failure with no obvious cause.
- **Windows:** the DLL is copied next to the built addon; Windows searches the
  loading module's directory.

If you relocate the library, the rpath has to move with it.

---

## electron-builder

A native library inside an asar archive cannot be `dlopen`ed. It must be
unpacked:

```yaml
# electron-builder.yml
asarUnpack:
  - "**/node_modules/node-eos-sdk/prebuilds/**"
  - "**/node_modules/node-eos-sdk/vendor/eos/lib/**"
```

This is the single most common packaging failure with any native addon, and the
EOS shared library makes it two files instead of one.

---

## macOS signing and notarisation

The EOS dylib must be signed and covered by the hardened-runtime entitlements,
or notarisation rejects the app. In practice:

- Sign the dylib as part of the app bundle, not separately after the fact.
- The addon loads a library at runtime, so if you use a restrictive
  entitlements file you will need
  `com.apple.security.cs.disable-library-validation`, unless the dylib is signed
  with the same Team ID as the app.

Test notarisation before you need it to work.

---

## Electron main process only

Load the addon in the **main process**. The renderer should keep
`nodeIntegration: false` and `contextIsolation: true`, and reach the network
through a narrow `contextBridge` — see
[`examples/electron-preload.js`](../examples/electron-preload.js).

This is not only a security posture. The tick model assumes one platform
instance in one process: EOS_Initialize may be called at most once per process,
and a renderer that reloads would have no way back.

---

## A packaging checklist

Before shipping, on each platform:

- [ ] `npm run bundle` produces a binary in `prebuilds/`
- [ ] The packaged app contains the EOS shared library **outside** `app.asar`
- [ ] The app launches with the SDK log at `debug` and reaches a Product User ID
- [ ] Two packaged builds on different networks complete a match
- [ ] macOS: the build passes notarisation, not just signing
