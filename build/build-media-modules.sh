#!/bin/bash
# Build CoreELEC's media_modules-aml against the rebuilt kernel tree, with the flags of
# projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/package.mk plus
# CONFIG_AMLOGIC_MEDIA_VENC_COMMON=m + CONFIG_AMLOGIC_MEDIA_VENC_MULTI=m
# (-> encoder_common.ko, amvenc_multi.ko; GPL-2.0). The decoder modules are built too: they are
# compared with the box's copies to prove the build matches its kernel (build.sh).
set -euo pipefail
. "$ENV"
M=$B/media_modules
if [ ! -d "$M" ]; then
    rm -rf "$B/.unpack" && mkdir -p "$B/.unpack"
    tar -xzf "$WORK/dl/media_modules-$MEDIA_MODULES_COMMIT.tar.gz" -C "$B/.unpack"
    mv "$B/.unpack"/media_modules-aml-* "$M" && rm -rf "$B/.unpack"
fi
export PATH=$SRC/clang-$CLANG_VERSION/bin:$PATH
unset LDFLAGS
make -C "$B/linux" ARCH=arm64 LLVM=1 CC=clang CROSS_COMPILE=aarch64-linux-gnu- -j"$JOBS" \
  M="$M/drivers" \
  CONFIG_AMLOGIC_MEDIA_VDEC_MPEG2_MULTI=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_MPEG4_MULTI=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_VC1=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_H264_MULTI=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_H264_MVC=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_H265=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_H266=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_VP9=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_MJPEG_MULTI=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_AVS_MULTI=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_AVS2=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_AVS3=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_AV1=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_VP9_FB=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_H265_FB=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_AV1_FB=m \
  CONFIG_AMLOGIC_MEDIA_VDEC_AVS2_FB=m \
  CONFIG_AMLOGIC_HW_DEMUX=m \
  CONFIG_AMLOGIC_MEDIA_VENC_COMMON=m \
  CONFIG_AMLOGIC_MEDIA_VENC_MULTI=m \
  modules
rm -rf "$OUT/ko" && mkdir -p "$OUT/ko"
find "$M" -name '*.ko' -not -path '*/.*' -exec cp {} "$OUT/ko/" \;
ls "$OUT/ko"
echo MEDIA_MODULES_BUILD_DONE
