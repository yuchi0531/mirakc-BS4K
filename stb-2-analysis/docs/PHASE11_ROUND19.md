# Phase 11, Round 19 — STB-2 UHID: crash/log oracle, trigger-path GET_REPORT recon, hid-sei-rcon IDs

Date: 2026-11-17. Target: `adb -s 192.168.1.119:5555` (online, Android 14, kernel
`5.4.286-ab0600`, shell `uid=2000(shell)` in groups incl. `3011(uhid)`).

## SAFETY (observed for the whole session)

- No reboot, no panic/exploit, no system/vendor/block writes, no flashing.
- Never sent a UHID_GET_REPORT_REPLY with a wrong/mismatched report ID and never
  triggered the 4095/4096-byte OOB `memset` in `hid_report_raw_event()`.
- Local writes only under `tools/uhid/` (`uhid_r19.c`, `uhid_r19`) and this doc.
  Device writes only `/data/local/tmp/uhid_r19`, `/data/local/tmp/r19.log` (both
  removed at the end).
- Baseline/final health: `adb devices` → `192.168.1.119:5555 device`;
  `getprop sys.boot_completed` → `1`; uptime monotonic 3:37 → 3:39 (no reboot).

Context (from prior rounds / static audit): CVE-2026-43047 in the device's exact
5.4.286 source — `hid-multitouch.ko`'s `mt_get_feature()` calls
`hid_report_raw_event()` on a buffer whose `buf[0]` comes from a UHID
GET_REPORT_REPLY; a reply naming the *large* feature report makes
`hid_report_raw_event()` `memset(cdata+csize, 0, rsize-csize)` past an 8-byte
kmalloc buffer. **This bug was NOT triggered.** The local source mirror
`refs/stblinux` is 7.1.0-rc2 (fixed code path), not the vulnerable 5.4.286.

---

## TASK A — crash / log oracle (read-only)

### A1. `dmesg`

```
$ adb shell dmesg 2>&1 | tail -20
dmesg: klogctl: Permission denied
```
Confirmed denied. SELinux audit from `logcat -b all -d`:
```
type=1400 audit(...): avc: denied { syslog_read } for comm="dmesg"
  scontext=u:r:shell:s0 tcontext=u:r:kernel:s0 tclass=system permissive=0
```
→ `syslog(2)`/`klogctl` is SELinux-blocked for shell.

### A2. kernel logcat buffer

```
$ adb logcat -b kernel -d -t 100 2>&1     -> (empty)
$ adb logcat -b kernel -d 2>&1            -> (empty), exit 0
$ adb logcat -g
  main:    ring buffer 256 KiB (249 KiB consumed)
  system:  ring buffer 256 KiB (81 KiB consumed)
  crash:   ring buffer 256 KiB (64 KiB consumed, 0 B readable)
  kernel:  ring buffer 256 KiB (0 B consumed, 0 B readable)
```
→ The `kernel` buffer exists but is **always empty** — logd is not draining
`/dev/kmsg` into it on this device, so it will not hold a future oops/panic either.

### A3. kmsg nodes

```
$ adb shell 'ls -l /proc/kmsg /dev/kmsg; head -c 100 /dev/kmsg 2>&1'
ls: /proc/kmsg: Permission denied
crw------- 1 root root 1, 11 2018-06-01 09:00 /dev/kmsg
head: /dev/kmsg: Permission denied
```
→ `/proc/kmsg` SELinux-denied (`avc: denied { getattr } ... tcontext=...proc_kmsg`),
`/dev/kmsg` is mode 0600 root:root (shell denied).

### A4. kernel sysctls

```
$ adb shell 'for f in panic panic_on_oops kptr_restrict dmesg_restrict; do
    printf "%s = " "$f"; cat /proc/sys/kernel/$f; done'
panic = cat: /proc/sys/kernel/panic: Permission denied
panic_on_oops = cat: /proc/sys/kernel/panic_on_oops: Permission denied
kptr_restrict = cat: /proc/sys/kernel/kptr_restrict: Permission denied
dmesg_restrict = cat: /proc/sys/kernel/dmesg_restrict: Permission denied
```
→ All four are unreadable from shell (SELinux on `/proc/sys/kernel`), so we
cannot even learn the panic/panic_on_oops settings.

