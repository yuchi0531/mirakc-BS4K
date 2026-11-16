# Phase 14, Round 22 — kmalloc-128 corrected math, grooming recon, and 2/3/4-byte UHID spill

Date: 2026-11-17
Device: `adb -s 192.168.1.119:5555` (adb online throughout), kernel
`5.4.286-ab0600`, `uname -m = armv8l` (arm64 kernel, 32-bit ARM userspace),
shell `uid=2000(shell)` in groups incl. `3011(uhid)`. SELinux enforcing, no
root/su, no dmesg/KASAN oracle.

## SAFETY envelope (observed at every step)

- Hard cap: feature-report length **L ∈ {129, 130, 131}** only; nothing larger,
  never 4095. Maximum spill 4 zero bytes.
- No reboot, no panic, no system/vendor/block writes, no flashing, no factory reset.
- Device writes restricted to `/data/local/tmp` (all temp binaries/files removed
  at the end); local writes restricted to `tools/kernel/` and this document.
- `adb devices`, `getprop sys.boot_completed`, `uptime` checked after **every** run.
- UHID devices always destroyed cleanly before the harness exits.

---

## 1. Corrected spill math — the kmalloc-128 finding

Round 21 (`PHASE13_ROUND21.md`) assumed `kzalloc(9) → kmalloc-16` and concluded
that N4=12..19 produced "4 bytes past the object". **That assumption is wrong on
this target.** Live `/proc/slabinfo` is readable and shows the smallest kmalloc
bucket is **128**, not 16 — consistent with arm64 `ARCH_DMA_MINALIGN=128`
(`KMALLOC_MIN_SIZE=128`, so kmalloc-16/32/64 caches do not exist).

Live evidence (`adb shell 'grep -i kmalloc /proc/slabinfo'`):

```
kmalloc-rcl-512       ...  512 ...
kmalloc-rcl-128     2207 3328  128 32 1 ...
kmalloc-8k           ... 8192 ...
kmalloc-4k           ... 4096 ...
kmalloc-2k           ... 2048 ...
kmalloc-1k           ... 1024 ...
kmalloc-512          ...  512 ...
kmalloc-256          ...  256 ...
kmalloc-128        76811 78336 128 32 1 ...
```

No `kmalloc-16`, `kmalloc-32`, or `kmalloc-64` line exists. `objsize=128` with
no SLUB redzone (`CONFIG_SLUB_DEBUG_ON` is not set), so the next object begins
at exactly +128 bytes.

Corrected derivation (source verified against v5.4.286 in R21):

- `mt_get_feature(report3)`: `buf = hid_alloc_report_buf()` →
  `kzalloc(hid_report_len(r3)+7) = kzalloc(9)` → **kmalloc-128** object (128 usable bytes).
- `hid_report_raw_event(hdev, HID_FEATURE_REPORT, buf, size=2, 0)`: numbered →
  `cdata=buf+1`, `csize=1`; reply `data[0]=4` selects feature report 4 (L data bytes);
  `rsize=hid_compute_report_size(report4)=L`; since `csize < rsize`:
  `memset(cdata+csize, 0, rsize-csize)` = `memset(buf+2, 0, L-1)`, last written byte
  index (0-based) = `2+(L-1)-1 = L`.

| Quantity | Value |
|---|---|
| Object size | 128 bytes (indices 0..127) |
| memset range | indices 2 .. L |
| First OOB index | 128 |
| **OOB bytes** | **L − 127** |

| L | OOB bytes | OOB indices |
|---|---|---|
| 129 | 2 | 128, 129 |
| 130 | 3 | 128, 129, 130 |
| 131 | 4 | 128, 129, 130, 131 |

Consequence: R21's N4=12..19 runs had last-written indices ≤ 19, i.e. **entirely
inside the 128-byte object — zero cross-object spill**. This round is the first
to drive a genuine cross-object write, using the corrected L values.

---

## 2. Task 0 — heap-grooming syscall / capability reconnaissance

