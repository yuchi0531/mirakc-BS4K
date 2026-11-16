# Phase 24: 放送中のリアルタイム TS/TLV バイト列取得の試行

実施: 2026-11-17 08:14-08:19 JST / 対象: C02AS5 (adb -s 192.168.1.119:5555)

方針: **録画ファイルは触らない**。reboot / fastboot / recovery / 物理操作は禁止。
未知 ioctl は連発せず 1 つずつ健康確認。最終的に端末を健全・再生可能状態で維持。

---

## 1. 再生状態への移行と確認

起動前は screensaver (`com.google.android.backdrop/.DreamActivity`) で、
`/proc/brcm/transport` は 11 行のみ（playpump / pidchannel / source の各線なし）。

NeoFront2 の LEANBACK ランチャーを特定して起動:

```sh
cmd package resolve-activity --brief -c android.intent.category.LEANBACK_LAUNCHER \
  jp.co.sei.streamcruiser.app.neohome2
# -> jp.co.sei.streamcruiser.app.neohome2/jp.co.sei.streamcruiser.app.ui.entrance.EntranceTopActivity

am start -n jp.co.sei.streamcruiser.app.neohome2/jp.co.sei.streamcruiser.app.ui.entrance.EntranceTopActivity
```

8 秒後、再生中を確認:

```
topResumedActivity=...neohome2/jp.co.sei.streamcruiser.app.ui.main.MainActivity
parser band 16: source TSMF 0xffffff8001bea800, enabled n, pid channels 14, ...
pidchannel ch160..ch173: pid 0x0,0x1,0x12,0x14,0x10,0x24,0x11,0x23,0x28,
                         0x1201,0x1202,0x1200,0x1440,0x1444
RAVE0: type=decode, pidch=173, CDB 0/262144(0%)      at 0x0bb514000
RAVE2: type=decode, pidch=172, CDB 0/6291456(0%)     at 0x0b1157200
RAVE3: type=decode, pidch=172, CDB 7544/8192(92%)    at 0x0b1151000
```

`/proc/brcm/debug` は 452 カテゴリ。`xpt_tsmf`, `nexus_tsmf`, `xpt_playback`,
`nexus_xpt_dma`, `bxpt_dma`, `NEXUS_MEMORY_DUMP`, `BMEM_ALLOCATED` 等すべて `wrn`。

**重要**: 再生中は **playpump の行が transport に出ない**。
停止時（PHASE3 §8.3）に見えた `playpump[1/32] 1: stopped, fifo 0/131072(0%) at 0x0b117b000`
は消え、BS4K の TLV は **TSMF → RAVE ダイレクト**で流れる（playpump DMA FIFO を経由しない）。
つまり「playpump FIFO 131072B を読めば TS が取れる」という仮説は本機の BS4K 再生では成立しない。

---

## 2. `/proc/brcm/` 全 37 エントリの連続スナップショット

2 通りで取得:

| 取得 | 方法 | 実測間隔 |
|---|---|---|
| 1 回目 | `timeout 0.4 cat` × 37 + `sleep 0.5` | 約 3.1 秒（timeout プロセス生成が支配的） |
| 2 回目 | 自作 C ヘルパ `tools/phase24_tight.c`（fork + 300ms alarm + 500ms アライン） | **490-537ms** |

データは `/data/local/tmp/phase24` と `phase24b` に保存しホストへ pull して解析。

### 2.1 経時変化したエントリ（0.5 秒間隔・10 回）

| エントリ | 異なるハッシュ数 | 変化の内容 |
|---|---|---|
| `transport` | 10 | cc/PUSI エラーの増加、CDB overflow 畳み、`stcChannel` の value（PCR 時刻） |
| `display` | 10 | vsync / EOP カウンタ、line counter |
| `hdmi_output` | 10 | Video Line Counter |
| `graphics2d` | 4 | blit/checkpoint カウンタ |
| `kni` | 10 | `nexus_base.c:1121` の 1640B アロケーションが増加 |
| `mma` | 10 | RAVE CDB の参照カウント (`nexus_rave.c:352` の第 5 フィールドが 6→14 等) |
| `surface_compositor` | 3 | composition 回数、framebuffer の Available/Displaying/Submitted 状態 |
| `gpio` | 2 | `gpio84` の value が 1→0（LED 等、TS と無関係） |
| `debug` | 2（1 回目のみ） | カテゴリが遅れて登録される（`vce_proc`, `NEXUS_MEMORY_DUMP`, `BMEM_*` 等）。2 回目は起動直後から安定 |

