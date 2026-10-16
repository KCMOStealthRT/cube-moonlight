#!/bin/sh
# cube-moonlight installer - run on the Fire TV Cube 3 (CoreELEC 22) after extracting the
# package in /storage:
#   cd /storage && tar -xzf cube-moonlight-<release>.tar.gz && sh cube-moonlight/install.sh
# Options: --yes (don't ask)
# What it does (and uninstall.sh undoes):
#   1. checks box, CoreELEC release and package match
#   2. edits /flash/dtb.img (VDIN1 + audio loopback; backup first) -> needs a reboot
#   3. installs + enables cube-moonlight.service (starts at boot)
# Safe to run again (e.g. after a CoreELEC update + rebuild).
set -e
E=/storage/cube-moonlight
UNIT=/storage/.config/system.d/cube-moonlight.service
YES=; [ "${1:-}" = --yes ] && YES=1

die() { echo "install: $*" >&2; exit 1; }
ask() {
    [ -n "$YES" ] && return 0
    [ -t 0 ] || die "not interactive: re-run with --yes to confirm"
    printf '%s [y/N] ' "$1"; read -r a; [ "$a" = y ] || [ "$a" = Y ]
}

[ "$(cd "$(dirname "$0")" && pwd)" = $E ] || die "extract the package in /storage (expected $E)"
. /etc/os-release
[ "$NAME" = CoreELEC ] || die "this is not CoreELEC"
case "$VERSION_ID" in 22*) ;; *) die "CoreELEC $VERSION: only CoreELEC 22 (Kodi 22) is supported" ;; esac
DTID=$(tr -d '\0' < /proc/device-tree/amlogic-dt-id 2>/dev/null || true)
[ "$DTID" = t7_gazelle_pvt ] || die "this is not a Fire TV Cube 3 (dt-id '$DTID')"
. $E/BUILD-INFO
[ "$BUILD_ID" = "$COREELEC_BUILD_ID" ] && [ "$(uname -r)" = "$KERNEL_RELEASE" ] ||
    die "package built for CoreELEC $COREELEC_VERSION ($COREELEC_BUILD_ID), box runs $VERSION ($BUILD_ID): rebuild (build/build.sh)"
for f in /storage/.config/system.d/cube-stream.service /storage/enc/bin/cube-stream; do
    [ -e $f ] && die "found $f from another cube-stream setup: remove it first (two hosts can't share the capture)"
done
for t in python3 fdtget fdtput dtc; do command -v $t >/dev/null || die "$t missing"; done

# Kodi's JSON-RPC (passthrough off during a stream, seek back) needs "Allow remote control from
# applications on this system" (Settings > Services > Control)
if ! python3 - <<'PY' 2>/dev/null
import socket, sys
s = socket.create_connection(('127.0.0.1', 9090), 3)
s.sendall(b'{"jsonrpc":"2.0","id":1,"method":"JSONRPC.Ping"}')
sys.exit(0 if b'pong' in s.recv(4096) else 1)
PY
then
    echo "WARNING: Kodi JSON-RPC (127.0.0.1:9090) does not answer. Enable Kodi > Settings >"
    echo "         Services > Control > 'Allow remote control from applications on this system',"
    echo "         else Dolby/DTS passthrough audio can't be captured during a stream."
fi

echo "cube-moonlight for CoreELEC $COREELEC_VERSION, Fire TV Cube 3"
python3 $E/dtb-patch.py check || true
before=$(md5sum /flash/dtb.img | cut -d' ' -f1)
if ! python3 $E/dtb-patch.py dryrun >/dev/null; then
    die "the device tree can't be patched (see above); nothing changed"
fi
if python3 $E/dtb-patch.py dryrun | grep -q 'would apply nothing'; then
    echo "device tree: already patched"
else
    echo
    echo "Step 1: edit /flash/dtb.img (enables the VDIN1 video loopback and the audio loopback;"
    echo "        the original is kept in $E/backup/). This is a boot file: if the box ever fails"
    echo "        to boot, restore it from Fire OS / recovery (README, 'Undo')."
    ask "Edit /flash/dtb.img now?" || die "aborted, nothing changed"
    python3 $E/dtb-patch.py apply
fi
after=$(md5sum /flash/dtb.img | cut -d' ' -f1)

echo
echo "Step 2: install and enable cube-moonlight.service"
ask "Install the service?" || die "aborted (dtb.img stays patched; uninstall.sh undoes it)"
mkdir -p /storage/.config/system.d
cp $E/cube-moonlight.service $UNIT
rm -f $E/autostart-loading
systemctl daemon-reload
systemctl enable cube-moonlight.service

if [ "$before" != "$after" ] || [ ! -e /dev/video71 ]; then
    echo
    echo "Done. REBOOT the box to activate the device-tree change (cube-moonlight then starts"
    echo "by itself ~30 s after Kodi)."
else
    systemctl restart cube-moonlight.service
    echo
    echo "Done. cube-moonlight is (re)starting; log: $E/cube-stream.log"
fi
echo "Then in Moonlight: add the box by its IP, click it, and run here:  $E/pin.sh <PIN>"
