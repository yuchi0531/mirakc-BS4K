# Phase 16 / Round 23B — SAFE descriptor-binding fuzz for `hid-sei-rcon`

Target: STB-2 at `adb -s 192.168.1.119:5555` (kernel 5.4.286-ab0600, Android 14,
`CONFIG_HID_SEI_RCON=y`). All work is local writes under `tools/uhid/` and this
document only. No reboot, no system/vendor/block writes, no flashing, no crafted
input, no malformed or oversized reports, no crashes.

## Goal

Discover the bind condition of the vendor `hid-sei-rcon` driver. Prior rounds
proved that a digitizer/ContactID descriptor binds `hid-multitouch` **by
descriptor signature** (any VID/PID), and that a 14-VID/PID probe list on bus=3
all fell through to `hid-generic`. This round fuzzed descriptors, buses, IDs,
names and finally swept the entire 16-bit VID space.

## 1. Reading the bound driver without `uevent`

`/sys/class/hidraw/*/device/uevent` is SELinux-denied. The working oracle is the
set of **per-driver device symlinks** under `/sys/bus/hid/drivers/<drv>/`, which
are world-readable. A bound HID device `B:VID:PID.NNNN` appears as a symlink in
exactly one driver directory.

Exact diff method (validated):

```sh
# BEFORE (baseline: empty on this device — pre-existing sei-mouse/sei-keyboard
# are BUS_HOST platform devices, not HID)
for d in /sys/bus/hid/drivers/*; do
  drv=${d##*/}
  for e in "$d"/[0-9A-F]*; do
    key=${e##*/}
    case "$key" in ????\:????\:????*) echo "$drv $key" ;; esac
  done
done > before.txt

# create ONE device with UHID_CREATE2 and hold the fd open; then:
... > after.txt
while IFS=' ' read -r drv key; do
  grep -qxF "$drv $key" before.txt || echo "BOUND_DRIVER=$drv KEY=$key"
done < after.txt
```

Gotcha: toybox `test -e` / `[ -e "$e" ]` returns **false** on these sysfs
symlinks (SELinux target-path denial), even though `ls -l` shows them. Do **not**
filter with `-e`; filter unmatched globs by the `????:????:????*` basename shape
as above. Verified example: mouse bus=3 046d:c05a → `hid-generic
0003:046D:C05A.0028`; digitizer bus=3 1234:5678 → `hid-multitouch
0003:1234:5678.002F`. Killing the harness closes `/dev/uhid`, which makes the
kernel destroy the device; the symlink disappears.

## 2. Harness

- `tools/uhid/uhid_r23b.c` → `uhid_r23b` (armv7a static bare-metal, 9316 B).
  `uhid_r23b <bus> <vid> <pid> <desc|hex:...> [hold] [name]`.
  Only well-formed descriptors; no `UHID_INPUT2`; `GET_REPORT`/`SET_REPORT`
  answered with an error reply (`err=1,size=0`) echoing the correct `id`.
- `tools/uhid/r23b_scan.sh` (full, ~2.5 s/case) and `r23b_fast.sh`
  (~1 s/case): snapshot → create → snapshot → diff → kill → health check.
- `tools/uhid/uhid_vidscan.c` → `uhid_vidscan`: in-process VID sweep; creates and
  destroys one well-formed keyboard device per VID and checks
  `/sys/bus/hid/drivers/sei-rcon/` directly with `getdents64` (~110 VID/s).

`sizeof(struct uhid_event)=4376`; `UHID_CREATE2=11`, `UHID_INPUT2=12`,
`UHID_GET_REPORT=9`; create2 layout name128/phys64/uniq64/rd_size@256/bus@258/
vendor@260/product@264/version@268/country@272/rd_data[4096]@276.

## 3. Results

### 3.1 Descriptor signatures at bus=3, VID:PID=1234:5678
All ten well-formed descriptors (`mouse`, `kbd`, `cc`, `cc_vendor`, `rcon_ff1c`,
`rcon_ff00`, `rcon_ff1c_rid`, `rcon_ff00_rid`, `cc_rid`, `vendor_ff01`) bound
**hid-generic**. Only `digi` (control) bound `hid-multitouch`. Consumer/vendor
descriptors with no standard usage produced no `UHID_OPEN` (hid-generic bound
but created no input device) — still visible as a symlink.

### 3.2 Bus variation (USB=3, BT=5, I2C=0x18, SPI=0x14, none=0, plus
PCI=1/2/4/6 and HOST=0x19)
`digi` → hid-multitouch on **every** bus; every other descriptor → hid-generic
on every bus. No `sei-rcon` bind.

### 3.3 Vendor usage-page sweep
All 256 pages `0xFF00–0xFFFF`, descriptor
`06 xx FF 09 01 A1 01 15 00 26 FF 00 75 08 95 08 81 02 C0`:
**256/256 → hid-generic**. No signature match.

### 3.4 Descriptor-shape sweep
8 usage pages (0x000C, 0x0001, 0x000D, 0x0020, 0xFF00, 0xFF1C, 0xFFA0, 0xFF80)
× report IDs {none,1,2,3} × input counts {1,4,8} = 96 cases: all → hid-generic.

