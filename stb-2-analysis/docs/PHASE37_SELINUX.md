# PHASE37 — SELinux `gpu_device` allowxperm 完全解析と ioctl 代行の可否

- 日時: 2026-11-17
- 端末: STB-2 (KDDI C02AS5) `192.168.1.119:5555`, Android 14 `UTT1.250730.001`, kernel `5.4.286-ab0600`, Build user/release-keys, `ro.debuggable=0`
- 権限: `uid=2000(shell)`, `context=u:r:shell:s0`, `getenforce=Enforcing`
- 制約遵守: reboot / flash / system 書込 / 録画 / blu / recovery / bootloader / physical は一切実施せず。書込は `/data/local/tmp` と `docs/` のみ。

## 結論（先に要約）

**抜け道なし。SELinux `gpu_device` の allowxperm は shell から越えられない。**

決定的根拠は 3 点:

1. 実機の **ロード済みポリシ** (`/vendor/etc/selinux/precompiled_sepolicy`, Policy v30) を `sesearch` で全走査したが、**`ioctl 0x5a25` を含む allowxperm ルールは 1 件も存在しない**（全 2478 allowxperm ルールを `-x 0x5a25` で検索 → 0 件）。つまり `shell` だけでなく **どのドメインにも 0x5a25 を発行する権限が無い**。
2. `0x65a25` (AVC 上の `ioctlcmd=0x5a25`) を実装しているのは `/dev/nexusmem` と `/dev/nxmw` の **2 ノードのみ**。両者とも `u:object_r:gpu_device:s0` で、全 `/dev` 走査でも `gpu_device` ラベルの他ノードは存在しない（`/dev/dri/*` は `gmem_device` で別ドライバ）。
3. 実測で許可 cmd 106 種を全て叩いたが、カーネルアドレスや mmap offset を返すものは **0 件**（全て ENOTTY / EFAULT / EINVAL / ENOSPC）。`0x5a00–0x5aff` は 256/256 EACCES（PHASE36 と一致）。

したがって「許可された cmd のふりをして get_mmap_offset を呼ぶ」「別ラベル側から同じドライバに到達する」「binder の別ドメインに代行させる」の 3 経路はいずれも閉じている。

---

## TASK A — デバイスラベルと allowxperm の完全マッピング

### A-1. デバイスノードのラベル（`ls -lZ` 実測）

| ノード | major,minor | SELinux ラベル | パーミッション | ioctl 0x5a25 |
|---|---|---|---|---|
| `/dev/nexusmem` | 10,117 | `u:object_r:gpu_device:s0` | `crw-rw-rw- system system` | **EACCES** |
| `/dev/nxmw` | 10,116 | `u:object_r:gpu_device:s0` | `crw-rw-rw- system system` | **EACCES** |
| `/dev/dri/card0` | 226,0 | `u:object_r:gmem_device:s0` | `crw-rw-rw- system system` | ENOTTY (別ドライバ) |
| `/dev/dri/card1` | 226,1 | `u:object_r:gmem_device:s0` | `crw-rw-rw- system system` | ENOTTY |
| `/dev/dri/renderD128` | 226,128 | `u:object_r:gmem_device:s0` | `crw-rw-rw- root graphics` | ENOTTY |
| `/dev/ion` | 10,127 | `u:object_r:ion_device:s0` | `crw-rw-r-- system system` | open EACCES |
| `/dev/nxwk` | 10,118 | `u:object_r:nexus_wake_device:s0` | `crw-rw-rw- system system` | (未使用) |
| `/dev/input/event*` | 13,64–71 | `u:object_r:input_device:s0` | `crw-rw---- root input` | — |
| `/dev/bcmpip` | 10,126 | `u:object_r:bcmpip_device:s0` | `crw-rw-rw-` | 別機能 |
| `/dev/hddidentify` | 10,119 | `u:object_r:fuse_device:s0` | `crw-rw-r--` | 別機能 |

**全 `/dev` 再帰走査の結果、`gpu_device` ラベルは nexusmem / nxmw の 2 ノードのみ。** タスク前提の「`/dev/dri/*` が gpu_device かも」は否定された（`gmem_device` であり、V3D/bgem 系ドライバ）。

