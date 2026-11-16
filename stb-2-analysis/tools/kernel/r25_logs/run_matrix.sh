#!/bin/sh
# Phase 18 Round 25 run matrix. Host-side driver. Stops on first corruption.
set -u
DEV=192.168.1.119:5555
ADB="adb -s $DEV"
LOGDIR=tools/kernel/r25_logs
mkdir -p "$LOGDIR"

health() {
  echo "--- health ---"
  $ADB devices | grep 192.168.1.119 || return 1
  $ADB shell getprop sys.boot_completed | tr -d '\r' || return 1
  $ADB shell uptime | tr -d '\r' || return 1
}

run_one() {
  mode=$1; n=$2; seed=$3
  tag="m${mode}_n${n}_s${seed}"
  echo "===== RUN $tag ====="
  $ADB shell /data/local/tmp/pipe_spill_r25 129 "$n" "$mode" "$seed" 2>&1 | tee "$LOGDIR/$tag.log"
  if grep -q 'OOB_CORRUPTION' "$LOGDIR/$tag.log"; then
    echo "!!! CORRUPTION OBSERVED in $tag -- STOPPING ALL FURTHER RUNS !!!"
    return 99
  fi
  health || { echo "!!! DEVICE UNHEALTHY after $tag -- STOP !!!"; return 98; }
  return 0
}

for mode in 0 1; do
  for n in 200 300 400; do
    for seed in 0 1 2; do
      run_one "$mode" "$n" "$seed"
      rc=$?
      [ $rc -ne 0 ] && exit $rc
    done
  done
done
echo "ALL RUNS COMPLETED -- NO CORRUPTION"
exit 0
