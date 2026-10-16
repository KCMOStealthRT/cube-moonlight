#!/bin/sh
# Started by cube-moonlight.service: loads the encoder modules (RAM only, every boot), then runs
# cube-stream in the foreground; systemd stops it with SIGTERM.
#   off switch:      touch /storage/cube-moonlight/no-autostart
#   boot-loop guard: a marker exists while the modules load; if a load ever crashes the kernel
#                    (CoreELEC reboots on an oops), the next boot finds it and skips everything.
#                    rm /storage/cube-moonlight/autostart-loading to allow it again.
E=/storage/cube-moonlight
MARK=$E/autostart-loading
LOG=$E/cube-stream.log

log() { echo "autostart: $*" > $LOG; }

if [ -e $E/no-autostart ]; then
    log "$E/no-autostart exists, not starting"; exit 0
fi

# the kernel modules only fit the exact CoreELEC build they were compiled for
. $E/BUILD-INFO
BOX_BUILD=$(. /etc/os-release; echo "$BUILD_ID")
if [ "$BOX_BUILD" != "$COREELEC_BUILD_ID" ] || [ "$(uname -r)" != "$KERNEL_RELEASE" ]; then
    log "built for CoreELEC $COREELEC_VERSION ($COREELEC_BUILD_ID, kernel $KERNEL_RELEASE)," \
        "this box runs $(. /etc/os-release; echo "$VERSION") ($BOX_BUILD, kernel $(uname -r))." \
        "Rebuild for this release and run install.sh again."
    exit 0
fi

# Wait for Kodi (cube-stream talks to it over JSON-RPC at start) and for CoreELEC's decoder
# modules: amvenc_multi needs symbols from media_clock + decoder_common, which load ~15 s into
# boot (too early -> "Unknown symbol").
i=0
until systemctl is-active --quiet kodi.service && lsmod | grep -q '^media_clock' &&
      lsmod | grep -q '^decoder_common'; do
    [ $i -ge 120 ] && break
    sleep 1; i=$((i + 1))
done
sleep 10

if [ ! -e /dev/video71 ]; then
    log "/dev/video71 missing: the dtb change is not active (reboot after install.sh, or" \
        "a CoreELEC update replaced dtb.img: run install.sh again)"
    exit 0
fi

if ! lsmod | grep -q '^amvenc_multi'; then
    if [ -e $MARK ]; then
        log "an earlier module load did not finish ($MARK), not loading; rm $MARK to try again"
        exit 0
    fi
    touch $MARK; sync
    lsmod | grep -q '^encoder_common' || insmod $E/ko/encoder_common.ko
    insmod $E/ko/amvenc_multi.ko
    rm -f $MARK; sync
    if ! lsmod | grep -q '^amvenc_multi'; then
        log "loading the encoder modules failed (see dmesg), not starting"
        exit 0      # no restart loop: nothing would change
    fi
fi

cd $E || exit 1
export LD_LIBRARY_PATH=$E/lib
# extra cube-stream options (bin/cube-stream -h), one line, e.g. "-F 30" (more video FEC)
OPTS=; [ -f $E/options ] && OPTS=$(cat $E/options)
# shellcheck disable=SC2086
exec bin/cube-stream $OPTS > $LOG 2>&1 < /dev/null