`gpu_device` は CIL 上 `dev_type` かつ `mlstrustedobject` の_member_であり、AOSP ポリシで `base_typeattr_596 = (and (appdomain) (not (isolated_app_all)))` に対して `allow ... gpu_device (chr_file (ioctl read write ...))` が付与されている。すなわち **untrusted_app を含む通常アプリが open/ioctl 可能**（前提どおり稀有なクラス）。ただし ioctl は allowxperm で追加制限される。

### A-2. ロード済みポリシの特定

- 配布 `.cil` (plat / vendor / plat_pub_versioned / system_ext / product) を grep しても `allowxperm` は **0 件**。実機で効いているのは `/vendor/etc/selinux/precompiled_sepolicy`（792722 bytes）である。
- これをホスト側に pull し、setools 4.6.0 の `seinfo` / `sesearch` で解析（ツールは `/tmp/opencode/setools` に deb 展開、システムには未インストール）。

```
seinfo: Policy Version: 30 (MLS enabled), Classes 104, Types 1950, Attributes 183
        Allow 33010, Allowxperm 2478, Neverallowxperm 0,
        Auditallowxperm 0, Dontauditxperm 732, Permissives 0
```

### A-3. shell → gpu_device の基本 allow と xperm ゲート

`sesearch` 実出力（引用）:

```
type shell, appdomain, coredomain, domain, mlstrustedsubject, netdomain;

allow shell gpu_device:chr_file { append getattr ioctl lock map open read watch watch_reads write };
allow shell gpu_device:dir     { getattr ioctl lock open read search watch watch_reads };

allowxperm appdomain gpu_device:chr_file ioctl 0x0200;
allowxperm appdomain gpu_device:chr_file ioctl 0x0400;
...
allowxperm appdomain gpu_device:chr_file ioctl { 0x0000 0x0008 0x000e 0x0014-0x0016 0x0018 0x0036 };
allowxperm appdomain gpu_device:chr_file ioctl { 0x0a00 0x0a0e 0x0a15-0x0a16 0x0a19 0x0a1f 0x0a22-0x0a23 0x0a25-0x0a2a 0x0a31 };
allowxperm appdomain gpu_device:chr_file ioctl { 0x1600-0x1604 0x1608 0x160e-0x160f 0x1611 0x1617 0x161b 0x1622 };
allowxperm appdomain gpu_device:chr_file ioctl { 0x3000-0x3004 0x3007-0x3008 0x300a-0x300b 0x300d 0x3011 0x3014 };
allowxperm appdomain gpu_device:chr_file ioctl { 0x3a00 0x3a10-0x3a12 0x3a14-0x3a16 0x3a18 0x3a1a-0x3a20 0x3a22 };
allowxperm appdomain gpu_device:chr_file ioctl { 0x4202 0x4210-0x4213 0x4215 0x4217-0x4219 };
allowxperm appdomain gpu_device:chr_file ioctl { 0x6400 0x6411-0x6413 0x6415-0x6417 0x641c };
```

`shell ∈ appdomain` なので shell はこの白リストを継承する。**直接 `allowxperm shell gpu_device` は存在せず、全て appdomain 継承**。`untrusted_app` はさらに `untrusted_app_all gpu_device:chr_file ioctl <同系統>` も持つ。`system_app` も `allowxperm system_app gpu_device:...` を追加で持つが、範囲は同じ系統で 0x5a25 は含まれない。

**重要**: `0x5a25` は「type=0x5a, nr=0x25」。白リストの type は `0x00,0x0a,0x16,0x30,0x3a,0x42,0x64` のみで **0x5a が欠落**。特に vendor 特権ドメイン (`nxserver`, `stbcored`, `bcmstb_app`, `tee`, `sldr`, `hal_cas_default`, `nxlic`, `nxmini`, `nxdispfmt` 等) は `0x0200-0x0207`, `0x0400-0x04ff`, ..., `0x3c00-0x3c16`, `0x6400-0x641c` と広いレンジを持つが、**0x3c16 と 0x6400 の間（0x5a00-0x5aff を含む）が意図的に空けられている**。

### A-4. 0x5a25 の全域検索

