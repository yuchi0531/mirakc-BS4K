# Phase 19, Round 26 — SLUB freelist adjacency / pipe readback attempt

Date: 2026-11-17.
Device: `adb -s 192.168.1.119:5555`, kernel `5.4.286-ab0600` (arm64 kernel,
32-bit ARM compat shell), shell uid 2000 in root memcg (`4:memory:/`).
No UART; panic ⇒ ~2 min watchdog reboot. Budget: max 3 reboots.

## SAFETY envelope (honored)

- Feature report length **L = 129 only**; validated at runtime in the harness.
  Max spill = 2 zero bytes at object indices 128, 129.
- No reboot / system / vendor / block writes, no flashing.
- Local writes only under `tools/kernel/` and this document; device writes
  only `/data/local/tmp` (pushed harness).
- Health (`adb devices`, `getprop sys.boot_completed`, `uptime`) checked after
  **every** spill.
- **Reboots used: 0 of 3. Device healthy throughout.**

## New recon established this round

| Property | Value | Source |
|---|---|---|
| `CONFIG_SLAB_FREELIST_RANDOM` | **not set** | `zcat /proc/config.gz` |
| `CONFIG_SLAB_FREELIST_HARDENED` | **not set** | same |
| `CONFIG_SLUB_CPU_PARTIAL` | **y** | same |
| kmalloc-128 cache | single, objsize 128, 32 obj/slab, no clones | `/proc/slabinfo` |
| CPUs online | 0-3 | `/sys/devices/system/cpu/online` |
| fd limit | 32768 | `ulimit -n` |
| `/dev/kmsg`, `logcat -b kernel` | unreadable by shell | — |

Confirmed against the actual **v5.4.286** sources (fetched from `gregkh/linux`):
- `hid_report_raw_event()` does contain the vulnerable
  `memset(cdata + csize, 0, rsize - csize)` (removed in the local v7.1 tree).
- `mt_get_feature()` in v5.4.286 has **no** `report->id != buf[0]` guard (the
  guard exists in v7.1), so the mismatched `data[0]=4` reply does reach the
  memset. Confirmed: `rsize = 129`, `csize = 1` ⇒ `memset(buf+2, 0, 128)`,
  last written index 129 ⇒ 2 bytes past the 128-byte object.
- `allocate_slab()` builds the freelist **ascending** (`object → object+128 → …`)
  when randomization is off, so a pristine slab is sequential.
- **`hid_add_device()` is invoked from a workqueue** (`schedule_work(&uhid->worker)`
  in `uhid_dev_create2()`), i.e. `mt_get_feature()`'s `kzalloc(9)` runs in
  `kworker` context, **not** the harness process context. This is the main
  reason a same-CPU SLUB adjacency between the HID buffer and our sprayed
  pipe `->bufs` arrays could not be assumed.

## Harness

`tools/kernel/pipe_spill_r26.c` → `pipe_spill_r26` (armv7a static ELF, raw
`svc 0`, no libc). Build:

    clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
          -fno-builtin -o pipe_spill_r26 pipe_spill_r26.c

Usage: `pipe_spill_r26 <L:129> <npipes> <mode:0|1|2|3> <seed> <cpupin:-1..3> <npre>`

Modes:
- 0: pipes first, then HID, immediate reply (R25 ordering).
- 1: HID + GET_REPORT first (reply deferred), spray pipes while HID buffer
  alive, then reply — **safe readback path**.
- 2: mode 1 + post-spill allocation burst.
- 3: mode 0 + post-spill allocation burst (poison-consumption probe).

Additions over R25: `sched_setaffinity`-capable (we actually used toybox
`taskset` because the raw syscall returned EFAULT), `/proc/slabinfo`
`kmalloc-128` snapshots at 5 checkpoints, pre-groom pipe count `npre`,
full-pipe (up to 2600) readback, and configurable allocation burst.

## Spill attempts (full log index under `tools/kernel/r26_logs/`)