Harnesses: `tools/kernel/groom_probe.c` and `tools/kernel/key_probe.c` → armv7a
static ELF, raw `svc 0`, no libc. Syscall numbers from local
`refs/stblinux/arch/arm/tools/syscall.tbl`: `setxattr=226 getxattr=229 mq_open=274
socketpair=288 sendmsg=296 recvmsg=297 add_key=309 keyctl=311 openat=322`.

Relevant `/proc/config.gz`: `CONFIG_KEYS=y`, `CONFIG_KEYS_COMPAT=y`,
`CONFIG_POSIX_MQUEUE=y`, `CONFIG_UNIX=y`, `CONFIG_UNIX_SCM=y`,
`CONFIG_F2FS_FS_XATTR=y`, `CONFIG_TMPFS_XATTR=y`, `# CONFIG_SYSVIPC is not set`.
`/data/local/tmp` is on f2fs (`user_xattr,inline_xattr,acl,no_heap`).

### 2.1 Raw results

```
=== Phase14 Round22 Task0: grooming primitives ===
pid=22447
[KEYS] session keyring id = 690082381
  add_key(user, desc=120, payload=128) = -13
  add_key(user, desc=120, payload=256) = -13
[XATTR] path=/data/local/tmp/r22_xattr
  open = 3
  setxattr(size=128) = 0
    getxattr = 128 mismatched=0
  setxattr(size=256) = 0
    getxattr = 256 mismatched=0
[MQ] mq_open(O_CREAT|O_RDWR) = -13
  -> POSIX mqueue unavailable to shell
[UNIX] socketpair(AF_UNIX,SOCK_DGRAM) = 0 fds=(3,4)
  sendmsg(size=128) = 128
  recvmsg = 128 mismatched=0
=== done ===
```

`mismatched=0` means the full value round-tripped byte-for-byte (readback works).

Keyring follow-up (`key_probe`):

```
=== key_probe: can shell get a writable keyring? ===
JOIN_SESSION_KEYRING('r22fresh') = 986248872
  GET_KEYRING_ID(PROCESS)=-126  SESSION=-13  USER=-13
  USER_SESSION=-126  THREAD=-126
  add_key(PROCESS)=-13  add_key(SESSION)=-13  add_key(USER)=-126
  add_key(USER_SESSION)=-126  add_key(THREAD)=-13
TRY add_key(keyring, 'r22own') = -13
=== done ===
```

AVC from logcat explaining the key failure:

```
avc: denied { write } for comm="groom_probe" scontext=u:r:shell:s0
     tcontext=u:r:init:s0 tclass=key permissive=0
```

### 2.2 Task 0 verdict — which primitive sprays a controlled kmalloc-128 object AND reads it back?

| Primitive | Available? | Readback? | Controlled **kmalloc-128** object with canary at offset 0? |
|---|---|---|---|
| `add_key`/`keyctl` (CONFIG_KEYS=y) | **No** — `add_key` = EACCES; SELinux `{write}` on `tclass=key` (`shell`→`init`); all special rings denied; even a freshly joined session keyring and a self-created keyring fail | n/a | No |
| `setxattr`/`getxattr` on `/data/local/tmp` (f2fs) | **Yes** (128 & 256 B, exact readback) | **Yes** | **No.** f2fs stores xattrs in the dedicated `f2fs_xattr_entry` slab (slabinfo objsize 208) / on-disk xattr blocks, not kmalloc. The transient `setxattr_copy()` `kmalloc(len)` buffer (would be kmalloc-128 for len≈120) is freed before the syscall returns — not a persistent spray object. |
| POSIX `mq_open` (CONFIG_POSIX_MQUEUE=y) | **No** — `mq_open` = EACCES (SELinux), confirms R21 | n/a | No |
| `socketpair(AF_UNIX,SOCK_DGRAM)` + `sendmsg`/`recvmsg` | **Yes** (128 B, exact readback) | **Yes** | **No.** skb linear buffer = `SKB_DATA_ALIGN(len) + SKB_DATA_ALIGN(sizeof(struct skb_shared_info))`; `skb_shared_info` (17 frags) dominates → kmalloc-512. Slabinfo confirmed kmalloc-512 +66 during spray while kmalloc-256 stayed flat. Message data begins at `skb->data` (offset 0 of that kmalloc-512 buffer), but it is the wrong size class to neighbor the UHID kmalloc-128 object. |
| `/proc/slabinfo` | **Yes**, readable | (oracle for bucket sizes) | n/a — used to prove min bucket = 128 |

