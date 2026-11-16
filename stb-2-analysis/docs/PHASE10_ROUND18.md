# Phase 10, Round 18 — Virtual HID driver-binding probe on STB-2 (192.168.1.119:5555)

Date: 2026-11-17. Operator: `adb -s 192.168.1.119:5555 shell` (uid 2000, gid 2000,
groups incl. `uhid` 3011, `input` 1004).

## 0. Scope & safety

Goal: learn **which virtual HID device binds to which in-kernel HID driver** without
crashing the production STB (no recovered UART; a panic would brick it).

Rules actually followed:

* Only fixed, well-formed boot mouse / digitizer / keyboard report descriptors.
* No malformed/oversized descriptors, no report injection, no oversized
  `UHID_GET_REPORT_REPLY`. When a `UHID_GET_REPORT`/`UHID_SET_REPORT` arrives the
  harness replies with a **minimal well-formed error reply** (`err=1`, `size=0`,
  no data) — this avoids a driver probe hang without feeding it any report bytes.
  In practice **no GET_REPORT/SET_REPORT was ever requested** in any test below.
* One device at a time; each held briefly then destroyed via process exit (`close(fd)`).
* No reboot, no system/vendor/block-device writes, no flashing. Host writes only
  under `tools/uhid/`; device writes only under `/data/local/tmp/`.
* Device verified pristine at end: no `/sys/bus/hid/devices/` entries, no
  `/dev/hidraw*`, 8 pre-existing input devices, no leftover probe processes.

No crash, hang, or device unresponsiveness occurred. **Device remained healthy.**

## 1. Task D — kernel / build proof (read-only)

```
$ adb ... shell 'cat /proc/version'
Linux version 5.4.286-ab0600 (user@host) (Android (7284624, based on
r416183b) clang version 12.0.5 ...) #1 SMP PREEMPT Wed Jan 28 23:21:44 JST 2026

$ adb ... shell 'getprop ro.build.version.release'
14

$ adb ... exec-out 'zcat /proc/config.gz | grep -E "CONFIG_(HID|UHID|INPUT|HID_SEI_RCON)"'
CONFIG_INPUT=y
CONFIG_INPUT_EVDEV=y
CONFIG_INPUT_UINPUT=y
CONFIG_INPUT_SEI_KEYBOARD=y
CONFIG_INPUT_SEI_MOUSE=y
CONFIG_HID_SEI_RCON=y
... (UHID/HIDRAW/HID_GENERIC/HID_MULTITOUCH also =y; see tools/uhid/kernel_config.txt)
```

(Also confirms `CONFIG_HID_UHID`/`CONFIG_UHID` and `CONFIG_HIDRAW` built in —
consistent with Round 17, commit `67c5a74c`, doc `docs/PHASE9_ROUND17.md`.)

## 2. Discovering a working binding oracle

The Round-17 doc recorded `cat /sys/class/hidraw/hidraw0/device/uevent` as
SELinux-denied. Re-verified on a held device:

```
$ cat /sys/class/hidraw/hidraw0/device/uevent      -> No such file or directory
$ cat /sys/devices/virtual/misc/uhid/0003:.../uevent -> Permission denied
$ ls -la /sys/devices/virtual/misc/uhid/0003:.../   -> Permission denied
$ cat /sys/bus/hid/devices/.../modalias            -> Permission denied
$ cat /proc/bus/input/devices                      -> Permission denied
```

**The `uevent` DRIVER field is not reachable from `adb shell`.** However, two
readable oracles were found and used instead:

1. **Per-driver sysfs directory listing.** `/sys/bus/hid/drivers/<drv>/` contains
   one symlink per *bound* device. It is world-readable even though the device's
   own directory is not:
   ```
   $ ls -la /sys/bus/hid/drivers/hid-generic/
   lrwxrwxrwx ... 0003:1D6B:0001.0006 -> ../../../../devices/virtual/misc/uhid/0003:1D6B:0001.0006
   -????????? ... bind  new_id  uevent  unbind
   ```
   Scanning all 70 driver dirs for our device key gives the exact bound driver.
2. **`getevent -il`** lists input devices with `bus/vendor/product/name` and
   capability bitmap (works despite `/proc/bus/input/devices` being denied).
3. **`UHID_OPEN` event** from the harness indicates a HID protocol driver
   (not just hidraw) claimed the device.

Helper scripts pushed to the device: `tools/uhid/inspect_binding.sh`,
`tools/uhid/rcon_scan.sh`.

## 3. Task A — three well-formed virtual devices

Harness: `tools/uhid/uhid_bind.c` → `uhid_bind` (bare-metal armv7a static ELF,
raw `svc 0` syscalls). Usage:
`uhid_bind <bus> <vendor> <product> <mouse|digi|kbd> [hold]`.
It writes `UHID_CREATE2` (Linux 5.4 write/read protocol — no ioctl, per Round 17),
drains events, and (with `hold`) keeps the fd open for sysfs inspection.

