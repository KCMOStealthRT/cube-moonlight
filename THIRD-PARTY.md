# Third-party code and licences

cube-moonlight's own code (`src/`, `box/`, `build/`, `tools/`, `docs/`) is MIT (see `LICENSE`).
**This repository contains no third-party code.** `build/build.sh` downloads everything below
from its original location at a pinned version and builds it on your machine, for your box.

| Component | Source (pinned in `build/versions.sh`) | Licence | Used for |
|---|---|---|---|
| CoreELEC kernel (`linux-amlogic`, `common_drivers`) | github.com/CoreELEC, the commits your CoreELEC release uses | GPL-2.0 | only to build the modules below against (not installed) |
| `media_modules-aml` → `encoder_common.ko`, `amvenc_multi.ko` | github.com/CoreELEC/media_modules-aml | GPL-2.0 | Wave521 hardware encoder driver (already in CoreELEC's source, just not enabled) |
| Android clang | android.googlesource.com, the tag CoreELEC uses | Apache-2.0 with LLVM exception | compiler for the kernel modules |
| **`libencoder`** (`libamvenc_api`, `libvpcodec`, embedded Wave5 firmware) | github.com/aml-streambox/libencoder | **Amlogic Software License Agreement** (see below) | encoder userspace API |
| **`libge2d`** | github.com/aml-streambox/libge2d | **Amlogic SLA** (same notice) | GE2D helper the encoder library needs |
| `libion` | github.com/aml-streambox/libion | Apache-2.0 (`ion.c`, Google) + Amlogic (`IONmem.c`, "see LICENSE") | ION/dma-buf allocation |
| ENet (Moonlight fork) | github.com/cgutman/enet | MIT | Moonlight control stream |
| nanors | github.com/sleepybishop/nanors | MIT | Reed-Solomon FEC (video and audio) |
| Opus | opus-codec.org | BSD-3-Clause | audio encoding |
| speexdsp resampler | speex.org | BSD-3-Clause | 44.1 → 48 kHz resampling |
| OpenSSL, ALSA (`libasound`) | your box's own libraries (linked, not shipped) | Apache-2.0 / LGPL-2.1 | TLS and crypto, audio capture |
| Avahi (`libavahi-client`) | your box's own library (loaded at run time with dlopen, not shipped) | LGPL-2.1 | mDNS announcement |

## Amlogic SLA: read this

The encoder userspace library from `aml-streambox/libencoder` (and `libge2d`) carries this notice:

> This software is provided to you pursuant to Software License Agreement (SLA) with Amlogic
> Inc ("Amlogic"). This software may be used only in accordance with the terms of this
> agreement. Redistribution and use in source and binary forms, with or without modification
> is strictly prohibited without prior written permission from Amlogic.

So:

- This project does **not** host, bundle or distribute that code or anything built from it. The
  build script fetches it from its public GitHub location onto your machine.
- The package `build.sh` produces (`cube-moonlight-<release>.tar.gz`) **contains binaries built
  from it** (`libamvenc_api.so`, `libvpcodec.so`, `libge2d.so`, `libion.so`). It is for your own box only. **Don't share or upload it** (no GitHub releases, no
  forum attachments).
- Whether *you* may use that code at all is between you and Amlogic. Taken literally, the notice
  also restricts use without a signed agreement. Decide for yourself whether to build it. This
  is not legal advice.

`cube-stream` includes one Amlogic header (`vp_multi_codec_1_0.h`) at compile time to call the
library. No Amlogic code is in this repository.

## Protocol

The Moonlight/GameStream host is written from scratch, following the protocol as documented in
`docs/moonlight-protocol.md`. That document was written from reading Sunshine and
moonlight-common-c (both GPL-3.0) and Wolf (MIT). No code from them is copied. "Moonlight",
"GameStream", "Fire TV" and "Kodi" are trademarks of their owners. This project is not affiliated
with any of them.
