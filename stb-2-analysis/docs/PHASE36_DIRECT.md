# PHASE36: カーネル VA を mmap offset にする直接攻撃（最終）

- 日付: 2026-11-17
- 端末: `adb -s 192.168.1.119:5555` (C02AS5 / Android 14 / カーネル 5.4.286-ab0600 / `armv8l`、userspace は `armeabi-v7a` / shell uid=2000 `u:r:shell:s0`)
- 作業 dir: `/home/yuchi0531/projects/mirakc-BS4K/stb-2-analysis`
- 種別: **読み取り・ioctl / mmap probing のみ**。reboot / flash / system 書込 / 録画 / blu / recovery / bootloader / physical は一切なし。
- 書き込み先: 端末は `/data/local/tmp/` のみ、ローカルは `docs/phase36_raw/` とビルド一時 `/tmp/opencode/phase36/` のみ。
- 安全: 各 TASK 後に `sys.boot_completed` を確認、**全行程で 1 を維持**。tombstone 0 → 0、パニック・再起動なし。
- mmap 長は 4 KiB（一部 1 MiB）、巨大 mmap は実施せず。0x47 らしきバイトは **1 バイトも検出されず**。

## 結論（先に）

**核心仮説は否定。カーネル仮想アドレスを mmap offset に指定しても、いかなるデバイスでもカーネルメモリはユーザ空間にマップされない。**

1. TASK A: `/proc/brcm/transport` から **TSMF source のカーネル VA `0xffffff8001bea800`** と RAVE CDB 物理アドレス 3 個を抽出。`debug` は 452 カテゴリのレベル一覧のみで **アドレス 0 個**。
2. TASK B: 5 デバイス × 全 offset パターン × aarch64 の 64 bit mmap + prot/flags マトリクス = **全失敗**（`ENOMEM` / `EINVAL` / `EACCES`）。0x47 / 0x7f パターン 0 件。
3. TASK C: libnexus 逆アセンブルで **真の ioctl は `0x65a25` (`NEXUS_MemoryBlock_GetMmapOffset_private`)** と特定。これは shell から **SELinux で EACCES**（`avc: denied { ioctl } ioctlcmd=0x5a25 tcontext=gpu_device`）。`0x4004xxxx` 空間も 1013/1024 が EACCES。nexusmem で成功する ioctl は `0x2` のみで、これは入力に依存せず固定 `0x1000` を返すだけ。
4. TASK D: /dev/mem・/proc/kcore・debugfs・kprobe_events は存在せず、BPF も `bpf(MAP_CREATE) = EACCES`。`su` 無し。カーネルメモリを読む primitives は残っていない。

→ **PHASE24/34/35 の negative result が最終確定。残る現実的ルートは GPL ソース開示依頼のみ** (`docs/GPL_REQUEST_GUIDE.md`)。

---

## TASK A — transport / debug のアドレス全抽出

再生は PHASE24 手順で確認済み（起動前に `neohome2/...MainActivity` が top、`source TSMF` あり）。2 秒間隔の 2 スナップショットで PUSI error が `431724→431816` / `675248→675348`、stc value が `0xf7862e0a→0xf787a410` と増加し、**定常再生中**を確認。

### A-1. `/proc/brcm/transport` の全アドレスとラベル

| アドレス | 直前のラベル / 出所 | 種別 |
|---|---|---|
| `0xffffff8001bea800` | `parser band 16: source TSMF 0xffffff8001bea800, enabled n, pid channels 14` | **カーネル VA**（TSMF source 構造体） |
| `0x0bb514000` | `RAVE0: type=decode, pidch=173, ... CDB 0/262144(0%) at 0x0bb514000` | 物理（CDB 262144 B） |
| `0x0b1157200` | `RAVE2: type=decode, pidch=172, ... at 0x0b1157200` | 物理（CDB 6291456 B） |
| `0x0acd9c000` | `RAVE3: type=decode, pidch=172, ... CDB 7544/8192(92%) at 0x0acd9c000` | 物理（CDB 8192 B） |
| `0xc5ce3e1c` | `stcChannel ... offset 0xc5ce3e1c` | STC offset |
| `0xf7862e0a` | `stcChannel ... value 0xf7862e0a` | PCR 時刻（経時変化） |
| `0x00010300` | `MTSIF RX0: Band Detect 0x00010300` | レジスタ値 |
| `0x00000000xxxxxxx` | `pidchannel 000000002b40fd7c` 等 28 個 | 32 bit オブジェクト ID / ハンドル（VA ではない） |