### A5. `logcat -b all -d` for kernel events

```
$ adb logcat -b all -d 2>&1 | wc -l                -> 16765
$ adb logcat -b all -d 2>&1 | grep -i kernel ...   -> matches shown below
06-01 ... I auditd  : type=1400 audit(0.0:6): avc: denied { create } for
   comm="kdevtmpfs" ... scontext=u:r:kernel:s0 tcontext=u:object_r:device:s0 ...
11-17 03:42:29 ... I auditd  : type=1400 audit(0.0:73503): avc: denied
   { syslog_read } for comm="dmesg" scontext=u:r:shell:s0 ...
11-17 03:42:29 ... W dmesg   : type=1400 audit(...) ...
```
These are **SELinux AVC audit records** (delivered to logd via NETLINK_AUDIT and
written to the `main`/`system` buffers with tag `auditd`, or the triggering
comm as tag). The `u:r:kernel:s0` string is the *subject* of the audited action,
not a kernel-printk tag. **No `printk`/`dev_warn`/oops/panic text appears.**

### A-extra. alternate oracles

```
$ adb shell 'mount | grep pstore'
pstore on /sys/fs/pstore type pstore (rw,seclabel,...)
$ adb shell 'ls -la /sys/fs/pstore'   -> Permission denied
$ adb shell id    -> uid=2000(shell) ... ; su -> "su: inaccessible or not found"
$ adb shell 'getprop | grep -i logd'  -> (none)
```
pstore/ramoops is mounted but shell cannot read it; no root, no logd tuning
properties. So even a panic that wrote to pstore would not be readable here.

### TASK A ANSWER

**There is NO way to see kernel oops/panic messages from the shell after a
crash on this device.** Every direct channel is closed:

- `dmesg` → SELinux `syslog_read` denied.
- `/proc/kmsg` → SELinux denied; `/dev/kmsg` → 0600 root:root.
- `logcat -b kernel` → buffer exists but is permanently empty (logd not feeding it).
- `logcat -b all` → only SELinux audit records, never kernel printk text.
- `pstore`/`ramoops` → mounted but shell cannot read; `/proc/sys/kernel/*` → denied; no root.

The only *indirect* crash oracles are (a) **process-level**: the UHID harness's
`read()` on `/dev/uhid` failing / events ceasing, and (b) **device-level**: a
panic reboot detected by `adb devices` reconnecting, `uptime` resetting, and
`getprop sys.boot_completed`. Neither yields the actual oops text or a stack
trace. No exact "show me the oops" command exists.

---

## TASK B — non-destructive GET_REPORT / descriptor reconnaissance

### Harness

`tools/uhid/uhid_r19.c` → `uhid_r19`, bare-metal armv7a static ELF (raw `svc 0`,
per Round 17/18). Built with:

```
clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os -o uhid_r19 uhid_r19.c
```

Usage: `uhid_r19 <bus> <vendor> <product> [observe|errreply|okreply] [long]`.
It emits `UHID_CREATE2`, prints **every** event for 5 s (`long` → 30 s), then
sends `UHID_DESTROY` and drains `UHID_CLOSE`/`UHID_STOP`. Reply modes:
`observe` = no reply; `errreply` = `err=1,size=0`; `okreply` = `err=0,size=2,
data[0]=rnum` **only when `rnum==3`** (matching report ID). It *never* crafts a
reply that could select the large report 4, and never emits oversized data.

### Descriptor (64 bytes) — the trigger path, un-fired