```
$ sesearch precompiled_sepolicy --allowxperm -x 0x5a25
(出力 0 行)

$ grep -n "0x5a" policy_sesearch.txt   # gpu_device allowxperm セクション
(0x5a を含むルール行なし。ヒットは検索コマンド見出しのみ)
```

対照実験: `-x 0x4202` と `-x 0x6411` は複数ルールを返し、検索手法が正しいことを確認済み。`neverallowxperm` は 0 件。

=> **0x5a25 は「全ドメイン・全オブジェクト型で allowxperm が存在しない」= カーネル実装はあるが誰も呼べないコマンド**。

### A-5. 「別デバイスに振り替えたら通るか」

| 試行 | 結果 |
|---|---|
| `ioctl(nexusmem, 0x65a25)` | EACCES (AVC: `ioctlcmd=0x5a25 tcontext=gpu_device`) |
| `ioctl(nxmw, 0x65a25)` | EACCES (同上) |
| `ioctl(renderD128, 0x65a25)` | **ENOTTY** — SELinux は通るが V3D/DRM ドライバが未実装 |
| `ioctl(card0/card1, 0x65a25)` | ENOTTY (同上) |
| `/data/local/tmp/nexuslink -> /dev/nexusmem` 経由 | EACCES (シンボリックリンクでも inode の gpu_device が効く) |
| `/proc/self/fd/N` 経由 | open 不可 |
| `mknod /data/local/tmp/nexusalias c 10 117` | **EPERM** (CAP_MKNOD 無し) — 別ラベルでノード再生成不可 |

gmem_device には allowxperm が無いため 0x5a25 は SELinux を通過する（renderD128 で ENOTTY になる事実が証明）。しかし機能を持つドライバは gpu_device 側だけなので、**「SELinux だけを回避しても機能には到達しない」**。

---

## TASK B — ioctlcmd 白名单の実測

### B-1. ポリシ許可 xperm の全走査

`appdomain gpu_device` の whitelist を展開した **106 個**の xperm について、完全な `_IOC` エンコード `cmd = 0x00060000 | xperm`（size=6, type=高バイト, nr=低バイト）で zeroed 64B バッファを渡して実測（`tools/docs/phase37_raw/sweep_allowed.c`, 生データ `sweep_nexusmem.txt` / `sweep_nxmw.txt`）。

- 106/106 が ENOTTY (nexusmem) または EFAULT (nxmw; 独自バッファ形式必須) または EINVAL / ENOSPC。
- **rc>=0 かつバッファにデータが入ったもの: 0 件**。
- EFAULT/範囲外/パニック/再起動 なし。

既知の観測（PHASE33/34 の gmem_device 側の話であり gpu_device ではない）:
- renderD128 の nr `0x00/44/49/4a/4b → 0`, `0x46/48 → 216`, `0x47 → 296`, `0x0c → EOPNOTSUPP`, `0x41 → ENOSPC`, `45nr → EACCES` (DRM render-node の `DRM_RENDER_ALLOW` ゲート), `20nr → EINVAL`。
- これら gmem_device の vendor ioctl (0x41/0x42/0x45) は構造体先頭 u32 を解析して 16B を書き戻すが、値は **fd 単位の handle に比例する小さな固定 GPU/物理アドレス** (`0xea2000`, `0x3e95000`, `0xa3d00000` 等) で、`0xffffff...` のカーネル仮想アドレスは出ない（PHASE34 で確定）。
- renderD128 で `0x6411`/`0x641c` が EACCES になるのは **DRM コアの権限ゲート**で、AVC は出ていない = SELinux ではない。

### B-2. エイリアス／ふりをした呼び出しの検討

- `_IOC` の size/dir ビットを変えても SELinux が見るのは cmd の下位 16bit (`ioctlcmd=0x5a25`) のみ。下位 16bit を 0x5a25 にしない限り get_mmap_offset のハンドラには到達しない。
- `0x60a25` (type=0x0a, nr=0x25; appdomain で許可) を nexusmem に投げても ENOTTY。**許可された type 0x0a の nr 0x25 は別機能**で、0x5a25 のエイリアスではない。
- 全 106 許可 cmd にデータ/アドレスを返すものが無いため、「任意のカーネルアドレスを引数に取る cmd」も存在しない。

=> **get_mmap_offset 相当機能を許可 cmd のふりをして呼ぶことは不可能**。