Build (host):
```
clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os -o uhid_bind uhid_bind.c
```

### A1. USB mouse — bus `0x03`, VID `0x046d`, PID `0xc05a`, 50-byte boot mouse

```
[cfg] bus=0x00000003 vendor=0x0000046d product=0x0000c05a rd=boot mouse
open(/dev/uhid,O_RDWR) fd=3
write(CREATE2) ret=4376
  EVENT type=2 (UHID_START) dev_flags.l0=0x00000000 rawlen=4376
  EVENT type=4 (UHID_OPEN) rawlen=4376
```

```
$ ls /sys/bus/hid/devices/                -> 0003:046D:C05A.0008
$ sh inspect_binding.sh 0003:046D:C05A    -> BOUND_DRIVER=hid-generic
$ getevent -il                            -> bus 0003, vendor 046d, product c05a,
                                             name "vhid-r18" (new input node)
$ ls -l /dev/hidraw*                      -> /dev/hidraw0 (0600 root:root)
```
**UHID_GET_REPORT: not requested.**

### A2. Digitizer with ContactID — bus `0x03`, VID `0x1234`, PID `0x5678`

Descriptor (25 bytes, well-formed): Digitizer page `0x0D`, Usage Touch Screen
`0x04`, Finger `0x22`, usages ContactID `0x51`, X `0x30`, Y `0x31`, logical
0..255, 3 × 8-bit input. **No feature reports, no input reports injected.**

```
[cfg] bus=0x00000003 vendor=0x00001234 product=0x00005678 rd=digitizer/ContactID
write(CREATE2) ret=4376
  EVENT type=2 (UHID_START) ...
  EVENT type=4 (UHID_OPEN) ...
```

```
$ ls /sys/bus/hid/devices/                -> 0003:1234:5678.0009
$ sh inspect_binding.sh 0003:1234:5678    -> BOUND_DRIVER=hid-multitouch
$ getevent -il                            -> bus 0003, vendor 1234, product 5678,
                                             name "vhid-r18"
```
Notable: `hid-multitouch` claimed a **made-up VID/PID** — it matches on the
digitizer/touch report-descriptor signature, not on a VID/PID allow-list.
**UHID_GET_REPORT: not requested.**

### A3. Bluetooth keyboard — bus `0x05`, VID `0x1234`, PID `0x5679`, 63-byte boot keyboard

```
[cfg] bus=0x00000005 vendor=0x00001234 product=0x00005679 rd=boot keyboard
write(CREATE2) ret=4376
  EVENT type=2 (UHID_START) ...
  EVENT type=4 (UHID_OPEN) ...
```

```
$ ls /sys/bus/hid/devices/                -> 0005:1234:5679.000A
$ sh inspect_binding.sh 0005:1234:5679    -> BOUND_DRIVER=hid-generic
$ getevent -il                            -> bus 0005, vendor 1234, product 5679,
                                             name "vhid-r18"
$ ls -l /dev/hidraw*                      -> /dev/hidraw0
```
**UHID_GET_REPORT: not requested.**

## 4. Task B — trying to reach `hid-sei-rcon`

* Driver **is registered**: `/sys/bus/hid/drivers/sei-rcon/` exists (one of ~70
  built-in HID driver dirs). Kernel config has `CONFIG_HID_SEI_RCON=y`.
* Its VID/PID id-table is **not publicly documented**: web searches for
  `hid-sei-rcon`, `CONFIG_HID_SEI_RCON` source, and SEI VID/PID returned nothing
  relevant; no local source or `.ko` (driver is built-in); no readable
  `modalias`/`uevent`; `/proc/device-tree`, `/vendor`, `/system` contained no
  `rcon` VID/PID strings. The pre-existing input devices `sei-mouse` /
  `sei-keyboard` are **platform** `BUS_HOST(0x19)` devices, not HID devices.
* No broad wildcard match: USB `0000:0000` boot keyboard bound to `hid-generic`,
  not `sei-rcon`.

Batch probe (`tools/uhid/rcon_scan.sh`, all bus `0x03`, boot keyboard, 1 device
at a time, all cleanly destroyed):

