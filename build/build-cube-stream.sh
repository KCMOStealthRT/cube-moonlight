#!/bin/bash
# Cross-build cube-stream (src/) (called by build.sh). Output: $OUT/bin/cube-stream
#   alsa headers: from the build host's libasound2-dev (arch-independent), copied into their own
#                 dir so no other x86 headers get on the include path
#   openssl:      headers from the build host's libssl-dev 3.x (incl. the two x86_64 config
#                 headers - identical for LP64 aarch64), copied the same way
#   libasound.so.2, libssl.so.3, libcrypto.so.3: the box's own (fetch-from-box.sh); they need
#                 a newer glibc than the cross toolchain's -> --allow-shlib-undefined
#   libopus: static (build-opus.sh); speexdsp resample.c (BSD) compiled in standalone mode
#   libavahi-client: not linked; src/gs-mdns.c dlopens the box's copy at run time (mDNS)
#   ENet (cgutman fork, MIT): Moonlight control stream; nanors (MIT): Reed-Solomon FEC at the
#                 commit moonlight-common-c pins, so parity matches the client's decoder
set -euo pipefail
. "$ENV"
SRCDIR=$REPO/src
ENET=$SRC/enet-$ENET_COMMIT; NANORS=$SRC/nanors-$NANORS_COMMIT; SPX=$SRC/speexdsp-$SPEEXDSP_VERSION
ENC=$SRC/libencoder-$LIBENCODER_COMMIT/multiEnc
CC=aarch64-linux-gnu-gcc
O=$B/obj; rm -rf "$O" && mkdir -p "$O/nanors" "$O/enet" "$OUT/bin"
for f in "$NANORS/rs.c" "$NANORS/deps/obl/oblas_common.c" "$NANORS/deps/obl/oblas_lite.c"; do
    $CC -O3 -w -I"$NANORS" -I"$NANORS/deps/obl" -c "$f" -o "$O/nanors/$(basename "$f" .c).o"
done
ENETDEF="-DHAS_FCNTL=1 -DHAS_IOCTL=1 -DHAS_POLL=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 -DHAS_SOCKLEN_T=1"
for f in callbacks compress host list packet peer protocol unix; do
    $CC -O2 -w $ENETDEF -I"$ENET/include" -c "$ENET/$f.c" -o "$O/enet/$f.o"
done
mkdir -p "$O/inc"
cp -r /usr/include/alsa "$O/inc/"
cp -r /usr/include/openssl "$O/inc/"
cp /usr/include/x86_64-linux-gnu/openssl/*.h "$O/inc/openssl/"
$CC -O2 -Wall -o "$OUT/bin/cube-stream" \
    "$SRCDIR/cube-stream.c" "$SRCDIR/gamestream.c" "$SRCDIR/gs-stream.c" "$SRCDIR/gs-input.c" "$SRCDIR/gs-mdns.c" \
    "$SPX/libspeexdsp/resample.c" "$O"/enet/*.o "$O"/nanors/*.o \
    -DOUTSIDE_SPEEX -DRANDOM_PREFIX=cube -DFLOATING_POINT \
    -I"$ENET/include" -I"$NANORS" -I"$NANORS/deps/obl" -I"$SPX/include/speex" -I"$SPX/libspeexdsp" \
    -I"$ENC/amvenc_lib/include" -I"$OUT/opus/include" -I"$O/inc" \
    -L"$OUT/lib" "$OUT/opus/lib/libopus.a" \
    "$SYSROOT/libasound.so.2" "$SYSROOT/libssl.so.3" "$SYSROOT/libcrypto.so.3" \
    -lvpcodec -lamvenc_api -lge2d -lion -lpthread -lm -ldl \
    -Wl,-rpath-link,"$OUT/lib" -Wl,--allow-shlib-undefined
file "$OUT/bin/cube-stream"
echo CUBE_STREAM_BUILD_DONE
