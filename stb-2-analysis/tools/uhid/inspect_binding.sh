#!/system/bin/sh
# Phase 10 Round 18 -- inspect which HID driver bound to a virtual device.
# Usage: sh inspect_binding.sh <bus:vid:pid prefix, e.g. 0003:046D:C05A>
# Runs on-device. Only reads sysfs symlink names (no SELinux-denied file reads).
key="$1"
echo "=== hid device keys on bus ==="
ls /sys/bus/hid/devices/ 2>&1
echo "=== binding scan for key '$key' ==="
for d in /sys/bus/hid/drivers/*; do
  drv="${d##*/}"
  for e in "$d"/*; do
    b="${e##*/}"
    case "$b" in
      "$key"*) echo "BOUND_DRIVER=$drv  entry=$b" ;;
    esac
  done
done
echo "=== matching /dev/input/event nodes (getevent) ==="
getevent -il 2>/dev/null | grep -i -E "vendor|product|name:|bus:" | head -20