| VID:PID | key | bound driver |
|---|---|---|
| 0bda:0001 | 0003:0BDA:0001.000C | hid-generic |
| 1a2c:0001 | 0003:1A2C:0001.000D | hid-generic |
| 04e8:0001 | 0003:04E8:0001.000E | hid-generic |
| 19d2:0001 | 0003:19D2:0001.000F | hid-generic |
| 1d6b:0001 | 0003:1D6B:0001.0010 | hid-generic |
| 05ac:0001 | 0003:05AC:0001.0011 | hid-generic |
| 046d:c52b | 0003:046D:C52B.0012 | NONE (scan timing; not sei-rcon) |
| 1234:5678 | 0003:1234:5678.0013 | hid-generic |
| ffff:ffff | 0003:FFFF:FFFF.0014 | hid-generic |
| 0001:0001 | 0003:0001:0001.0015 | hid-generic |
| 0bda:c05a | 0003:0BDA:C05A.0016 | hid-generic |
| 1b80:0001 | 0003:1B80:0001.0017 | hid-generic |
| 05ac:0234 | 0003:05AC:0234.0018 | hid-generic |
| 0451:0001 | 0003:0451:0001.0019 | hid-generic |

**Result: `hid-sei-rcon` was NOT reachable.** No candidate bound to it; the
driver has an exact (unknown) VID/PID match and no wildcard. Without the vendor
BSP source or the driver's id-table, its exact trigger cannot be determined from
the device surface. No exploit primitive was attempted.

## 5. Task C — `/dev/uinput` minimal probe

Harness `tools/uhid/uinput_probe.c` (open O_RDWR, UI_GET_VERSION, UI_SET_EVBIT,
UI_SET_KEYBIT, UI_DEV_SETUP, UI_DEV_CREATE, hold). No event injection.

```
$ /data/local/tmp/uinput_probe
open(/dev/uinput,O_RDWR) fd=3
ioctl(UI_GET_VERSION) ret=0 version=5
ioctl(UI_SET_EVBIT,EV_KEY) ret=0
ioctl(UI_SET_KEYBIT,KEY_A) ret=0
ioctl(UI_DEV_SETUP) ret=0
ioctl(UI_DEV_CREATE) ret=0
ioctl(UI_DEV_DESTROY) ret=0
```

**Vendor quirk discovered:** mainline `_IOW('U',100,int)` passes a *pointer* to
the bit value, but this kernel's `UI_SET_EVBIT`/`UI_SET_KEYBIT` return `-EINVAL`
for the pointer form and succeed when the **value is passed directly** as the
ioctl arg (verified with `tools/uhid/uinput_argtest.c`). `UI_DEV_SETUP`/CREATE/
DESTROY take pointers normally. `UI_GET_VERSION` returns 5 (real uinput).

Verified while held:

```
$ ls -l /dev/input/event*          -> new /dev/input/event8
$ getevent -il
  bus: 0003  vendor 1234  product 0002  version 0001
  name: "r18-uinput"
  events: KEY (0001): KEY_A
```

`/dev/uinput` is fully functional from `adb shell` (mode `crw-rw-rw- system:system
10,223`); a virtual input device with a real capability appears as `/dev/input/eventN`.

## 6. Verdict

| Virtual device | bus | VID:PID | descriptor | bound driver | UHID_GET_REPORT? |
|---|---|---|---|---|---|
| USB mouse | 0x03 | 046d:c05a | boot mouse | **hid-generic** | no |
| Digitizer/ContactID | 0x03 | 1234:5678 | digitizer | **hid-multitouch** | no |
| Bluetooth keyboard | 0x05 | 1234:5679 | boot keyboard | **hid-generic** | no |
| rcon candidates ×14 | 0x03 | various | boot keyboard | hid-generic (never sei-rcon) | no |

* `hid-generic` and `hid-multitouch` are reachable from shell-created UHID devices.
* `hid-sei-rcon` is registered but **not reachable** with any VID/PID tried; its
  exact id-table is unknown and no public/reachable source exists on-device.
* Every test produced `UHID_START` + `UHID_OPEN` (a real HID protocol driver
  claimed the device, not just hidraw) and a `/dev/hidraw0` + `/dev/input/eventN`.
* No `UHID_GET_REPORT`/`UHID_SET_REPORT` was ever requested, so no reply (and
  therefore no possibly-unsafe report data) was ever sent.
* Device remained healthy throughout; final state pristine (see §0).

## 7. Artifacts

Host (`tools/uhid/`):

* `uhid_bind.c` / `uhid_bind` — parameterized well-formed UHID creator + event logger.
* `uinput_probe.c` / `uinput_probe` — minimal uinput creator (direct-value EVBIT quirk).
* `uinput_argtest.c` / `uinput_argtest` — pointer-vs-direct ioctl arg test.
* `inspect_binding.sh` — scan `/sys/bus/hid/drivers/*/` for a bound device key.
* `rcon_scan.sh` — batch VID/PID binding scan.

Device (all under `/data/local/tmp/`, transient): `uhid_bind`, `uinput_probe`,
`uinput_argtest`, `inspect_binding.sh`, `rcon_scan.sh`, and `r18_*.log`.

References: Round 17 `docs/PHASE9_ROUND17.md`; kernel UAPI headers
`include/uapi/linux/{uhid,uinput}.h` v5.4 (read via searxng `web_url_read`).
