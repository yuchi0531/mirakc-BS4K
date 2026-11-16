# Phase 9, Round 17 — UHID software-LPE reconnaissance on STB-2

Date: 2026-11-17
Device: `adb -s 192.168.1.119:5555` (online, healthy before/after)
Working dir: `stb-2-analysis`
Safety observed: **no reboot, no writes to /system, /vendor, /block devices, no flashing,
no factory reset.** Device writes limited to `/data/local/tmp`. Local writes limited to
`tools/uhid/` and this document. Virtual HID creation via `/dev/uhid` is the intended
experiment (normal kernel API). All virtual devices were destroyed/closed afterwards.

---

## 0. Executive verdict (also at end)

**Yes — `adb shell` (uid 2000) can create a virtual HID device on this STB-2.**
It can open `/dev/uhid` O_RDWR, issue `UHID_CREATE2`, receive `UHID_START` (and
`UHID_OPEN` with a complete descriptor), inject `UHID_INPUT2`, and make the kernel
create `/dev/hidraw0`. This is confirmed experimentally, not inferred.

**However**, the resulting `/dev/hidraw0` is `crw------- root:root 244,0` and
shell gets **EACCES** opening it, so the classic "uhid → crafted descriptor → open
/dev/hidrawX → kernel bug" chain is blocked at the last step on this build.

**No confirmed LPE-grade HID bug was identified as reachable.** The obvious
Dec-2024 HID info leak (CVE-2024-50302) is already fixed in 5.4.286. The
2025/2026 HID bugs found are either DoS-only with both KASAN and UBSAN compiled
out, require `CONFIG_HID_PID`/`CONFIG_HID_UNIVERSAL_PIDFF` (not set), or require
opening `/dev/hidrawX` (EACCES). Best remaining lead is the very large set of
**built-in HID drivers** (`=y`, ~70 of them) probeable with arbitrary descriptors
via UHID_CREATE2 — a fuzzing target, not a known exploit.

The task's proposed `ioctl(_IOW('U',0x11,...))` is **obsolete for this kernel**:
Linux 5.4's `uhid_fops` has no `.ioctl` handler and returns **ENOTTY (25)**.
The working protocol is `write()`/`read()` of `struct uhid_event`.

---

## 1. Device and kernel facts (exact)

```
$ adb -s 192.168.1.119:5555 shell 'uname -a; cat /proc/version'
Linux localhost 5.4.286-ab0600 #1 SMP PREEMPT Wed Jan 28 23:21:44 JST 2026 armv8l Toybox
Linux version 5.4.286-ab0600 (user@host) ... clang version 12.0.5 ... #1 SMP PREEMPT Wed Jan 28 23:21:44 JST 2026

$ adb -s ... shell id
uid=2000(shell) gid=2000(shell) groups=2000(shell),1004(input),1007(log),1011(adb),
1015(sdcard_rw),1028(sdcard_r),1078(ext_data_rw),1079(ext_obb_rw),
3001(net_bt_admin),3002(net_bt),3003(inet),3006(net_bw_stats),3009(readproc),
3011(uhid),3012(readtracefs) context=u:r:shell:s0

$ adb -s ... shell 'getenforce; ls -l /dev/uhid'
Enforcing
crw-rw---- 1 uhid uhid 10, 239 ... /dev/uhid
```

Two different dates matter and were initially conflated:

* **Upstream base = Linux 5.4.286, released 2024-11-17** (kernel.org tar date;
  ChangeLog-5.4.286 dated 2024-11-06). This is the "2024-11-17" in the task.
* **Vendor build = 2026-01-28** (`#1 SMP PREEMPT Wed Jan 28 ... 2026`), i.e. a
  vendor kernel carrying its own patches on top of the Nov-2024 LTS base.
  It is *not* a fully current 2026 kernel.

`/proc/config.gz` is readable and was copied to `tools/uhid/kernel_config.gz`
(decompressed: `tools/uhid/kernel_config.txt`, 6406 lines). Relevant options:

```
CONFIG_HID=y
CONFIG_HIDRAW=y
CONFIG_UHID=y
CONFIG_INPUT=y / CONFIG_INPUT_EVDEV=y / CONFIG_INPUT_UINPUT=y
CONFIG_HID_GENERIC=y
CONFIG_HID_MULTITOUCH=y
# CONFIG_HID_PID is not set
# CONFIG_KASAN is not set
# CONFIG_UBSAN is not set
CONFIG_SECURITY_SELINUX=y
```

