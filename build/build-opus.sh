#!/bin/bash
# Cross-build static libopus (called by build.sh). Output: $OUT/opus/{include/opus,lib/libopus.a}
set -euo pipefail
. "$ENV"
[ -f "$OUT/opus/lib/libopus.a" ] && { echo OPUS_BUILD_DONE; exit 0; }
rm -rf "$B/opus" && cp -a "$SRC/opus-$OPUS_VERSION" "$B/opus" && cd "$B/opus"
./configure --host=aarch64-linux-gnu --prefix="$OUT/opus" --enable-static --disable-shared \
    --disable-doc --disable-extra-programs CFLAGS="-O2" >/dev/null
make -j"$JOBS" >/dev/null
make install >/dev/null
echo OPUS_BUILD_DONE
