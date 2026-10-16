#!/bin/bash
# Compare every rebuilt media module with the box's own copy (from fetch-from-box.sh):
# same vermagic and the same modversions CRC for every imported symbol, else the new encoder
# modules can't be trusted on that kernel. (A CRC mismatch would only make insmod refuse, but
# a matching build is the proof that the sources and config are the box's.)
set -euo pipefail
BOX=$SYSROOT/media_modules-aml
[ -d "$BOX" ] || { echo "verify: $BOX missing (run fetch-from-box.sh)" >&2; exit 1; }
n=0; bad=0
for f in "$BOX"/*.ko; do
    m=$OUT/ko/$(basename "$f")
    [ -f "$m" ] || { echo "verify: $(basename "$f") not rebuilt"; continue; }
    n=$((n + 1))
    if ! out=$(python3 "$REPO/build/ko-versions.py" "$f" "$m"); then
        echo "$out"; bad=$((bad + 1))
    fi
    va=$(sed -n 's/^vermagic A: //p' <<<"$out"); vb=$(sed -n 's/^vermagic B: //p' <<<"$out")
    [ "$va" = "$vb" ] || { echo "verify: $(basename "$f") vermagic '$vb' != box '$va'"; bad=$((bad + 1)); }
done
[ $n -gt 0 ] || { echo "verify: no modules compared" >&2; exit 1; }
if [ $bad -ne 0 ]; then
    echo "verify: $bad of $n modules differ from the box's - the build does NOT match this CoreELEC" >&2
    exit 1
fi
echo "verify: $n media modules match the box's kernel (vermagic + all CRCs)"
