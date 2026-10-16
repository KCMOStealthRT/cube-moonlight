#!/bin/sh
# Copy what the build needs from your Cube (run on any machine that can ssh to it: Linux,
# macOS, Git Bash on Windows; WSL only with mirrored networking):
#   build/fetch-from-box.sh <box-ip> [dir]        default dir: ./sysroot
#   SSH_OPTS="-i ~/.ssh/mykey" build/fetch-from-box.sh 192.168.1.50
# Gets: /etc/os-release (CoreELEC release -> source pins), libasound.so.2 / libssl.so.3 /
# libcrypto.so.3 (cube-stream links against the box's own), and the box's media modules
# (the build is checked against them). Read-only on the box.
set -eu
HOST=${1:?usage: fetch-from-box.sh <box-ip> [dir]}
DIR=${2:-sysroot}
SSH_OPTS=${SSH_OPTS:-}
mkdir -p "$DIR"
# shellcheck disable=SC2086
info=$(ssh $SSH_OPTS "root@$HOST" 'uname -r; tr -d "\0" < /proc/device-tree/amlogic-dt-id 2>/dev/null; echo; . /etc/os-release; echo "$NAME $VERSION"' </dev/null)
KVER=$(echo "$info" | sed -n 1p)
DTID=$(echo "$info" | sed -n 2p | tr -d '\0')
echo "box: $(echo "$info" | sed -n 3p), kernel $KVER, dt-id $DTID"
[ "$DTID" = t7_gazelle_pvt ] || echo "WARNING: this is not a Fire TV Cube 3 (dt-id t7_gazelle_pvt); cube-moonlight is untested there"
# shellcheck disable=SC2086
scp -q $SSH_OPTS "root@$HOST:/etc/os-release" "root@$HOST:/usr/lib/libasound.so.2" \
    "root@$HOST:/usr/lib/libssl.so.3" "root@$HOST:/usr/lib/libcrypto.so.3" "$DIR/"
rm -rf "$DIR/media_modules-aml"
# shellcheck disable=SC2086
scp -q -r $SSH_OPTS "root@$HOST:/usr/lib/kernel-overlays/base/lib/modules/$KVER/media_modules-aml" "$DIR/"
echo "$KVER" > "$DIR/kernel-release"
echo "saved to $DIR: $(ls "$DIR" | tr '\n' ' ')"