Other tmpfs mounts (`/mnt`, `/storage`, `/dev`) exist but are not shell-writable,
and are out of scope anyway (device writes restricted to `/data/local/tmp`).

**Bottom line:** two readback primitives exist (`setxattr`/`getxattr`, AF_UNIX
datagrams), but **neither produces a controlled kmalloc-128 allocation**, so no
canary can be placed adjacent to the UHID `kzalloc(9)` object. `add_key` (the
classic kmalloc-128 spray+readback via key descriptions) and mqueue are
SELinux-denied. Therefore a kmalloc-128 corruption oracle is **not available**
from uid 2000 under the safety envelope. Per the task's Task 2 branch, the
L=129/130/131 runs were still executed, with the available readback primitives
sprayed as a best-effort canary.

---

## 3. Combined harness and L=129 / 130 / 131 runs

Harness: `tools/kernel/uhid_groom.c` → `tools/kernel/uhid_groom` (armv7a static
ELF, raw syscalls). It:
1. Sprays `NSRAY=200` setxattr values of `VSZ=120` bytes (unique tag at byte 0,
   pattern elsewhere) on `/data/local/tmp/r22_groom_xattr`, and `NSRAY=200`
   AF_UNIX datagrams of the same values over a socketpair.
2. Builds the R19 descriptor at runtime with the large FEATURE report **ID 4**
   length set to `L` (`Report Count 0x95,L`). Report 3 remains the 1-byte
   `HID_DG_CONTACTMAX` probe trigger.
3. Creates the UHID device, replies to `UHID_GET_REPORT(rnum=3)` with
   `.size=2, .data[0]=4, .data[1]=0` (mismatched → selects report 4 → spill),
   pumps to `UHID_OPEN`, sends `UHID_DESTROY`, drains CLOSE/STOP.
4. Reads back every xattr and every datagram and reports indices whose bytes
   changed (`bad` counters + first few offending indices).

`L` is validated at runtime to be exactly 129/130/131; anything else exits.

### Run log (all three runs)

| L | OOB | setxattr ok | unix ok | GET_REPORT | SPILL reply | OPEN | CLOSE/STOP | readback bad | read_errors | Health |
|---|---|---|---|---|---|---|---|---|---|---|
| 129 | 2 B | 31/200 | 200/200 | yes (rnum=3) | size=2 data[0]=4 | yes | yes | xattr 0, unix 0 | 0 | OK |
| 130 | 3 B | 31/200 | 200/200 | yes | yes | yes | yes | xattr 0, unix 0 | 0 | OK |
| 131 | 4 B | 31/200 | 200/200 | yes | yes | yes | yes | xattr 0, unix 0 | 0 | OK |

`setxattr ok=31/200`: f2fs's per-inode inline-xattr area fills after ~31 values
of 120 bytes; 200 setxattr calls were attempted but only 31 persisted. `unix
ok=200/200`: all 200 datagrams queued. This does not affect the soundness of the
readback check (all persisted objects were verified).

Representative output (L=131, the maximum safe spill):