### 3.5 Consumer usage sweep
All 256 Consumer-page usages `0x00–0xFF` (descriptor
`05 0C 09 xx A1 01 85 01 15 00 26 FF 00 75 08 95 01 81 02 C0`):
**256/256 → hid-generic**.

### 3.6 VID/PID sweeps
- 14 prior-round VID candidates: hid-generic.
- All **177** `Vendor_XXXX_Product_XXXX` pairs from `/system/usr/keylayout`,
  `/vendor/usr/keylayout`, `/system/usr/idc`, `/vendor/usr/idc`, bus=3, kbd:
  all hid-generic **except** known vendor drivers that correctly claimed their
  own IDs: `dragonrise` (0079:0011), `nintendo` (057e:2009), and several Sony/
  Logitech IDs that issued `UHID_GET_REPORT` at probe (rnum=242/129/9) and then
  failed probe (no symlink). This validates the oracle: ID-table binding and
  even probe-time GET_REPORT are observable. None was `sei-rcon`.
- Same 177 pairs on bus=0x18 (I2C) with `rcon_ff1c`: all hid-generic.
- VID 0x000d PID 0x3838 (from `/vendor/usr/keylayout/Vendor_000d_Product_3838.kl`):
  hid-generic with kbd/cc/rcon_ff1c/mouse.
- Full 16-bit VID sweep with PID=0x0001, bus=3, kbd: see §3.8.

### 3.7 Name / phys / uniq variation
Device names `SEI RCON`, `RCON`, `sei-rcon`, `Smart_Control_2016`,
`Smart_Remote_S`, `Remote Control`, `NexusIrHandler` (bus=3, 1234:5678,
`rcon_ff1c`): all → hid-generic. No name-based match.

### 3.8 Full VID sweeps
- **PID=0x0001, bus=3, keyboard:** all 65536 VIDs (`0x0000–0xFFFF`), in four
  bounded chunks — **no `sei-rcon` bind**.
- **PID=0xFFFF (`HID_ANY_ID`), bus=3, keyboard:** all 65536 VIDs — **no bind**.
- **Vendor page × usage grid:** pages `0xFF00–0xFF1F` × usages `0x00–0xFF`
  (8192 well-formed descriptors, report ID 1, 8-byte input, bus=3,
  1234:5678) via `uhid_descfuzz` — **no bind**.

## 4. Probe-time behavior of claiming drivers (task 3)

`hid-sei-rcon` never bound, so no driver-specific GET_REPORT could be attributed
to it. For comparison, the oracle did capture other drivers' probe requests:
`UHID_GET_REPORT id=1 rnum=242 rtype=0` (Sony DS3), `rnum=129 rtype=0` (Sony
DS4), `rnum=9 rtype=0` (DualSense), all answered with an `err=1,size=0` reply
echoing `id=1`; devices then closed cleanly. No malformed or oversized reply
was ever sent, and no input events were injected.

## 5. Safety / device health

Every case ended with `UHID_DESTROY` (or fd close via `kill`) and a health check.
For the bulk sweeps the in-process harness creates and destroys one well-formed
device per iteration and checks the target driver directory with `getdents64`.
Observed: `sys.boot_completed=1`, `uptime` monotonically increasing within each
session (e.g. 15090 → 16770 s, then 101 → 627 s after the incident below),
`adb devices` online, no leftover `B:VID:PID.NNNN` symlink after any case.
No system/vendor/block write, no flashing, no crafted input, no malformed or
oversized report was ever sent.

**Incident:** during the first (background) VID sweep the device went offline
and came back with `uptime` reset. `getprop` reported
`sys.boot.reason.last = reboot,cold.software` and logcat contained
`cast_shell: Daily reboot is enabled` — the device has a daily software-reboot
feature, so this was device-initiated rather than a kernel crash from a UHID
device. After reconnect (`adb connect`, already-connected state cleared) the
device was healthy (`boot_completed=1`, uptime advancing) with no orphan process
and no bound UHID device. All subsequent sweeps were run in bounded foreground
chunks with explicit before/after health checks and completed without incident.

## 6. Verdict

**No bind condition for `hid-sei-rcon` was found.** Exhaustive SAFE fuzzing —
10 class-typical + 96 shape-varied + 256 vendor-page + 256 consumer-usage
descriptors, 8192 vendor-page×usage descriptors, 10 bus types, 177 platform
VID/PID pairs on two buses, two full 16-bit VID sweeps (PID=1 and PID=FFFF),
and 7 device names — never produced a `sei-rcon` symlink. The only
non-`hid-generic` bindings observed were the known signature match
(`digi → hid-multitouch`, independent of bus/VID/PID) and ID-table matches for
other real drivers (`dragonrise` 0079:0011, `nintendo` 057e:2009, Sony/Logitech
IDs that issued probe-time `UHID_GET_REPORT`).

Conclusion: `hid-sei-rcon` has **no descriptor-signature match reachable from
UHID**; it is reachable only through a specific ID-table entry whose
VID/PID is not exposed to the shell (no readable `uevent`, `new_id`, `bind`,
`/sys/module/hid_sei_rcon`, `kallsyms` or `kcore`). The ID table cannot be
recovered by black-box descriptor fuzzing; the exact VID/PID remains unknown.
This is a negative result, not evidence that the driver is unreachable in
principle — it may bind to a physical RCON device whose IDs differ from every
value tested.