~70 HID protocol drivers are compiled in (`=y`): A4TECH, APPLE, CHICONY,
DRAGONRISE, EMS_FF, GYRATION, HOLTEK, LENOVO, LOGITECH, MAGICMOUSE, MICROSOFT,
MONTEREY, MULTITOUCH, NINTENDO, PANTHERLORD, PETA Lynx, PICOLCD, PLAYSTATION,
ROCCAT, SAITEK, SAMSUNG, **SEI_RCON (vendor)**, SENSOR_HUB, SONY, WACOM, WIIMOTE,
ZEROPLUS, ZYDACRON, … (full list in `kernel_config.txt`). `CONFIG_SEI_RCON` is a
custom vendor HID driver. Loaded modules (`/proc/modules`): `bcmdhd`, `nexus`,
`brcmv3d`, `bgem`, `nexusmem`, `droid_pm`, `stbmisc`, `hddidentify`, `xfs` —
no HID modules loaded (all built-in).

Raw config was extracted with:
```
adb -s ... exec-out 'cat /proc/config.gz' > tools/uhid/kernel_config.gz
```

---

## 2. Task A — confirm `/dev/uhid` access from shell

Source: `tools/uhid/uhid_open.c`. Bare-metal armv7 ELF, no libc, raw `svc 0`
syscalls (`openat`=322, `close`=6, `write`=4, `exit`=1). Volatile helpers avoid
clang folding loops into `strlen`/`__aeabi_memclr`.

Build / push / run:
```
cd tools/uhid
clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os -o uhid_open uhid_open.c
adb -s ... push uhid_open /data/local/tmp/uhid_open
adb -s ... shell 'chmod 755 /data/local/tmp/uhid_open; /data/local/tmp/uhid_open'
```

Output:
```
=== Task A: /dev/uhid open test ===
open(/dev/uhid,O_RDONLY) fd=3 errno=0
open(/dev/uhid,O_RDWR) fd=3 errno=0
(errno 13=EACCES 1=EPERM 2=ENOENT 19=ENODEV)
exit=0
```

**Result: DAC *and* SELinux both permit shell to open `/dev/uhid` O_RDWR.**
No AVC denial observed. Task A complete.

---

## 3. Task B — UHID harness

### 3.1 UAPI verification (fetched, not guessed)

Sources:
* `https://raw.githubusercontent.com/torvalds/linux/v5.4/include/uapi/linux/uhid.h`
* `https://raw.githubusercontent.com/torvalds/linux/v5.4/drivers/hid/uhid.c`

`drivers/hid/uhid.c` `uhid_fops` is:
```c
static const struct file_operations uhid_fops = {
    .owner = THIS_MODULE, .open = uhid_char_open, .release = uhid_char_release,
    .read = uhid_char_read, .write = uhid_char_write,
    .poll = uhid_char_poll, .llseek = no_llseek,
};
```
**There is no `.ioctl` / `.unlocked_ioctl` / `.compat_ioctl`.** The legacy
ioctl interface was removed years ago. `uhid_char_write()` dispatches on
`uhid->input_buf.type` with `case UHID_CREATE2`, `UHID_INPUT2`, `UHID_DESTROY`, …
after `copy_from_user(event, buffer, min(len, sizeof(*event)))`.
`uhid_char_read()` returns queued `struct uhid_event`s (`UHID_START`, `UHID_OPEN`,
`UHID_STOP`, `UHID_CLOSE`, `UHID_OUTPUT`, report requests) via `poll()`.

### 3.2 Exact struct layouts used (Linux 5.4, `__attribute__((packed))`)

`struct uhid_create2_req` — **4372 bytes (0x1114)**:

| offset | field | type | size |
|---|---|---|---|
| 0 | `name` | `__u8[128]` | 128 |
| 128 | `phys` | `__u8[64]` | 64 |
| 192 | `uniq` | `__u8[64]` | 64 |
| 256 | `rd_size` | `__u16` | 2 |
| 258 | `bus` | `__u16` | 2 |
| 260 | `vendor` | `__u32` | 4 |
| 264 | `product` | `__u32` | 4 |
| 268 | `version` | `__u32` | 4 |
| 272 | `country` | `__u32` | 4 |
| 276 | `rd_data` | `__u8[4096]` (`HID_MAX_DESCRIPTOR_SIZE`) | 4096 |