```
05 0D             Usage Page (Digitizer)
09 04             Usage (Touch Screen)
A1 01             Collection (Application)
  85 01           Report ID (1)
  09 22           Usage (Finger)
  A1 02           Collection (Logical)
  09 51           Usage (ContactID)   -> HID_GROUP_MULTITOUCH scan hit
  09 30 09 31     Usage (X), Usage (Y)
  15 00 26 FF 00  Logical 0..255
  75 08 95 03     Size 8, Count 3
  81 02           Input (Data,Var,Abs)
  C0
  85 03           Report ID (3)
  09 55           Usage (Contact Count Maximum) = HID_DG_CONTACTMAX 0x000d0055
  15 00 26 FF 00 75 08 95 01 B1 02    Feature (Data,Var,Abs)  <-- mt_get_feature target
  85 04           Report ID (4)
  06 00 FF 09 01  Usage Page (Vendor 0xFF00), Usage 1
  15 00 26 FF 00 75 08 95 40 B1 02    64-byte Feature (Data,Var,Abs)  <-- LARGE report
C0
```
Chain (confirmed from source): `report_features()` (hid-input.c:2007) walks all
FEATURE reports at `hid_connect()` time → `mt_feature_mapping(CONTACTMAX)`
(hid-multitouch.c:551) → `mt_get_feature()` → `hid_hw_raw_request(..., id=3,
HID_FEATURE_REPORT, GET_REPORT)` → **kernel emits UHID_GET_REPORT**.

### B1 — observe mode (no replies at all)

```
$ adb shell /data/local/tmp/uhid_r19 3 1234 5678 observe
open(/dev/uhid,O_RDWR) fd=3
write(CREATE2) ret=4376
[pump] observing for 5s
  EVENT type=2 (UHID_START) flags=0x00000005 rawlen=4376
  EVENT type=9 (UHID_GET_REPORT) id=1 rnum=3 rtype=0 rawlen=4376
write(DESTROY) ret=4376
  EVENT type=3 (UHID_STOP) rawlen=4376
=== done ===
```
**Answer:** YES, the kernel issues `UHID_GET_REPORT(9)` at probe time — for
`rnum=3` (the ContactMax FEATURE report), `rtype=0`, correlation `id=1`.
Event types seen in 5 s: `START(2)`, `GET_REPORT(9)`, then after our DESTROY
`STOP(3)`. There is **no `UHID_OPEN(4)`** in observe mode: with no reply,
`mt_get_feature()` blocks in `hid_hw_raw_request()` waiting for the reply, so
`hid_connect()` never finishes and the device never opens. This is itself a
clean confirmation that the GET_REPORT is on the probe path.

### Field interpretation (verified against kernel source / GET_REPORT reply correlation)

`struct uhid_get_report_req { u32 id; u8 rnum; u8 rtype; }` on the 5.4.286 device:

- `id = 1`: UHID **request-correlation token** assigned by uhid's IDR. The reply
  MUST echo this in `get_report_reply.id` (confirmed by `uhid_dev_get_report_reply()`
  → `uhid_report_wake_up(..., ev->u.get_report_reply.id, ...)`).
- `rnum = 3`: the actual **report ID** requested — exactly the ContactMax feature.
- `rtype = 0`: a `enum uhid_report_type` value, where `UHID_FEATURE_REPORT=0`,
  `UHID_OUTPUT_REPORT=1`, `UHID_INPUT_REPORT=2` (NOT a `hid_report_type`). So
  `0` = **FEATURE** report request.
- `UHID_START flags = 0x00000005` = `UHID_DEV_NUMBERED_FEATURE_REPORTS (1<<0) |
  UHID_DEV_NUMBERED_INPUT_REPORTS (1<<2)` — kernel parsed our report IDs.

### B2 — safe reply variants (normal paths)

errreply (`err=1,size=0`):
```
  EVENT type=2 START ...
  EVENT type=9 GET_REPORT id=1 rnum=3 rtype=0
  -> GET_REPORT_REPLY err=1 size=0 id=1 ; reply write ret=4376
  EVENT type=4 UHID_OPEN
  write(DESTROY) ret=4376
  EVENT type=5 UHID_CLOSE ; EVENT type=3 UHID_STOP
```
okreply (`err=0,size=2,data[0]==rnum==3`, matching — no wrong-ID reply):
```
  EVENT type=2 START ...
  EVENT type=9 GET_REPORT id=1 rnum=3 rtype=0
  -> GET_REPORT_REPLY ok matching rnum=3 size=2 ; reply write ret=4376
  EVENT type=4 UHID_OPEN
  write(DESTROY) ret=4376
  EVENT type=5 UHID_CLOSE ; EVENT type=3 UHID_STOP
```
Both complete normally (`UHID_OPEN`) and destroy cleanly. Note: an initial build
of `okreply` keyed on `id==3` and therefore refused (id=1) — fixed to key on
`rnum==3`; this confirmed the field semantics above.

