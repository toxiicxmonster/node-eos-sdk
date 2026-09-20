# vendor/eos

**This directory is intentionally empty in the repository.**

The Epic Online Services SDK is not redistributable, so it cannot be committed
here. You must download it yourself and unpack it into this directory before
building. See [`docs/sdk-setup.md`](../../docs/sdk-setup.md) for the full
procedure.

Expected layout once you have done that:

```
vendor/eos/
  include/                          SDK headers (eos_sdk.h, eos_lobby.h, ...)
  lib/win64/EOSSDK-Win64-Shipping.lib
  lib/win64/EOSSDK-Win64-Shipping.dll
  lib/osx/libEOSSDK-Mac-Shipping.dylib
  lib/linux/libEOSSDK-Linux-Shipping.so
```

`npm run check-sdk` verifies the layout and tells you what is missing.