`struct uhid_input2_req` — **4098 bytes (0x1002)**: `u16 size @0`, `u8 data[4096] @2`.

`struct uhid_start_req`: `u64 dev_flags` (8 bytes).

`struct uhid_output_req`: `u8 data[4096] @0`, `u16 size @4096`, `u8 rtype @4098` (4099 bytes).

`struct uhid_event` — **4376 bytes (0x1118)**: `u32 type @0`, packed union `u @4`
(largest member = `uhid_create2_req`, so `4 + 4372 = 4376`).

`enum uhid_event_type` (**these are the `ev.type` values on write/read**):

| value | name | value | name |
|---|---|---|---|
| 0 | `__UHID_LEGACY_CREATE` | 8 | `__UHID_LEGACY_INPUT` |
| 1 | `UHID_DESTROY` | 9 | `UHID_GET_REPORT` |
| 2 | `UHID_START` | 10 | `UHID_GET_REPORT_REPLY` |
| 3 | `UHID_STOP` | 11 | **`UHID_CREATE2`** |
| 4 | `UHID_OPEN` | 12 | **`UHID_INPUT2`** |
| 5 | `UHID_CLOSE` | 13 | `UHID_SET_REPORT` |
| 6 | `UHID_OUTPUT` | 14 | `UHID_SET_REPORT_REPLY` |
| 7 | `__UHID_LEGACY_OUTPUT_EV` | | |

### 3.3 Exact ioctl numbers (legacy, computed with host `_IOW`, 14-bit size field)

```
struct uhid_create2_req = 4372 bytes (0x1114)
struct uhid_input2_req  = 4098 bytes (0x1002)

UHID_CREATE2 = _IOW('U',0x11,struct uhid_create2_req) = 0x51145511 = 1360286993
UHID_INPUT2  = _IOW('U',0x12,struct uhid_input2_req)  = 0x50025512 = 1342330130
legacy UHID_CREATE = _IOW('U',0x10,char *)           = 0x40085510
```
Breakdown for 0x51145511: `_IOC_WRITE(1)<<30 | 'U'(0x55)<<8 | 0x11 | 0x1114<<16`.
These numbers are the ones the task asked for; they are correct *as numbers*, but
**unusable on this kernel** — see 3.4.

Host verification snippet lived at `/tmp/uhid_ioctl.c` (not in repo).

### 3.4 Harness

Source: `tools/uhid/uhid_harness.c`.
Raw ARM syscalls used: `exit=1, read=3, write=4, close=6, ioctl=54, poll=168, openat=322`.

```
clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os -o uhid_harness uhid_harness.c
clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os -DRD_FULL -o uhid_harness_full uhid_harness.c
clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os -DHOLD -o uhid_hold uhid_harness.c
adb -s ... push uhid_harness{,_full} /data/local/tmp/
adb -s ... shell 'chmod 755 /data/local/tmp/uhid_harness*'
```