### Binding confirmation (supervised 30 s `okreply long` run)

```
$ adb shell 'nohup /data/local/tmp/uhid_r19 3 1234 5678 okreply long ... &'
$ adb shell 'ls -l /sys/bus/hid/drivers/hid-multitouch/'
lrwxrwxrwx ... 0003:1234:5678.001E -> ../../../../devices/virtual/misc/uhid/0003:1234:5678.001E
$ adb shell getevent -il   -> add device /dev/input/event8, vendor 1234, name "vhid-r19"
```
After the harness self-destroyed: `/sys/bus/hid/devices/` empty,
`hid-multitouch/` has no device symlink, `/dev/input/event8` gone.

### Health after every B test

`adb devices` online; `sys.boot_completed=1`; `uptime` 3:37 → 3:39 (monotonic,
no reboot). Temp binaries/logs removed from `/data/local/tmp`. No exploitation
occurred and the device is back to its pristine state.

---

## TASK C — hid-sei-rcon ID discovery (read-only)

### C1. HID driver directories

```
$ adb shell 'ls -l /sys/bus/hid/drivers/'
```
~70 built-in HID driver dirs registered, including `hid-generic`,
`hid-multitouch`, `hid-sensor-hub`, `hid-picolcd`, and **`sei-rcon`** (driver
name `sei-rcon`, config `CONFIG_HID_SEI_RCON=y`). Every driver dir exposes only
the standard `bind`, `unbind`, `new_id`, `uevent` attributes (shown as
`-?????????` because shell cannot stat them); each `uevent` is `Permission denied`.

### C2. sei-rcon dir contents

```
$ adb shell 'ls -la /sys/bus/hid/drivers/sei-rcon/'
bind   new_id   uevent   unbind        (all: Permission denied on read)
$ adb shell 'cat /sys/bus/hid/drivers/sei-rcon/{bind,new_id,uevent,unbind}'
head: .../bind: Permission denied
head: .../new_id: Permission denied
head: .../uevent: Permission denied
head: .../unbind: Permission denied
$ adb shell 'ls /sys/bus/hid/drivers/sei-rcon/module'      -> No such file or directory
$ adb shell 'ls /sys/module/hid_sei_rcon'                 -> No such file or directory
$ adb shell 'find /sys/module -iname "*sei*"'             -> (nothing)
```
No `id`/`ids`/ID-table attribute is exposed; the driver has no `module` symlink
and no `/sys/module/hid_sei_rcon` parameter directory.

### C3. IDs found

**None.** No vendor/product table for `hid-sei-rcon` is readable from the shell.
The only `new_id` file is write-only and also permission-denied. (Prior Round 18
additionally tried 14 candidate VID/PIDs via UHID_CREATE2; all bound to
`hid-generic`, never `sei-rcon`.) No source for this custom vendor driver exists
in the workspace or in the upstream-form `refs/stblinux` mirror.

---

## Final answers

1. **Crash oracle:** none. Kernel oops/panic text is unreachable from shell
   (dmesg, /proc/kmsg, /dev/kmsg, logcat kernel, pstore all blocked/empty). Only
   indirect liveness signals exist (UHID fd errors / event silence; adb reboot +
   uptime reset). No exact command can display an oops.
2. **GET_REPORT details:** probe-time `UHID_GET_REPORT(9)` IS issued for the
   ContactMax FEATURE report: `rnum=3` (report ID), `rtype=0` (UHID_FEATURE_REPORT),
   `id=1` (correlation token echoed in the reply). Events in first 5 s:
   `START(2)`, `GET_REPORT(9)` (and `STOP(3)` after our clean DESTROY); no OPEN
   unless a reply is supplied. err-reply and matching-ID reply both reach
   `UHID_OPEN` cleanly. Trigger path confirmed without firing the bug.
3. **hid-sei-rcon IDs:** none readable — driver registered but all sysfs files
   (`bind`/`unbind`/`new_id`/`uevent`) are permission-denied, no ID table and no
   `/sys/module` entry.

## Changed files

- `tools/uhid/uhid_r19.c` (new), `tools/uhid/uhid_r19` (built binary)
- `docs/PHASE11_ROUND19.md` (this file)