補足: PHASE3 §12.3 では `source TSMF 0xffffff82f23d5200`、PHASE24 / 本 Phase では `0xffffff8001bea800`。起動ごとに変わる（KASLR / アロケーション差）。CDB アドレスも PHASE24 の `0x0acd9c000` に対し PHASE3 は `0x0acc90400` 等、やはり変動。

### A-2. `/proc/brcm/debug`

8767 バイト、452 行。全行が `<category>\twrn` 形式（例 `xpt_tsmf wrn`, `nexus_tsmf wrn`, `NEXUS_MEMORY_DUMP wrn`, `BMEM_ALLOCATED wrn`）。**16 進アドレスは 1 個も含まれない**。よって「debug にリアルタイム TSMF の VA が出ている」という想定はこの端末では成立しない（アドレス情報は transport 側のみ）。

生データ: `docs/phase36_raw/phase36_transport1.txt`, `phase36_transport2.txt`, `phase36_debug.txt`

---

## TASK B — 各デバイスで mmap offset にカーネル VA を指定

userspace が 32 bit の `mmap2` では pgoff が 32 bit に切詰られ、64 bit のカーネル VA を渡せない。そこで **aarch64 の静的バイナリを端末で実行できることを確認**し（`AARCH64_OK`）、64 bit `mmap` syscall (nr=222) の byte offset に VA を直接渡した。カーネル側の `vm_pgoff = offset >> 12` が VA>>12 になる。

 offset 候補（TSMF VA = `0xffffff8001bea800`）:

| # | offset (byte) | 意味 |
|---|---|---|
| 1 | `0xffffff8001bea800` | VA そのもの（非ページ整列 → syscall が EINVAL） |
| 2 | `0xffffff8001bea000` | `VA & ~0xFFF`（ページ整列。pgoff=`0xffffff8001bea`） |
| 3 | `0xffffff8001bea` | `VA >> 12` を byte offset として渡す |
| 4 | `0x1bea800` | `VA - PAGE_OFFSET(0xffffff8000000000)`（線形写像なら物理） |
| 5 | `0x1bea` | 同上をさらに >>12 |
| 6-8 | `0xbb514000` / `0xb1157200` / `0xacd9c000` | RAVE CDB 物理アドレス |

### B-1. 全デバイス × 全 offset の結果（`PROT_READ, MAP_SHARED, len=0x1000`）

| デバイス | VA そのもの | VA&~FFF | VA>>12 | VA-PAGE_OFFSET | PAGE_OFFSET>>12 | CDB×3 | 判定 |
|---|---|---|---|---|---|---|---|
| `/dev/nexusmem` | **-22** | **-12** | **-22** | **-22** | **-22** | -12/-22/-12 | 全失敗 |
| `/dev/nxmw` | -22 | -22 | -22 | -22 | -22 | -22×3 | 全失敗 |
| `/dev/dri/renderD128` | -22 | -22 | -22 | -22 | -22 | -22×3 | 全失敗 |
| `/dev/dri/card0` | -22 | -22 | -22 | -22 | -22 | -22×3 | 全失敗 |
| `/dev/dri/card1` | — | -22 | — | -22 | — | -22×3 | 全失敗 |
| `/dev/ion` | open **-13** | open -13 | open -13 | open -13 | open -13 | open -13 | open 不可 |
| `/dev/nxwk` | open -13 | open -13 | — | open -13 | — | open -13 | open 不可 |

（`-22`=EINVAL, `-12`=ENOMEM, `-13`=EACCES）