変化しないエントリ: `frontend`（定常再生中はブロッキングイベントストリームで 0B）、
`stream_mux`（**常に 0 バイト**）、`tee`, `status`, `platform`, `core`, `astm`, `audio*`,
`bhdm_*`, `cec`, `dtu`, `sage`, `sync_channel`, `video_decoder`, `video_encoder` 他。

### 2.2 バイナリ / TS ヘッダ (0x47) の検査

全 390 + 380 ファイルをバイト単位で走査:

- 非印字文字を含むファイルは `s7-kni` の **1 バイトのみ**（文字列終端 NUL 0x00）。
- 0x47 が 188 バイト stride で並ぶファイルは `tee` に見えたが、
  これは text 中の ASCII `'G'` (`RAVE FW[00] clear\nRAAGA...`) の一致で偽陽性。
- **連続 0x47 も、TS ヘッダらしい 0x47 パターンも一切なし。**
- `stream_mux` は 10 サンプルすべて 0 バイト。

→ **`/proc/brcm/` 配下には TS/TLV バイト列は出ていない。すべてテキストの状態/カウンタ。**

### 2.3 mma から見えた DMA バッファ（参考）

```
0x0b1151000,    8192  nexus_rave.c:352      <- RAVE3 CDB (92% 使用中)
0x0bb514000,  262144  nexus_rave.c:352      <- RAVE0 CDB
0x0b1157200, 6291456  nexus_rave.c:352      <- RAVE2 CDB
0x0acc9d000,  131072  nexus_rave.c:355
0x0b112e000,   65536  nexus_message_pid2buf.c:333
0x0b1155000,    4280  bgrc_packet.c:1009
```

これらは **復号後の ES / 内部メッセージ用**であり、TS パケットそのものではない。
アドレスは `/proc/brcm/mma` に出るが、ユーザー空間からはマップできない（後述）。

---

## 3. `/proc/brcm/config` からの TS ダンプ起動の試行

`cat /proc/brcm/config` は常に 0 バイト（write-only コマンド/モジュール設定チャネル）。
書き込みは **config のみ**に行い、rc と `/proc/brcm/debug` 差分・logcat・健康状態を記録。

| # | 書き込み文字列 | rc | debug 差分 | 変化 |
|---|---|---|---|---|
| 1 | `help` | 0 | 0 | なし |
| 2 | `debug` | 0 | 0 | なし |
| 3 | `debug xpt_tsmf dbg` | 0 | 0 | なし |
| 4 | `debug nexus_tsmf dbg` | 0 | 0 | なし |
| 5 | `debug xpt_playback dbg` | 0 | 0 | なし |
| 6 | `debug NEXUS_MEMORY_DUMP dbg` | 0 | 0 | なし |
| 7 | `nexus msg_modules='xpt_tsmf'` | 0 | 0 | なし（nxserver の定期 mem-use ログのみ） |
| 8-11 | 3-6 の `dbg` を `wrn` に戻す | 0 | 0 | なし |

全コマンドで rc=0、書き込み後も `wc -c` = 0（消費型）。
`/proc/brcm/debug` のレベルは一切変わらず、TS ダンプも logcat に出ず、
再生は継続（`source TSMF=1`, `29 KERNEL MODE ACTIVE`）。

searxng で `Broadcom Nexus /proc/brcm/config msg_modules`,
`BDBG_SetModuleLevel Nexus debug config runtime` 等を検索したが、
refsw のこのインターフェースに関する公開ドキュメントは見つからず
（PHASE3 §31.5 の「公開 refsw ヘッダは存在しない」という結論と一致）。

libnexus.so の文字列から config は `%s%s='%s'` 形式の**モジュール起動時設定チャネル**
（`msg_modules`, `nxproxyif`, `BP3BIN_PATH`, `secure_log` 等）と推定され、
起動後に書いてもデバッグレベルやダンプは変わらない。

---

## 4. playpump DMA FIFO / メモリへのユーザー空間アクセス

### 4.1 デバイスノード

```
crw-rw-rw-  system system  10,117  /dev/nexusmem
crw-rw-rw-  system system  10,116  /dev/nxmw
crw-rw-rw-  system system  10,118  /dev/nxwk
/dev/nexus  -> 存在しない (ENOENT)
```

