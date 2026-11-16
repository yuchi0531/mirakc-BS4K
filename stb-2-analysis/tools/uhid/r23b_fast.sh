#!/system/bin/sh
# Fast single-case binding scan. Args: <bus> <vid> <pid> <desc> [name]
# Prints one compact line: "bus vid pid desc -> DRIVER KEY | GR<...>"
snapshot() {
  for d in /sys/bus/hid/drivers/*; do
    drv=${d##*/}
    for e in "$d"/[0-9A-F]*; do
      key=${e##*/}
      case "$key" in ????\:????\:????*) echo "$drv $key" ;; esac
    done
  done
}
bus=$1; vid=$2; pid=$3; desc=$4; name=${5:-vhid-r23b}
snapshot > /data/local/tmp/r23b_fb.txt
/data/local/tmp/uhid_r23b "$bus" "$vid" "$pid" "$desc" hold "$name" \
  > /data/local/tmp/r23b_fl.log 2>&1 &
p=$!
sleep 0.4
snapshot > /data/local/tmp/r23b_fa.txt
drv=NONE; key=-
while IFS=' ' read -r d k; do
  [ -z "$d" ] && continue
  if ! grep -qxF "$d $k" /data/local/tmp/r23b_fb.txt; then drv=$d; key=$k; fi
done < /data/local/tmp/r23b_fa.txt
gr=$(grep 'UHID_GET_REPORT)' /data/local/tmp/r23b_fl.log | head -2 | tr '\n' ';')
kill "$p" 2>/dev/null
sleep 0.3
echo "$bus $vid $pid $desc '$name' -> $drv $key | GR=$gr | boot=$(getprop sys.boot_completed) up=$(cat /proc/uptime | cut -d' ' -f1)"
