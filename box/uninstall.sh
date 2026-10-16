#!/bin/sh
# Remove cube-moonlight from the box:
#   sh /storage/cube-moonlight/uninstall.sh            stop + remove the service, restore dtb.img
#   sh /storage/cube-moonlight/uninstall.sh --purge    ... and delete /storage/cube-moonlight
#                                                      (pairings are lost)
# Add --yes to skip the questions. Reboot afterwards to unload everything.
E=/storage/cube-moonlight
UNIT=/storage/.config/system.d/cube-moonlight.service
PURGE=; YES=
for a in "$@"; do case $a in --purge) PURGE=1 ;; --yes) YES=1 ;; esac; done
ask() {
    [ -n "$YES" ] && return 0
    [ -t 0 ] || { echo "not interactive: re-run with --yes"; exit 1; }
    printf '%s [y/N] ' "$1"; read -r a; [ "$a" = y ] || [ "$a" = Y ]
}

if [ -e $UNIT ]; then
    systemctl stop cube-moonlight.service    # SIGTERM: releases VDIN, restores Kodi passthrough
    systemctl disable cube-moonlight.service
    rm -f $UNIT
    systemctl daemon-reload
    echo "service removed"
fi

restored=
if [ -f $E/backup/dtb.img.orig ]; then
    if ask "Restore the original /flash/dtb.img (undo the loopback changes)?"; then
        python3 $E/dtb-patch.py rollback && restored=1
    fi
else
    echo "no dtb backup in $E/backup (nothing was patched by install.sh)"
fi

if [ -n "$PURGE" ]; then
    if [ -f $E/backup/dtb.img.orig ] && [ -z "$restored" ]; then
        echo "not purging: dtb.img was not restored and the backup lives in $E/backup"
    elif ask "Delete $E (incl. Moonlight pairings)?"; then
        cd /storage && rm -rf $E && echo "$E deleted"
    fi
fi
echo "Reboot to finish (unloads the encoder modules; activates the restored dtb.img)."