`/sys/class/misc/nexusmem/` は `dev`, `uevent`, `device`, `power/` のみ。
FIFO サイズや物理アドレスをユーザーに露出する属性は無い
（`dev` と `uevent` は shell から読めず `-?????????`）。

### 4.2 再生中の mmap 再試行（既存ツールを再利用）

```
/data/local/tmp/mmap_probe:
  /dev/nexusmem off=0       len=65536  mmap=-12 (ENOMEM)
  /dev/nxmw      off=0      len=65536  mmap=-22 (EINVAL)
  /dev/ion                             open=-13 (EACCES)

/data/local/tmp/mmap2 (RAVE CDB / 旧 playpump FIFO アドレス):
  off=0x0b117b000  mmap=-12 (ENOMEM)
  off=0x0a9a00000  mmap=-12 (ENOMEM)
  off=0x0bb514000  mmap=-12 (ENOMEM)   <- 今回の RAVE0 CDB
  off=0x0          mmap=-12 (ENOMEM)
```

**再生中でも stopped 時と全く同じ失敗**。再生状態は mmap の可否に影響しない。

---

## 5. `/dev/nexusmem` / `/dev/nxmw` からの生バイト取得

すべて `timeout 2` 付きで実施（ブロック防止）:

| 操作 | 結果 |
|---|---|
| `cat /dev/nexusmem` | rc=1, 0B, `Invalid argument` (EINVAL) |
| `dd if=/dev/nexusmem bs=4096 count=1` | rc=1, 0B, EINVAL |
| `dd if=/dev/nexusmem bs=131072 count=1`（旧 playpump FIFO サイズ） | rc=1, 0B, EINVAL |
| `dd if=/dev/nexusmem bs=4096 skip=725329 count=1`（RAVE3 CDB アドレスへ seek） | rc=1, 0B, EINVAL |
| `dd if=/dev/nxmw bs=4096 count=1` | rc=1, 0B, `Function not implemented` (ENOSYS) |

**0x47 バイト列は 1 バイトも取得できず。** 生バイト経路は存在しない。

---

## 6. 結論

| 経路 | 結果 |
|---|---|
| `/proc/brcm/` 全エントリの連続スナップショット | 変化はカウンタ/状態のみ。**TS バイト・0x47 は皆無** |
| `stream_mux` | 再生中も常に 0 バイト |
| `/proc/brcm/config` からのダンプ起動 | 全コマンド rc=0 だが効果なし（起動時設定チャネルと推定） |
| playpump DMA FIFO | **再生中は playpump 自体が使われない**（TSMF→RAVE ダイレクト） |
| `nexusmem` mmap / read / seek+read | ENOMEM / EINVAL（再生中も同じ） |
| `nxmw` mmap / read | EINVAL / ENOSYS |
| 生バイト (0x47) | **取得できず** |

**BS4K の TLV は再生中も Nexus 内部（TSMF → RAVE）に留まり、
shell から到達できる `/proc/brcm/*` や `/dev/nexusmem` には一切漏れ出ない。**

これは PHASE3 ラウンド 10-12 の結論（Nexus IPC は `/dev/nxmw` の
LISTEN/ACCEPT = EACCES で閉塞、transport ioctl はサーバチャネル限定）と整合する。
今回の「再生中なら状況が変わるか」という仮説は **否定**された。

### 次の一手

1. 既に確定済みの通り、ソフトウェア経路は尽きた。
   `NXCLIENT_PASSWORD` 等の認証情報を nxserver (system uid) から得る手段が無い限り、
   `NEXUS_Recpump` / `NEXUS_StreamMux` は開けない。
2. 残るのは root 取得または物理アクセス（eMMC ISP / UART）のみ。
3. もしソフトウェアで続けるなら、`nexus_message_pid2buf.c` 系のバッファ
   (`0x0b112e000` 等) が PID 単位の生パケットを含む可能性だけは残るが、
   これも mmap が通らない以上、カーネルドライバ側の ioctl 構造体特定が必要。

### 端末の最終状態

```
29 KERNEL MODE ACTIVE
topResumedActivity=...neohome2/...MainActivity
parser band 16: source TSMF 0xffffff8001bea800
RAVE3: CDB 7544/8192(92%)
```

**健全・再生可能状態を維持。** reboot / 録画ファイル操作は行っていない。
