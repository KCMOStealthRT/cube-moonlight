#!/bin/bash
# Resolve the source pins for one CoreELEC release and write them to $WORK/versions-<ver>.env.
#   versions.sh <CoreELEC tag or full commit>      e.g. 22.0-Piers_rc1 or the box's BUILD_ID
# The kernel modules must be built from exactly the sources the box's kernel was built from
# (modversions CRCs), so the commits are read from CoreELEC's own package.mk files.
# Sourced by build.sh; can also be run alone to see what a release pins.
set -euo pipefail
REF=${1:?usage: versions.sh <CoreELEC tag or commit>}
WORK=${WORK:-${CUBE_WORK:-$HOME/cube-moonlight-build}}
mkdir -p "$WORK"

if [[ $REF =~ ^[0-9a-f]{40}$ ]]; then
    CE_COMMIT=$REF
    # name it after a tag pointing at it, if any
    CE_VERSION=$(git ls-remote --tags https://github.com/CoreELEC/CoreELEC |
                 awk -v c="$REF" '$1 == c && t == "" { t = $2; sub("refs/tags/", "", t); sub("\\^\\{\\}", "", t) }
                                  END { print t }')
    CE_VERSION=${CE_VERSION:-${REF:0:12}}
else
    CE_VERSION=$REF
    # an annotated tag also lists "<tag>^{}" = the commit; prefer that line
    CE_COMMIT=$(git ls-remote https://github.com/CoreELEC/CoreELEC "refs/tags/$REF" "refs/tags/$REF^{}" |
                awk '{ c = $1 } /\^\{\}$/ { p = $1 } END { print (p != "" ? p : c) }')
    [ -n "$CE_COMMIT" ] || { echo "versions.sh: no CoreELEC tag '$REF'" >&2; exit 1; }
fi

RAW=https://raw.githubusercontent.com/CoreELEC/CoreELEC/$CE_COMMIT
P=projects/Amlogic-ce
pkgmk() { curl -sfL --retry 3 "$RAW/$1" || { echo "versions.sh: can't fetch $1" >&2; exit 1; }; }
first() { awk -v k="$1" 'v == "" && index($0, k "=\"") == 1 { v = substr($0, length(k) + 3); sub(/".*/, "", v) }
                         END { print v }'; }   # value of the first VAR="..." line (no SIGPIPE)
pkgver() { pkgmk "$1" | first PKG_VERSION; }

LINUX_COMMIT=$(pkgver $P/packages/linux/package.mk)
COMMON_DRIVERS_COMMIT=$(pkgver $P/packages/linux-drivers/amlogic/common_drivers/package.mk)
MEDIA_MODULES_COMMIT=$(pkgver $P/packages/linux-drivers/amlogic/media_modules-aml/package.mk)
CLANG_MK=$(pkgmk $P/packages/lang/clang/package.mk)
CLANG_VERSION=$(first PKG_VERSION <<<"$CLANG_MK")
CLANG_URL=$(first PKG_URL <<<"$CLANG_MK" |
            sed -e "s/\${PKG_VERSION}/$CLANG_VERSION/g" -e 's/\${PKG_NAME}/clang/g')
KCONFIG_URL=$RAW/$P/devices/Amlogic-no/linux/linux.aarch64.conf

for v in LINUX_COMMIT COMMON_DRIVERS_COMMIT MEDIA_MODULES_COMMIT CLANG_VERSION CLANG_URL; do
    [ -n "${!v}" ] || { echo "versions.sh: could not read $v from CoreELEC $CE_VERSION" >&2; exit 1; }
done

# userspace + bundled libraries: fixed pins, independent of the CoreELEC release
LIBENCODER_COMMIT=d4ed7c0d088283da44470de157a3587d3addd2c0   # aml-streambox/libencoder (Amlogic SLA)
LIBION_COMMIT=15955bf464593b164e0675b8bd2aa6e5fcac1a54       # aml-streambox/libion
LIBGE2D_COMMIT=df518cc5f670694f8bf4a0dd1cd88093f549d05b      # aml-streambox/libge2d
ENET_COMMIT=aca8784                                          # cgutman/enet (Moonlight's fork, MIT)
NANORS_COMMIT=b1e3c22                                        # sleepybishop/nanors, as moonlight-common-c pins it
OPUS_VERSION=1.5.2
OPUS_SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
SPEEXDSP_VERSION=1.2.1
SPEEXDSP_SHA256=8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d

ENV=$WORK/versions-$CE_VERSION.env
{
    for v in CE_VERSION CE_COMMIT LINUX_COMMIT COMMON_DRIVERS_COMMIT MEDIA_MODULES_COMMIT \
             CLANG_VERSION CLANG_URL KCONFIG_URL LIBENCODER_COMMIT LIBION_COMMIT LIBGE2D_COMMIT \
             ENET_COMMIT NANORS_COMMIT OPUS_VERSION OPUS_SHA256 SPEEXDSP_VERSION SPEEXDSP_SHA256; do
        printf '%s=%q\n' "$v" "${!v}"
    done
} > "$ENV"
echo "$ENV"