```
=== Phase14 Round22: spray + UHID 2-4B spill ===
pid=22556
[cfg] L=131 OOB=4 bytes
[spray] setxattr:
  spray setxattr ok=31/200
[spray] unix dgram:
  spray unix sendmsg ok=200/200
[cfg] rd_size=64
open(/dev/uhid) fd=5
write(CREATE2) ret=4376
  EVENT type=2 (START) flags=0x rawlen=4376
  EVENT type=9 (GET_REPORT) id=1 rnum=3 rtype=0 rawlen=4376
  >>> SPILL REPLY id=1 rnum=3 size=2 data[0]=4
  reply write ret=4376
  EVENT type=4 (OPEN) rawlen=4376
write(DESTROY) ret=4376
  EVENT type=5 (CLOSE) rawlen=4376
  EVENT type=3 (STOP) rawlen=4376
[readback]
  [verify] xattr got=31 bad=0 unix got=200 bad=0
[result] saw_getreport=1 saw_open=1 read_errors=0
=== done ===
```

### Slabinfo deltas (before → after sprays+runs)

```
kmalloc-512   2558 → 2624   (+66, unix skb linear buffers, kmalloc-512)
kmalloc-256   5256 → 5256   (flat)
kmalloc-128  77173 → 77259  (+86, mixed device noise; not a controlled spray)
```

This is direct empirical confirmation that the AF_UNIX spray lands in
kmalloc-512, not kmalloc-128, and that no kmalloc-128 canary primitive was
achieved.

### logcat / AVC anomalies

No `panic`, `oops`, `BUG`, `kmalloc` corruption, `general protection`, or
`unable to handle kernel` text appeared. The only AVC records attributable to
this round were the expected SELinux denials (`tclass=key` for the add_key
attempts, `sysfs_uhid` search, `proc` panic knobs from R19) — nothing indicating
memory corruption.

### Health after every run

`adb devices` → `192.168.1.119:5555 device`; `sys.boot_completed` → `1`;
`uptime` monotonic `04:24 → 04:32` (no reboot); `read_errors=0` every run;
probe always reached `UHID_OPEN` and destroyed cleanly; no `1234:5678` device
left in `hid-multitouch`; all `/data/local/tmp` files (`groom_probe`,
`key_probe`, `uhid_groom`, `r22_xattr`, `r22_groom_xattr`) removed.

---

## 4. Verdict

| Question | Answer |
|---|---|
| Was a 2–4 byte cross-object zero write achieved? | **Mathematically yes.** With the corrected kmalloc-128 math, report lengths L=129/130/131 make the vulnerable `memset(buf+2,0,L-1)` write indices 2..L of a 128-byte object, i.e. 2/3/4 zero bytes at indices 128..L — past the object boundary into the next slab object. The `memset` was reached and executed with these exact parameters in all three runs (mismatched report 4 selected via `data[0]=4`). |
| Any observable corruption? | **None observed.** No kmalloc-128 spray+readback (canary) primitive is reachable from uid 2000 under the safety envelope: `add_key`/mqueue are SELinux-denied; f2fs xattrs use a dedicated slab/on-disk storage; AF_UNIX skb buffers are kmalloc-512. The sprayed, read-back xattrs (31) and datagrams (200) showed `bad=0` in every run, and logcat showed no crash/corruption signatures. This is a limitation of the available oracle, **not** evidence that the overflow is harmless. |
| Device health? | **Fully healthy.** Online, `boot_completed=1`, monotonic uptime across all runs, clean UHID lifecycle, no lingering devices, no read errors, temp files removed. |
| Key correction vs. prior rounds | R21's kmalloc-16 assumption was wrong for this arm64 target; live slabinfo proves the minimum kmalloc bucket is 128. R21's N4=12..19 runs therefore produced **zero** cross-object spill; this round is the first to drive a real 2/3/4-byte cross-object write. |

### Artifacts

- `tools/kernel/groom_probe.c`, `tools/kernel/groom_probe` — Task 0 primitive recon.
- `tools/kernel/key_probe.c`, `tools/kernel/key_probe` — keyring capability follow-up.
- `tools/kernel/uhid_groom.c`, `tools/kernel/uhid_groom` — combined spray + spill harness.
- `docs/PHASE14_ROUND22.md` — this document.
- All device-side binaries/files in `/data/local/tmp` were deleted; device left pristine.
