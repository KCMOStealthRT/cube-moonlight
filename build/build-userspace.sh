#!/bin/bash
# Cross-build the multienc userspace (called by build.sh):
#   libion.so, libge2d.so      aml-streambox/libion, aml-streambox/libge2d
#   libamvenc_api.so           libencoder multiEnc/vpuapi (C&M VPU API + embedded Wave5 firmware)
#   libvpcodec.so              libencoder multiEnc/amvenc_lib (vl_multi_encoder_* API)
# libencoder is Amlogic SLA code ("redistribution ... prohibited"): the results are for your
# own box only. The box's glibc is newer than the cross glibc, so plain dynamic linking is fine.
set -euo pipefail
. "$ENV"
mkdir -p "$OUT/lib"
export CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar
LIBION=$SRC/libion-$LIBION_COMMIT; LIBGE2D=$SRC/libge2d-$LIBGE2D_COMMIT
ENC=$SRC/libencoder-$LIBENCODER_COMMIT/multiEnc
ION="-I$LIBION/include -I$LIBION/include/ion"

# VDI_IOCTL_SYNC_INSTANCE_PARAM is missing from CoreELEC's driver (-EFAULT) -> make it non-fatal
cd "$ENC/vpuapi"
if ! grep -q 'returns -EFAULT ("No such IOCTL")' vdi.c; then
    # a tree patched with the first (ENOTTY-only) version: start again from the pristine file
    if [ -f vdi.c.orig ]; then
        cp vdi.c.orig vdi.c
    elif grep -q "predates this ioctl" vdi.c; then
        tar -xzOf "$WORK/dl/libencoder-$LIBENCODER_COMMIT.tar.gz" \
            "libencoder-$LIBENCODER_COMMIT/multiEnc/vpuapi/vdi.c" > vdi.c
    fi
    patch -p0 < "$REPO/build/libencoder-sync-inst-param.patch"
fi

cd "$LIBION"
$CC -fPIC -O2 -shared -Iinclude -Iinclude/ion -Ikernel-headers -Wl,-soname,libion.so \
    -o "$OUT/lib/libion.so" ion.c IONmem.c

cd "$LIBGE2D/libge2d"
$CC -fPIC -O2 -shared -Iinclude -Ikernel-headers $ION -Wl,-soname,libge2d.so \
    -o "$OUT/lib/libge2d.so" *.c -L"$OUT/lib" -lion

cd "$ENC"
make -C vpuapi CC=$CC ARM=1 >/dev/null
cp vpuapi/libamvenc_api.so "$OUT/lib/"
# (its link line puts $(LIB) before the objects -> --no-as-needed keeps libamvenc_api NEEDED)
make -C amvenc_lib CC=$CC PLATFORM_FLAGS="-I$LIBGE2D/libge2d/include $ION" \
     LDFLAGS="-shared -Wl,--no-as-needed" \
     LIB="-L$ENC/vpuapi -lamvenc_api -L$OUT/lib" >/dev/null
cp amvenc_lib/libvpcodec.so "$OUT/lib/"
file "$OUT"/lib/*
echo USERSPACE_BUILD_DONE
