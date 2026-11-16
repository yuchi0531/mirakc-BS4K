#!/system/bin/sh
# Phase 10 Round 18, Task B: try a batch of candidate VID/PIDs and report
# which HID driver binds each one. Safe: only well-formed boot keyboards,
# no report injection, GET_REPORT answered with graceful error reply.
probe=/data/local/tmp/uhid_bind
for pair in \
  "0bda 0001" "1a2c 0001" "04e8 0001" "19d2 0001" "1d6b 0001" \
  "05ac 0001" "046d c52b" "1234 5678" "ffff ffff" "0001 0001" \
  "0bda c05a" "1b80 0001" "05ac 0234" "0451 0001"; do
  set -- $pair
  vid=$1; pid=$2
  $probe 3 "$vid" "$pid" kbd hold >/data/local/tmp/rcon_scan_one.log 2>&1 &
  p=$!
  sleep 1
  key="$(ls /sys/bus/hid/devices/ 2>/dev/null | head -1)"
  drv="NONE"
  for d in /sys/bus/hid/drivers/*; do
    for e in "$d"/*; do
      b="${e##*/}"
      case "$b" in "$key"*) drv="${d##*/}";; esac
    done
  done
  echo "VID=$vid PID=$pid key=$key -> DRIVER=$drv"
  kill "$p" 2>/dev/null
  sleep 1
done
