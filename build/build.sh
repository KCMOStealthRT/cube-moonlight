#!/bin/bash
# Build cube-moonlight for your Fire TV Cube 3 (Linux x86_64 or WSL2; tested on Ubuntu 24.04).
#
#   1. build/fetch-from-box.sh <box-ip> sysroot     (copies a few files from the box)
#   2. build/build.sh --sysroot sysroot
#
# Options:
#   --sysroot DIR     output of fetch-from-box.sh (required)
#   --coreelec REF    CoreELEC tag or commit to build for (default: the box's BUILD_ID)
#   --work DIR        work dir, default $CUBE_WORK or ~/cube-moonlight-build (~3 GB per release,
#                     plus ~1 GB of downloads)
#   --jobs N          parallel jobs, default nproc
#
# Result: <work>/build-<release>/cube-moonlight-<release>.tar.gz - copy it to the box and run
# install.sh (README). It contains Amlogic SLA binaries: for your own use, never redistribute.
set -euo pipefail
REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
SYSROOT= REF= JOBS=$(nproc)
WORK=${CUBE_WORK:-$HOME/cube-moonlight-build}
while [ $# -gt 0 ]; do
    case $1 in
    --sysroot) SYSROOT=$(readlink -f "$2"); shift 2 ;;
    --coreelec) REF=$2; shift 2 ;;
    --work) WORK=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    -h|--help) sed -n 2,18p "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
    esac
done
[ -n "$SYSROOT" ] || { echo "--sysroot DIR is required (build/fetch-from-box.sh)" >&2; exit 1; }
for f in os-release libasound.so.2 libssl.so.3 libcrypto.so.3 kernel-release media_modules-aml; do
    [ -e "$SYSROOT/$f" ] || { echo "$SYSROOT/$f missing - run build/fetch-from-box.sh" >&2; exit 1; }
done
if [ -z "$REF" ]; then
    REF=$(. "$SYSROOT/os-release"; echo "${BUILD_ID:-}")
    [ -n "$REF" ] || { echo "no BUILD_ID in $SYSROOT/os-release; pass --coreelec" >&2; exit 1; }
fi

missing=
for t in aarch64-linux-gnu-gcc make curl git python3 patch bc flex bison file perl; do
    command -v $t >/dev/null || missing="$missing $t"
done
[ -d /usr/include/alsa ] || missing="$missing libasound2-dev"
[ -d /usr/include/openssl ] && ls /usr/include/x86_64-linux-gnu/openssl/*.h >/dev/null 2>&1 ||
    missing="$missing libssl-dev"
if [ -n "$missing" ]; then
    echo "missing:$missing" >&2
    echo "Ubuntu: sudo apt install gcc-aarch64-linux-gnu make curl git python3 patch bc flex bison file perl libasound2-dev libssl-dev libelf-dev" >&2
    exit 1
fi

export REPO SYSROOT WORK JOBS
mkdir -p "$WORK"
ENV=$(bash "$REPO/build/versions.sh" "$REF")
. "$ENV"
export ENV SRC=$WORK/src B=$WORK/build-$CE_VERSION
export OUT=$B/out
mkdir -p "$OUT"
echo "== CoreELEC $CE_VERSION ($CE_COMMIT), work dir $B"

step() { echo "== $1"; bash "$REPO/build/$1" > "$B/${1%.sh}.log" 2>&1 ||
         { echo "FAILED: $1 - see $B/${1%.sh}.log" >&2; tail -20 "$B/${1%.sh}.log" >&2; exit 1; }; }
step fetch-sources.sh
step build-kernel.sh
step build-media-modules.sh
echo "== verify-modules.sh"; bash "$REPO/build/verify-modules.sh"
step build-userspace.sh
step build-opus.sh
step build-cube-stream.sh
bash "$REPO/build/package.sh"