---

## TASK C — hwbinder/binder の SELinux 代行

### C-1. サービス一覧

- `service list`: **242 サービス**を `docs/phase37_raw/service_list.txt` に保存。
- `vndservice` バイナリは端末に存在せず（`inaccessible or not found`）。hwbinder 側は `lshal -l -a` で **161 行**を `docs/phase37_raw/lshal.txt` に保存。
- 注目サービス:
  - `bcm.hardware.nexus.INexus/default` (pid 246 nxserver, uid system)
  - `bcm.hardware.{bp32.Ibp3, cchak2.ICcHak, dspsvcext2.IDspSvcExt, prdysc.IPrdySc, ssdsync.ISsdSync, svpsync2.ISvpSync, vcsvc2.IVcSvc}/default` — いずれも nxserver (pid 246) ホスト
  - `SurfaceFlinger` / `SurfaceFlingerAIDL`, `gpu` (`android.graphicsenv.IGpuService`, pid 498), `media.camera`, `media.player`, `android.hardware.graphics.allocator@2.0`, `android.hidl.allocator@1.0/ashmem`
  - `sei.hardware.stbapi@1.0::IStbapi/default` は **lshal 上 `cannot be fetched from service manager (null)`** = 実質登録なし（プロセス pid 484 root は生存）。

### C-2. shell の binder 呼び出し権限（`sesearch -s shell -c binder -p call`）

```
allow shell nxserver:binder { call transfer };
allow shell gpuservice:binder { call transfer };
allow shell hal_graphics_allocator_server:binder { call transfer };
allow shell hal_cas_server:binder { call transfer };
allow shell hal_drm_server:binder { call transfer };
allow shell hal_codec2_server:binder { call transfer };
allow shell hal_omx_server:binder { call transfer };
allow shell servicemanager:binder { call transfer };
allow shell hwservicemanager:binder { call transfer };
allow appdomain binderservicedomain:binder { call transfer };  # 大半のシステムサービス
... (全 24 規則を docs/phase37_raw/policy_sesearch.txt に保存)
```

`service call` 自体は `fd`/`nfd`/`afd` で **任意の fd を parcel に渡せる**。つまり「fd を受け取って ioctl するメソッド」が存在すれば代行可能だった。

### C-3. 代行メソッドの洗い出し（ioctl 観点の再調査）

- **uid1000 /system アプリ** (PHASE31 再確認): neosystem2 の 5 Messenger / 3 receiver は `int msg.what/arg1` のみ、実行シンク (Runtime.exec / DexClassLoader / WebView.loadUrl) ゼロ、`ProcessBuilder` は固定 `logcat -d` のみ。**任意 ioctl / 任意 fd を扱う面なし**。
- **nxserver ホストの AIDL** (PHASE5/26 再確認): `bcm.hardware.nexus-V1-ndk.so` の 22 メソッドは setMaxPerf / register*Cb / getSuspendMode* / resetSuspendMode / assignSuspendMode / doesTvSupportDolby / lpmUserPreferences / rmlmk / client / getPwr/setPwr / setWOL / loadPak / acquireWL/releaseWL / forcedPCM / getInterfaceVersion/Hash。**fd / handle / object / shared / demux / stream / buffer / mmap 系メソッド名は 1 つも無い**。実 call も code 1..80 × 複数引数で非デフォルト応答は code 2/5 のみ、それ以外は `-74 EBADMSG ("Not a data message")`。本次も `service call ... 0x01000001` が `-74` を返すことを再確認。
- **標準 buffer/fd 返しサービス**:
  - `SurfaceFlinger` / `SurfaceFlingerAIDL`: `captureDisplay` 等は自前の GraphicBuffer を返す。渡した fd に対して ioctl はしない。
  - `gpu` (`IGpuService`): ドライバ情報/機能照会のみ (`dumpsys gpu` は stable/pre-release driver, memory snapshot, バージョン情報)。buffer/fd/任意 ioctl の面なし。
  - `graphics.allocator@2.0`, `hidl.allocator@1.0/ashmem`: `allocate(size)` が dmabuf/ashmem の fd を**返す**が、呼び出し側が渡した fd に ioctl するメソッドは無い。返るのは自前確保バッファで、Nexus メモリブロックではない。
  - `media.camera` / `media.player` / `media.extractor`: `ParcelFileDescriptor` を受け取るが、操作は read/lseek であり任意 ioctl ではない。
  - `IMediaCasService` / `IDrmFactory` (clearkey/playready/widevine): セッションと鍵要求のみ、fd/ioctl 転送なし。
