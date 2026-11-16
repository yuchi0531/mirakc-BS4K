#!/system/bin/sh
# Phase 16 Round 23B -- create ONE well-formed virtual HID device, then find
# which /sys/bus/hid/drivers/<drv>/ directory gained a bound-device symlink.
#
# Usage (on device): sh r23b_scan.sh <bus> <vid> <pid> <desc> [name]
#   desc: mouse|digi|kbd|cc|cc_vendor|rcon_ff1c|rcon_ff00|
#         rcon_ff1c_rid|rcon_ff00_rid|cc_rid|vendor_ff01|hex:<bytes>
#
# SAFETY: only UHID_CREATE2 with a well-formed descriptor; no input events;
# GET_REPORT answered with a well-formed error reply; clean destroy via kill
# (closing /dev/uhid fd makes the kernel destroy the device).
PROBE=/data/local/tmp/uhid_r23b
LOG=/data/local/tmp/r23b_one.log
BEFORE=/data/local/tmp/r23b_before.txt
AFTER=/data/local/tmp/r23b_after.txt

snapshot() {
  # NOTE: toybox `test -e` returns false on these sysfs symlinks (SELinux),
  # so filter unmatched globs by basename shape instead of -e.
  for d in /sys/bus/hid/drivers/*; do
    drv=${d##*/}
    for e in "$d"/[0-9A-F]*; do
      key=${e##*/}
      case "$key" in
        ????\:????\:????*) echo "$drv $key" ;;
      esac
    done
  done
}

bus=$1; vid=$2; pid=$3; desc=$4; name=${5:-vhid-r23b}
if [ -z "$desc" ]; then echo "usage: $0 <bus> <vid> <pid> <desc> [name]"; exit 2; fi

echo "=== CASE bus=$bus vid=$vid pid=$pid desc=$desc name=$name ==="
snapshot > "$BEFORE"
: > "$LOG"
$PROBE "$bus" "$vid" "$pid" "$desc" hold "$name" >"$LOG" 2>&1 &
p=$!
sleep 1.5
snapshot > "$AFTER"

echo "--- bound symlinks BEFORE ---"; cat "$BEFORE"
echo "--- bound symlinks AFTER ---"; cat "$AFTER"
echo "--- NEW symlinks (driver KEY) ---"
new=0
while IFS=' ' read -r drv key; do
  [ -z "$drv" ] && continue
  if ! grep -qxF "$drv $key" "$BEFORE"; then echo "BOUND_DRIVER=$drv KEY=$key"; new=$((new+1)); fi
done < "$AFTER"
[ "$new" -eq 0 ] && echo "(none)"

echo "--- harness event log ---"; cat "$LOG"

kill "$p" 2>/dev/null
sleep 1
echo "--- health after cleanup ---"
echo "boot_completed=$(getprop sys.boot_completed) uptime=$(cat /proc/uptime)"
echo "leftover bound:"; snapshot
echo "=== END CASE ==="