`/dev/nexusmem` でだけページ整列 VA が EINVAL ではなく ENOMEM になるが、これは「引数は通ったがドライバがマップできない」だけで、データは返らない。非ページ整列 VA が EINVAL なのは mmap syscall 側の整列チェック（3 回繰り返して再現性確認）。

### B-2. prot / flags マトリクス（nexusmem, off=`0xffffff8001bea000`）

`prot ∈ {READ, READ|WRITE}` × `flags ∈ {SHARED, PRIVATE, SHARED|FIXED, PRIVATE|FIXED}` × `len ∈ {0x1000, 0x100000}` = 16 通り。

| flags | 結果 |
|---|---|
| MAP_SHARED (0x1) | 8/8 ENOMEM (-12) |
| MAP_PRIVATE (0x2) | 8/8 ENOMEM (-12) |
| MAP_SHARED\|FIXED (0x11) | 8/8 **EPERM (-1)** |
| MAP_PRIVATE\|FIXED (0x12) | 8/8 **EPERM (-1)** |

→ カーネル VA をユーザ VA に FIXED マップする試みも拒否。

### B-3. 0x47 / 0x7f 検索

probe は各成功 mmap の先頭 4 KiB を走査し、188 バイト stride の 0x47 連鎖と 0x7f を数える設計。成功したのは `/dev/zero` のサニティチェックのみで、その内容は全 0。**0x47 / 0x7f は 0 件**。よって即座に 1 MiB 保存・報告する条件は発生せず。

生データ: `docs/phase36_raw/phase36_b_*.txt`（12 ファイル）

---

## TASK C — nexusmem ioctl で mmap offset を設定できるか

### C-1. 重要な新事実: 真の ioctl 番号は `0x65a25`

PHASE3 §19 の記述（`/proc/self/mem` の章）とは別に、`refs/device/libnexus.so` の `NEXUS_MemoryBlock_GetMmapOffset_private` (0x652e04) を逆アセンブルすると:

```
652e10: mov r2, r0          ; 引数0 (block handle) を struct.word0 へ
652e30: str r1, [sp,#8]     ; 引数1 (out ptr) を struct.word1 へ
652e34: movw r1, #0xa25
652e3c: movt r1, #0x65      ; cmd = 0x65a25
652e40: add r2, sp, #4      ; ioctl arg = 8B struct
652e44: bl ioctl            ; ioctl(fd, 0x65a25, &{u32 in, u32 out})
652e8c: ldr r4, [sp,#4]     ; 返値 = struct.word0 = mmap offset
```

隣接する `NEXUS_MemoryBlock_GetProperties` は `0x65a26`。つまり Broadcom の「mmap offset 取得」は **`0x4004xxxx` ではなく `0x65xxx` 空間**にある（ task 提示の `0x4004xxxx` は誤り）。

### C-2. 直接発行の結果 — SELinux に拒否

| 対象 | cmd | rc | 理由 |
|---|---|---|---|
| `/dev/nexusmem` | `0x65a25` | **-13** | SELinux |
| `/dev/nexusmem` | `0x65a26` | **-13** | SELinux |
| `/dev/nxmw` | `0x65a25`, `0x65a26` | **-13** | SELinux |

`logcat` の AVC:
```
avc: denied { ioctl } for path="/dev/nexusmem" dev="tmpfs" ino=12622
     ioctlcmd=0x5a25 scontext=u:r:shell:s0
     tcontext=u:object_r:gpu_device:s0 tclass=chr_file permissive=0
```
端末の `/dev/nexusmem` と `/dev/nxmw` は共に `u:object_r:gpu_device:s0`。shell にはこの ioctlcmd の許可が無い。また `0x65a00..0x65aff` の 256 通りを両デバイスで試したが **256/256 EACCES**。

### C-3. `0x4004xxxx` 空間の総当たり（task 指定）

`/dev/nexusmem` に対し `0x40040000..0x400400FF`（256 通り、size=0/type=4/nr=0..255）を argmode=1（64B zero buffer）で実施:

| rc | 件数 |
|---|---|
| EACCES (-13) | **248** |
| ENOTTY (-25) | 8 |
| 成功 | 0 |