- **決定的補足**: 仮に「任意 fd に ioctl する」代行サービスが見つかっても、**全ドメインが 0x5a25 の allowxperm を持たない**ため代行側も EACCES になる。したがって confused-deputy は原理的に成立しない。

=> **binder/hwbinder 経由の SELinux 代行経路は存在しない。**

---

## TASK D — 結論

### 越えられない根拠（引用付き）

1. **構造的理由**: SELinux の ioctl 制御は二段。`allow shell gpu_device:chr_file { ... ioctl ... }` で「ioctl .permission_」が許可され、さらに `allowxperm <domain> gpu_device:chr_file ioctl <cmd>` で「どの cmd か」が制限される。shell は前者を持つが、後者は appdomain 経由の白リストのみ。
2. **白リストに 0x5a25 が無い**: `appdomain gpu_device` の allowxperm は type `0x00/0x0a/0x16/0x30/0x3a/0x42/0x64` に限られ、`0x5a` type は **全ドメインで欠落**。`sesearch --allowxperm -x 0x5a25` は 0 件。特権 vendor ドメインですら 0x3c16→0x6400 のギャップがあり 0x5a00-0x5aff は明示的に除外されている。
3. **機能ノードが gpu_device に限られる**: 0x65a25 を実装する `/dev/nexusmem` (10,117) と `/dev/nxmw` (10,116) のみが `gpu_device`。`/dev/dri/*` は `gmem_device` で別ドライバ、0x5a25 は ENOTTY。
4. **代替ラベル生成不可**: `mknod` は EPERM、symlink / `/proc/self/fd` は inode の gpu_device コンテキストが維持される。
5. **代行不可**: 任意 fd を渡せる binder サービスは存在しても、0x5a25 を発行できるドメインが存在しない（上記 2）。
6. **許可 cmd 側に代替機能なし**: 106 個の許可 xperm の全走査でデータ/アドレス返しは 0 件、カーネルアドレスを引数に取る cmd も無し。

**=> PHASE36 の決定的事実「カーネルは機能を持つが SELinux が塞いでいる」は維持され、本フェーズでその SELinux 壁に抜け道が無いことをポリシ原文と実測の両面から確定した。**

### 実現可能な唯一のルート（変更なし）

コード/設定の改変を伴わない shell からのリアルタイム TS 取得は不可能。PHASE36 と同様、**GPL ソース開示要求 (`brcmv3d.ko` / Nexus ドライバ) による static 解析**が唯一の現実的次の一手（ただし本タスクの禁止事項に抵触しない範囲の調査のみ）。

### 安全性 / 健全性

- 全操作後 `sys.boot_completed=1` 維持、uptime 継続、再起動 / パニック / tombstone 増加なし。
- 書き込みは `/data/local/tmp/{ioctl_probe,ioctl_full,sweep_allowed}` と `docs/` のみ。system / vendor / recovery / bootloader への書込なし。録画・BIPBIND の LISTEN/ACCEPT・reboot 系 ioctl は一切呼んでいない。

### 成果物

- 本ドキュメント: `docs/PHASE37_SELINUX.md`
- 生データ / ツール: `docs/phase37_raw/`
  - `policy_sesearch.txt` (1980 行: sesearch 全結果), `KEY_RULES.txt` (引用用抜粋)
  - `service_list.txt` (242 サービス), `lshal.txt`, vndservice_contexts 等
  - `ioctl_probe.c`, `ioctl_full.c`, `sweep_allowed.c`, `allowed_xperms.h`
  - `sweep_nexusmem.txt`, `sweep_nxmw.txt` (106 xperm 走査結果)
  - `vendorlibs/bcm.hardware.nexus-V1-ndk.so` (メソッド名確認用)
- pull したポリシ: `docs/sepolicy_pull/{precompiled_sepolicy,vendor_sepolicy.cil,plat_pub_versioned.cil}`