Descriptor A (default, 13 bytes, exactly the task's suggestion):
`05 01 09 02 A1 01 95 03 75 08 81 06 C0`
— UsagePage(GenericDesktop), Usage(Mouse), Collection(Application),
ReportCount(3), ReportSize(8), Input(Data,Var,Abs), EndCollection.

Descriptor B (`-DRD_FULL`, 50 bytes): standards-complete boot mouse with 3 button
bits + 5 padding + X/Y relative 8-bit.

### 3.5 Run output — minimal descriptor

```
$ /data/local/tmp/uhid_harness
=== UhID harness (Linux 5.4 protocol: write/read struct uhid_event) ===
open(/dev/uhid,O_RDWR) fd=3 errno=0
ioctl(fd, UHID_CREATE2=0x51145511, ...) ret=-25 errno=25 (25=ENOTTY expected: ioctl interface removed in modern uhid)
[descriptor] simple 13-byte mouse
sizeof(uhid_event)=4376 rd_size=13
write(CREATE2) ret=4376 errno=0
[post-CREATE2] read ret=4376
  EVENT type=2 (UHID_START) dev_flags=0x00000000 rawlen=4376
[post-CREATE2] poll ret=0 -> no more events
write(INPUT2 size=3) ret=4376 errno=0
[post-INPUT2] poll ret=0 -> no more events
write(DESTROY) ret=4376 errno=0
=== done ===
exit=0
```

Note: `write()` returns the full `count` (4376), as the driver documents
("return count not len to not confuse the caller"). `read()` also returns 4376
(the whole queued `struct uhid_event`). `UHID_INPUT2` produces **no echo event**
— it is strictly userspace→kernel; the kernel may turn it into `UHID_OUTPUT`
only if a HID driver issues an output report, which this descriptor does not.

### 3.6 Run output — full descriptor

```
=== UhID harness (Linux 5.4 protocol: write/read struct uhid_event) ===
open(/dev/uhid,O_RDWR) fd=3 errno=0
ioctl(fd, UHID_CREATE2=0x51145511, ...) ret=-25 errno=25
[descriptor] full 74-byte boot mouse        <-- harness label; actual rd_size=50 (see 3.4)
sizeof(uhid_event)=4376 rd_size=50
write(CREATE2) ret=4376 errno=0
[post-CREATE2] read ret=4376
  EVENT type=2 (UHID_START) dev_flags=0x00000000 rawlen=4376
[post-CREATE2] read ret=4376
  EVENT type=4 (UHID_OPEN) rawlen=4376
[post-CREATE2] poll ret=0 -> no more events
write(INPUT2 size=3) ret=4376 errno=0
[post-INPUT2] poll ret=0 -> no more events
write(DESTROY) ret=4376 errno=0
=== done ===
exit=0
```
(The `74-byte` label was a source typo; corrected to 50 bytes and both binaries
rebuilt. Runtime `rd_size=50` is authoritative.)

`UHID_OPEN` with the full descriptor means a HID driver (hid-generic/hid-input)
actually claimed the device; with the minimal descriptor only hidraw bound.

### 3.7 Live kernel-side verification

Ran `uhid_hold` (same harness, holds the fd open after INPUT2) in the background
and inspected from a second shell:

```
$ nohup /data/local/tmp/uhid_hold > /data/local/tmp/uhid_hold.log 2>&1 &

$ ls -l /sys/class/hidraw/
total 0
lrwxrwxrwx 1 root root 0 ... hidraw0 -> ../../devices/virtual/misc/uhid/0003:1D6B:0001.0004/hidraw/hidraw0
$ ls -l /dev/hidraw*
crw------- 1 root root 244,   0 ... /dev/hidraw0
$ ls -l /sys/bus/hid/devices/
lrwxrwxrwx 1 root root 0 ... 0003:1D6B:0001.0004 -> ../../../devices/virtual/misc/uhid/0003:1D6B:0001.0004
```

So the kernel registered a real HID device: bus `0003` (USB), vendor `1D6B`
(Linux Foundation), product `0001`, instance `.0004`, with a hidraw child.
**UHID demonstrably works.**

`dmesg` is blocked (`klogctl: Permission denied`), `logcat -b kernel -d` returns
nothing, and HID sysfs attributes are SELinux-denied to shell:
```
$ cat /sys/class/hidraw/hidraw0/device/uevent   -> Permission denied
$ cat /sys/bus/hid/devices/.../modalias         -> Permission denied
$ cat /proc/bus/input/devices                   -> Permission denied
```
Hence no kernel log evidence could be captured; the sysfs/devnode appearance is
the Proof.

Shell cannot open the resulting hidraw node:
```
$ /data/local/tmp/dev_open        # -DDEV_PATH='"/dev/hidraw0"'
open(/dev/hidraw0,O_RDWR) fd=-13 errno=13 (13=EACCES 1=EPERM 19=ENODEV)
```
Reason: mode `0600 root:root` (devtmpfs default; no ueventd rule for virtual
HID), not SELinux on the char node itself.

Cleanup: killed PID 18989 (`uhid_hold`); `/sys/class/hidraw/` went back to empty,
`/dev/hidraw*` absent. Device remained online (`uptime` OK).

---

## 4. Task C — device-node enumeration

Raw dumps saved to `tools/uhid/dev_enumerate.txt` (733 lines) — note toybox
`find /dev -maxdepth 2 -ls` fails with `find: bad arg '(null)'`, so directory
listings were collected per-directory instead.

### 4.1 Nodes whose mode+group match shell's groups (DAC-openable candidates)

Shell supplementary GIDs: `1004 input, 1007 log, 1011 adb, 1015 sdcard_rw,
1028 sdcard_r, 1078 ext_data_rw, 1079 ext_obb_rw, 3001 net_bt_admin, 3002 net_bt,
3003 inet, 3006 net_bw_stats, 3009 readproc, 3011 uhid, 3012 readtracefs`.
Primary/egid: `2000 shell`.

| node | mode | owner:group (gid) | matches shell? |
|---|---|---|---|
| `/dev/uhid` | 0660 | uhid:uhid (3011) | **yes** — confirmed open |
| `/dev/input/event0`..`event7` | 0660 | root:input (1004) | **yes** |
| `/dev/uinput` | 0666 | system:system (1000) | yes (world) |
| `/dev/binder` → `/dev/binderfs/binder` | 0777 symlink | root:root | yes (symlink; binderfs dir itself `Permission denied`) |
| `/dev/nexusmem` | 0666 | system:system | yes (world) |
| `/dev/nxmw`, `/dev/nxwk`, `/dev/bcmpip` | 0666 | system:system | yes (world) |
| `/dev/ion` | 0664 | system:system | yes (world-readable) |
| `/dev/hddidentify` | 0664 | system:vendor_stbcore | "other" read bit only |
| `/dev/ashmem`, `ashmem1c2f…` | 0666 | root:root | yes (world) |
| `/dev/dri/card0`, `card1` | 0666 | system:system | yes (world) |
| `/dev/dri/renderD128` | 0660 | root:graphics (1003) | **no** by gid — see 4.3 |
| `/dev/full`, `null`, `ptmx`, `random`, `tty`, `urandom`, `zero` | 0666 | root:root | yes (world) |
| `/dev/watchdog`, `/dev/mmcblk0rpmb` | 0666 | system:system | yes by mode (not opened) |
| `/dev/hdmi_hpd` | 0660 | system:camera | no |
| `/dev/btusb0`, `/dev/ttyS1`, `/dev/ttyS2` | 0660 | bluetooth:bluetooth | no |
| `/dev/ppp`, `/dev/tun` | 0660 | radio:vpn / system:vpn | no |
| `/dev/snd/timer` | 0660 | system:audio | no |
| `/dev/rtc0` | 0640 | system:system | no |
| `/dev/hw_random` | 0400 | prng_seeder:prng_seeder | no |
| `/dev/block/loop*`, `/dev/tty*`, `/dev/pt*` | 0600 | root:root | no |
| `/dev/gpiochip0..6`, `/dev/kmsg`, `/dev/fuse`, `/dev/vsock`, `/dev/device-mapper`, `/dev/brcm_cma0`, `/dev/loop-control`, `/dev/ubi_ctrl`, `/dev/rfkill`, `/dev/watchdog0`, `/dev/vcs*` | 0600 | root:root | no |

Directories: `/dev/usb-ffs/adb` is `drwxrwx--- shell:shell` (shell-owned);
`/dev/fscklogs`, `/dev/pts`, `/dev/binderfs` are `Permission denied`.

### 4.2 Open-only probe (no ioctl/read/write on any node)

`tools/uhid/nodes_open.c`, raw `openat` only:

```
/dev/uhid          RDWR:fd=3    err=0
/dev/uinput        RDWR:fd=3    err=0
/dev/input/event0  RDWR:fd=-13 err=13  RDONLY:fd=3 err=0
/dev/input/event6  RDWR:fd=-13 err=13  RDONLY:fd=3 err=0
/dev/dri/card0     RDWR:fd=3    err=0
/dev/dri/renderD128 RDWR:fd=3   err=0     <-- see 4.3
/dev/nexusmem      RDWR:fd=3    err=0
/dev/nxmw          RDWR:fd=3    err=0
/dev/nxwk          RDWR:fd=-13 err=13  RDONLY:fd=-13 err=13
/dev/bcmpip        RDWR:fd=-13 err=13  RDONLY:fd=-13 err=13
/dev/ion           RDWR:fd=-13 err=13  RDONLY:fd=3 err=0
/dev/hddidentify   RDWR:fd=-13 err=13  RDONLY:fd=-13 err=13
/dev/binder        RDWR:fd=3    err=0
/dev/ashmem        RDWR:fd=-13 err=13  RDONLY:fd=-13 err=13
```

Interpretation: DAC mode alone is not decisive; SELinux filter is applied per
node. `/dev/input/event*` are **readable by shell** (group `input`) but SELinux
blocks write — a potential keystroke/touch info-leak surface already open to
`adb shell`. `nexusmem`/`nxmw` open fine (matches Round 15: EINVAL/ENOSYS on
later ioctl), while `bcmpip`/`nxwk`/`hddidentify` are SELinux-denied (also
matches Round 15). `/dev/ashmem` is EACCES despite 0666 (SELinux).

### 4.3 Anomaly: `/dev/dri/renderD128`

`ls -ln` reports `crw-rw---- 1 0 1003 226,128` (gid 1003 = graphics), and
`/proc/self/status` shows shell groups `1004 1007 1011 1015 1028 1078 1079
3001 3002 3003 3006 3009 3011 3012` — **1003 is absent** — yet both the ELF and
a plain shell `exec 9<>/dev/dri/renderD128` succeed. This is likely a POSIX ACL
or vendor-specific devtmpfs permission injected outside the mode bits. Recorded
as observed; not investigated further (out of scope).

### 4.4 Explicitly requested groups

```
$ ls -l /dev/binder* /dev/*nexus* /dev/*stb* /dev/hidraw*
/dev/binder -> /dev/binderfs/binder                          (0777 symlink; target dir denied)
/dev/nexusmem  crw-rw-rw- 1 system system 10,117             (open OK)
ls: /dev/*stb*: No such file or directory
ls: /dev/hidraw*: No such file or directory   (only exists while a UHID device is held)
```
Related nexus-family nodes (not matching `*nexus*`): `/dev/bcmpip`, `/dev/nxmw`,
`/dev/nxwk` (all 0666 system:system; open results in 4.2). No `/dev/*stb*` at all;
the STB-specific surface is the `stbmisc`/`hddidentify`/`nexus` char devices and
the Nexus/stbcore HALs documented in earlier rounds.

---

## 5. HID-bug triage for this exact kernel

Base 5.4.286 = upstream 2024-11-17; vendor build 2026-01-28. KASAN and UBSAN are
**not** compiled in; no HID force-feedback core (`# CONFIG_HID_PID is not set`,
no `CONFIG_HID_UNIVERSAL_PIDFF`).

| CVE / bug | relevance to base 5.4.286 | reachable here? |
|---|---|---|
| **CVE-2024-50302** — HID core report buffer not zero-initialized → kernel-memory leak via crafted report (fixed 2024-11) | **Fixed in 5.4.286** (OpenCVE: 5.4.286 unaffected) | No. |
| **CVE-2024-53104** — uvcvideo `UVC_VS_UNDEFINED` OOB write (CISA KEV) | Not in 5.4 (fixed in 5.15.170/6.x branches) and needs UVC USB video, not HID | No. |
| **CVE-2025-38556** — `s32ton()` shift-out-of-bounds on `Report Size 0`, triggerable by a UHID-created descriptor | Unpatched (2025 fix), but **DoS/UB only**; with `CONFIG_UBSAN=n` there is no trap, so effect is undefined-but-usually-silent | Theoretically yes via UHID_CREATE2; **not tested** (would risk device stability for no LPE gain). |
| **CVE-2025-38494** — HID callers bypass `hid_hw_raw_request()` validation → in-kernel OOB/LPE | Unpatched (2025 fix), but exploit path is in-kernel call sites, not directly a UHID userspace input | No direct route. |
| **CVE-2026-89624** — `hid-universal-pidff` UAF: PID-usage descriptor w/o input reports → surviving `/dev/hidrawX` → UAF write on open | Unpatched (2026) but requires `CONFIG_HID_UNIVERSAL_PIDFF` and opening `/dev/hidrawX` | **No** on both counts: option absent, and `/dev/hidraw0` is 0600 root:root → EACCES. |
| Unknown bugs in ~70 built-in HID drivers (incl. custom `CONFIG_HID_SEI_RCON`) reachable by feeding arbitrary descriptors through UHID_CREATE2 | Plausible; large untested attack surface, descriptors fully attacker-controlled with no secure-boot/authorization check on `/dev/uhid` | **Yes as a fuzzing surface.** Best next step, but needs a crash oracle (no dmesg/KASAN available) and risks device stability. |

Net: nothing known gives a clean shell→root path today. The two gating factors are
(1) hidraw is root-only, (2) the obvious 2024 HID info leak is already patched.

---

## 6. Reproduction cheat-sheet

```
# A
clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os -o tools/uhid/uhid_open tools/uhid/uhid_open.c
adb -s 192.168.1.119:5555 push tools/uhid/uhid_open /data/local/tmp/
adb -s 192.168.1.119:5555 shell '/data/local/tmp/uhid_open'

# B
clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os -o tools/uhid/uhid_harness tools/uhid/uhid_harness.c
adb -s 192.168.1.119:5555 push tools/uhid/uhid_harness /data/local/tmp/
adb -s 192.168.1.119:5555 shell '/data/local/tmp/uhid_harness'
# hold variant for live /sys/class/hidraw inspection:
clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os -DHOLD -o tools/uhid/uhid_hold tools/uhid/uhid_harness.c
adb -s 192.168.1.119:5555 push tools/uhid/uhid_hold /data/local/tmp/
adb -s 192.168.1.119:5555 shell 'nohup /data/local/tmp/uhid_hold >/data/local/tmp/uhid_hold.log 2>&1 &'
adb -s 192.168.1.119:5555 shell 'ls -l /sys/class/hidraw/ /dev/hidraw*'
# cleanup: kill the uhid_hold PID; hidraw disappears

# C
clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os -o tools/uhid/nodes_open tools/uhid/nodes_open.c
adb -s 192.168.1.119:5555 push tools/uhid/nodes_open /data/local/tmp/
adb -s 192.168.1.119:5555 shell '/data/local/tmp/nodes_open'
adb -s 192.168.1.119:5555 shell 'id; ls -l /dev; ls -l /dev/input /dev/dri /dev/binder* /dev/*nexus* /dev/*stb*'
adb -s 192.168.1.119:5555 exec-out 'cat /proc/config.gz' > tools/uhid/kernel_config.gz
```

Files created locally: `tools/uhid/{uhid_open.c,uhid_open,uhid_harness.c,
uhid_harness,uhid_harness_full,uhid_hold,dev_open.c,dev_open,nodes_open.c,
nodes_open,dev_enumerate.txt,kernel_config.gz,kernel_config.txt}` and this doc.
Device: only `/data/local/tmp/uhid_open`, `uhid_harness`, `uhid_harness_full`,
`uhid_hold`, `dev_open`, `nodes_open`, `uhid_hold.log` (all test artifacts;
virtual HID device destroyed; device online and unchanged otherwise).

---

## 7. Final verdict (restated)

* **Can shell create a virtual HID device? YES — proven.** `open("/dev/uhid",
  O_RDWR)` succeeds; `write()` of a 4376-byte `struct uhid_event` with
  `type=UHID_CREATE2 (11)` creates a real HID device; kernel emits `UHID_START
  (2)` and, with a complete mouse descriptor, `UHID_OPEN (4)`; a `/dev/hidraw0`
  node and `/sys/class/hidraw/hidraw0` appear; `UHID_INPUT2 (12)` is accepted.
* **Ioctl numbers**: legacy `UHID_CREATE2 = 0x51145511`, `UHID_INPUT2 =
  0x50025512` are correct but **return ENOTTY(25)** on 5.4 — the working API is
  `write`/`read`/`poll` of `struct uhid_event`, not ioctl.
* **Openable by shell**: `/dev/uhid`, `/dev/uinput`, `/dev/binder`,
  `/dev/nexusmem`, `/dev/nxmw`, `/dev/dri/card0`, `/dev/dri/renderD128` (anomaly),
  world-writable misc nodes; `/dev/input/event*`, `/dev/ion` **read-only**;
  `/dev/bcmpip`, `/dev/nxwk`, `/dev/hddidentify`, `/dev/ashmem` SELinux-denied.
  No `/dev/*stb*`. Created `/dev/hidraw0` is root-only (EACCES).
* **Which HID bug to target**: none of the known 2024–2026 HID CVEs is a viable
  shell→root path here (50302 patched; 38556 DoS with no UBSAN; 38494 not
  reachable from UHID userspace; 89624 needs an absent driver and an
  EACCES-protected hidraw). The only worthwhile follow-up is **descriptor-driven
  fuzzing of the ~70 built-in HID drivers via UHID_CREATE2** (notably the custom
  `hid-sei-rcon`), accepting that there is no dmesg/KASAN oracle and that a crash
  would require stopping. Absolute blocker to the classic hidraw UAF family:
  **hidraw nodes are 0600 root:root for shell.**