さらに raw `0x0..0x3FF`（1024 通り）でも:

| rc | 件数 | 内容 |
|---|---|---|
| **0** | 1 | cmd=`0x2` のみ。返り値は常に `buf[0]=0x1000` |
| ENOTTY (-25) | 10 | ハンドラ無し |
| EACCES (-13) | 1013 | SELinux |

`cmd=0x2` に TSMF VA や `1` を入力しても返り値は常に `0x1000` で不変（ページサイズ照会と推定）。**offset を返す ioctl は見つからず**。

生データ: `docs/phase36_raw/phase36_c_*.txt`

---

## TASK D — 代替読み取り primitive の有無

| 経路 | 結果 |
|---|---|
| `/dev/mem`, `/dev/kmem`, `/proc/kcore`, `/dev/port` | **存在しない**。config も `# CONFIG_DEVMEM is not set`, `# CONFIG_PROC_KCORE is not set` |
| debugfs `/sys/kernel/debug` | 存在しない（tracefs のみ marounted） |
| `/sys/kernel/tracing/kprobe_events` | 存在せず、書込も Permission denied。`trace_pipe` は readtracefs で読めるだけ |
| `bpf()` syscall | `bpf(BPF_MAP_CREATE) = -13 (EACCES)` → 不可 |
| `su` / magisk | 不存在。`id` は uid=2000(shell) のまま |
| renderD128 vendor ioctl `0x41`-`0x4b`（PHASE33/34/35 で解析済み） | 返るのはサイズ・ハンドル・物理/GPU アドレスのみ。TS データ 0 バイト |
| nexusmem ioctl `0x2` | 固定 `0x1000`。任意アドレスの内容を返さない |
| `/proc/self/mem` | 自プロセスのユーザ VA のみ。カーネル VA は `access_ok` で拒否される想定で、返す ioctl も無し |

→ カーネルアドレスの内容をユーザ空間へ `copy_to_user` する primitive は **存在しない**。

---

## 最終判定と健全性

| TASK | 判定 |
|---|---|
| A — アドレス抽出 | TSMF VA と CDB 物理を抽出。**debug にアドレス無し** |
| B — カーネル VA を mmap offset に | **失敗**（全デバイス・全パターン ENOMEM/EINVAL/EACCES） |
| C — nexusmem ioctl で offset 取得 | **失敗**（真の ioctl `0x65a25` は SELinux EACCES、`0x4004xxxx` も 1013/1024 EACCES） |
| D — 代替 kernel-read primitive | **無し** |
| リアルタイム TS 取得 | **不可**。0x47 (188 stride) / 0x7f とも 0 件 |

### 健全性 / 証拠

- 最終 `sys.boot_completed=1`。全行程で 1 を維持。tombstone 0 → 0、reboot / panic / tombstone 増加なし。
- 端末側変更は `/data/local/tmp/` に push した一時バイナリ (`mmap64`, `ioctl64`, `sweep64`, `nxmem2`, `mmapmatrix`, `bpfprobe`, `a64test`) と probing 出力のみ。
- 一時ビルド: `/tmp/opencode/phase36/*.c`（`clang --target=aarch64-linux-gnu -nostdlib -static -fuse-ld=lld -ffreestanding -O2`）。
- 生データ: `docs/phase36_raw/`（transport/debug 3 本 + TASK B 8 本 + TASK C 4 本）。
- 補足: 本 Phase で aarch64 static binary が端末で実行可能なことを確認（今後の probing で 64 bit syscall が使える点は有用な副次知見）。

### 残る現実的ルート

1. **GPL ソース開示依頼**（`docs/GPL_REQUEST_GUIDE.md`, `docs/gpl_request_draft_stb2.md`）。brcmv3d / nexusmem の源码で `vm_pgoff` 解釈と SELinux ioctlcmd ホワイトリストを確定する。
2. STB-2 に固執せず、既に e2e pass 済みの external decoded TLV server 構成（192.168.1.133:40772 + mirakc-arib-tlv）を本番化する（`ts-delivery-route-map-2026-11-17` 参照）。

**最終確認: `boot_completed=1`**