| # | Log | Order / params | Outcome |
|---|---|---|---|
| 1 | `t01_n1_m1_cpu1.log` | m1, n=1, npre=0, taskset CPU1 | NO_PIPE_CHANGE, healthy |
| 2 | (slabinfo-debug run) | m1, n=1, npre=0 | NO_PIPE_CHANGE, healthy |
| 3 | `a1_run.log` | m1, n=200, npre=48, taskset CPU1 | NO_PIPE_CHANGE, healthy |
| 4 | `a2_npre600.log` | m1, n=200, npre=600, CPU1 | NO_PIPE_CHANGE, healthy |
| 5 | `a3_allpipes.log` | m1, n=200, npre=600, all 800 pipes read | NO_PIPE_CHANGE, healthy |
| 6 | `a4_npre1500.log` | m1, n=200, npre=1500 (forced 5 new slabs) | NO_PIPE_CHANGE, healthy |
| 7 | `a5_m0_s2.log` | m0, n=200, seed=2, CPU1 | NO_PIPE_CHANGE, healthy |
| 8 | `a6_m3_s1.log` | m3, n=200, seed=1, 60-object burst | NO_PIPE_CHANGE, healthy |
| 9 | `a7_cpu0/2/3.log` | m1, n=200, seed=7, pinned CPU0/2/3 | NO_PIPE_CHANGE, healthy |
| 10 | `a8_burst500.log` | m3, n=200, seed=1, 500-object burst | NO_PIPE_CHANGE, healthy |

Total: **13 spills executed, 0 pipe-array hits, 0 panics, 0 reboots.**

Representative slabinfo traces (active_objs / num_objs / num_slabs):

- `a4_npre1500` (mode 1): T0 `70395/71936/2248` → T1 (GET_REPORT, buf alive)
  `71901/71936/2248` → T2 (after 200 pipes)
  `72096/72096/2253` → T3 (after spill) `72070/72576/2268` → T4
  `71580/72576/2268`. The spray forced 5 new slabs; no corruption appeared.
- `a8_burst500` (mode 3): T3 `71044/71968/2249`; 10 burst steps moved active
  monotonically `71081 → 71536` with **no** anomalous slab free/allocate and
  no panic even after consuming 500 fresh kmalloc-128 objects.

## Outcomes

1. **Reliable pipe-array corruption with readback: NOT ACHIEVED.** Across 13
   spills (n = 1..200, npre = 0..1500, seeds 0,1,2,7, CPUs 0-3 pinned and
   unpinned, both orderings, up to 1700 pipes checked per run), every pipe's
   64-byte pattern round-tripped byte-for-byte (`CHANGED_PIPES=0`, `err=0`
   during the live readback). No kernel-ish bytes, no page-pointer shift.

2. **Controlled freelist-poison confirmation via slabinfo: NOT ACHIEVED.**
   Neither the 60- nor the 500-object post-spill burst produced a slabinfo
   anomaly or a panic; `num_slabs`/`num_objs` changed only by normal slab
   churn. The poisoned-`next` hypothesis remains unproven on this device in
   this round.

3. **Panic oracle: only INDIRECT and NON-REPEATABLE.** The only positive signal
   remains R25's `m0_n200_s1` (200 pipes-first, seed 1, no pinning) which went
   offline at `DESTROY` and auto-rebooted ~2 min later — consistent with a
   free-object freelist `next` corruption. Attempts to reproduce it this round
   (same count, seeds 1/2, both pinned and unpinned) all completed cleanly, so
   it is not a controllable primitive.

## Analysis / why adjacency failed

- The HID buffer is allocated inside the UHID **workqueue** (`kworker`), while
  the pipe spray runs in the harness task. Even with `taskset` pinning the
  harness to one CPU, the freelist neighborhood of the HID object is governed
  by the workqueue CPU's `kmem_cache_cpu`, and the HID probe itself allocates
  and frees several kmalloc-128 objects (`hid_register_field`, report hash,
  parser collections) before `mt_get_feature()` runs, scrambling the freelist.
  Consequently `X->next` is generally not `X+128`, so the OOB write lands on an
  arbitrary object in the slab, not on a pipe `->bufs` array we control.
- Because `CONFIG_SLAB_FREELIST_RANDOM` is off, a *pristine* slab would be
  sequential; forcing 5 new slabs (npre=1500) still yielded no hit, consistent
  with the workqueue-vs-task CPU split and probe-time frees being the dominant
  factor rather than slab freshness.

## Artifacts / caveats

- `tools/kernel/pipe_spill_r26.c`, `tools/kernel/pipe_spill_r26` — harness.
- `tools/kernel/r26_logs/` — all per-run logs (full stdout, unfiltered).
- No dmesg/KASAN/`CONFIG_SLUB_STATS` oracle, so the R25 panic cause is inferred
  from timing/reboot only.
- Conclusion: this round produced a **negative result for a controllable
  primitive**; the only observed effect remains the prior round's
  non-deterministic reboot. Device is healthy, 0/3 reboot budget consumed.
  Pushed binary will be removed from `/data/local/tmp` at task end.
