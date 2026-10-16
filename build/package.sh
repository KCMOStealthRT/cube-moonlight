#!/bin/bash
# Assemble the box package (called by build.sh):
#   $B/cube-moonlight-<release>.tar.gz  ->  extract in /storage on the box (= /storage/cube-moonlight)
# Contains Amlogic SLA binaries (libamvenc_api, libvpcodec): for your own box only.
set -euo pipefail
. "$ENV"
P=$B/pkg/cube-moonlight
rm -rf "$B/pkg" && mkdir -p "$P/bin" "$P/lib" "$P/ko"
cp "$OUT/bin/cube-stream" "$P/bin/"
cp "$OUT/lib/libion.so" "$OUT/lib/libge2d.so" "$OUT/lib/libamvenc_api.so" "$OUT/lib/libvpcodec.so" "$P/lib/"
cp "$OUT/ko/encoder_common.ko" "$OUT/ko/amvenc_multi.ko" "$P/ko/"
cp "$REPO"/box/* "$P/"
chmod 755 "$P"/*.sh "$P"/*.py "$P/bin/cube-stream"
KREL=$(cat "$SYSROOT/kernel-release")
GITREV=$(git -C "$REPO" describe --always --dirty 2>/dev/null || echo unknown)
cat > "$P/BUILD-INFO" <<INFO
# cube-moonlight build - only for this CoreELEC build (kernel modules are tied to it)
COREELEC_VERSION=$CE_VERSION
COREELEC_BUILD_ID=$CE_COMMIT
KERNEL_RELEASE=$KREL
CUBE_MOONLIGHT_REV=$GITREV
BUILT=$(date -u +%Y-%m-%dT%H:%MZ)
INFO
T=$B/cube-moonlight-$CE_VERSION.tar.gz
tar -czf "$T" -C "$B/pkg" cube-moonlight
echo "package: $T"
