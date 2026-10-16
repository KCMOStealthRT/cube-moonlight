#!/bin/sh
# Pair a Moonlight client: add the box in Moonlight (by IP), click it, and enter the 4-digit PIN
# Moonlight shows here within 5 minutes:
#   /storage/cube-moonlight/pin.sh 1234
case "$1" in
[0-9][0-9][0-9][0-9]) ;;
*) echo "usage: $0 <4-digit PIN shown by Moonlight>"; exit 1 ;;
esac
D=/storage/cube-moonlight/moonlight
[ -d $D ] || { echo "$D missing: is cube-moonlight running? (systemctl status cube-moonlight)"; exit 1; }
echo "$1" > $D/pin
echo "PIN sent; Moonlight should show the box as paired in a few seconds."
