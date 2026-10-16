#!/bin/bash
# Download and unpack every source for one CoreELEC release (called by build.sh; needs $ENV).
# Tarballs are cached in $WORK/dl, unpacked trees in $WORK/src (shared between releases where
# the commit is the same).
set -euo pipefail
. "$ENV"
DL=$WORK/dl; SRC=$WORK/src; mkdir -p "$DL" "$SRC"

dl() {   # url file
    [ -s "$DL/$2" ] && return
    echo "fetch: $1"
    curl -fL --retry 3 -o "$DL/$2.part" "$1"
    mv "$DL/$2.part" "$DL/$2"
}
unpack() {   # file dir  (dir = $SRC/<name>; the tarball's single top dir becomes it)
    [ -d "$SRC/$2" ] && return
    rm -rf "$SRC/.unpack" && mkdir -p "$SRC/.unpack"
    tar -xzf "$DL/$1" -C "$SRC/.unpack"
    mv "$SRC/.unpack"/* "$SRC/$2"
    rmdir "$SRC/.unpack"
}
gh() {   # owner/repo commit name
    dl "https://github.com/$1/archive/$2.tar.gz" "$3-$2.tar.gz"
    unpack "$3-$2.tar.gz" "$3-$2"
}
shacheck() {   # file sha256
    echo "$2  $DL/$1" | sha256sum -c --quiet - || { rm -f "$DL/$1"; echo "checksum mismatch: $1" >&2; exit 1; }
}

# kernel trees: only downloaded here, build-kernel.sh / build-media-modules.sh unpack them per release
dl "https://github.com/CoreELEC/linux-amlogic/archive/$LINUX_COMMIT.tar.gz" "linux-$LINUX_COMMIT.tar.gz"
dl "https://github.com/CoreELEC/common_drivers/archive/$COMMON_DRIVERS_COMMIT.tar.gz" "common_drivers-$COMMON_DRIVERS_COMMIT.tar.gz"
dl "https://github.com/CoreELEC/media_modules-aml/archive/$MEDIA_MODULES_COMMIT.tar.gz" "media_modules-$MEDIA_MODULES_COMMIT.tar.gz"
dl "$KCONFIG_URL" "linux.aarch64-$CE_COMMIT.conf"

# clang: android.googlesource +archive tarballs have no top directory
dl "$CLANG_URL" "clang-$CLANG_VERSION.tar.gz"
if [ ! -d "$SRC/clang-$CLANG_VERSION" ]; then
    mkdir -p "$SRC/clang-$CLANG_VERSION.part"
    tar -xzf "$DL/clang-$CLANG_VERSION.tar.gz" -C "$SRC/clang-$CLANG_VERSION.part"
    mv "$SRC/clang-$CLANG_VERSION.part" "$SRC/clang-$CLANG_VERSION"
fi

gh aml-streambox/libencoder "$LIBENCODER_COMMIT" libencoder
gh aml-streambox/libion "$LIBION_COMMIT" libion
gh aml-streambox/libge2d "$LIBGE2D_COMMIT" libge2d

for r in "cgutman/enet $ENET_COMMIT enet" "sleepybishop/nanors $NANORS_COMMIT nanors"; do
    set -- $r
    if [ ! -d "$SRC/$3-$2" ]; then
        git clone -q "https://github.com/$1.git" "$SRC/$3-$2.part"
        git -C "$SRC/$3-$2.part" checkout -q "$2"
        mv "$SRC/$3-$2.part" "$SRC/$3-$2"
    fi
done

dl "https://downloads.xiph.org/releases/opus/opus-$OPUS_VERSION.tar.gz" "opus-$OPUS_VERSION.tar.gz"
shacheck "opus-$OPUS_VERSION.tar.gz" "$OPUS_SHA256"
unpack "opus-$OPUS_VERSION.tar.gz" "opus-$OPUS_VERSION"
dl "https://downloads.xiph.org/releases/speex/speexdsp-$SPEEXDSP_VERSION.tar.gz" "speexdsp-$SPEEXDSP_VERSION.tar.gz"
shacheck "speexdsp-$SPEEXDSP_VERSION.tar.gz" "$SPEEXDSP_SHA256"
unpack "speexdsp-$SPEEXDSP_VERSION.tar.gz" "speexdsp-$SPEEXDSP_VERSION"
echo FETCH_DONE
