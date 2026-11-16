# PHASE26 — HIDL/AIDL Nexus HAL の fd/handle 返却メソッド調査

Date: 2026-11-17. Target: STB-2 / C02AS5 @ `192.168.1.119:5555` (Android 14, `UTT1.250730.001`, armeabi-v7a, boot_completed=1).
目的: 放送中リアルタイム TS 取得のため、Nexus HAL に `ParcelFileDescriptor` / `sharedHandle` / `objectId` を返すメソッドが無いかを HIDL・AIDL 双方で確定する。
禁止事項遵守: 未知 ioctl の連発なし、録画なし、reboot なし、LISTEN なし。全操作は読み取り/service call のみ。

---

## 結論（先に要約）

| 経路 | 結果 |
|---|---|
| HIDL `bcm.hardware.nexus@1.0`〜`@1.3::INexus` | **現行ビルドに存在しない**。hwservicemanager 未登録、カーネル内にも .so 無し。トランザクション総当たりは対象不在のため実行不能。 |
| AIDL `bcm.hardware.nexus.INexus/default` code 1..30 | PHASE5 済み: power/suspend/WOL/perf/callback のみ。fd/handle 無し。 |
| AIDL code 31..80（本フェーズで追加） | 150/150 呼び出しが `0xffffffb6 = -74 EBADMSG "Not a data message"`。fd/handle 返却メソッドは **存在しない**。 |
| 静的解析（V1-ndk.so / libnexus.so / OSS） | AIDL 22 メソッドを確認、いずれも fd/handle/object/shared 系なし。HIDL の .hal/.so は非公開（Broadcom proprietary）で入手不可。 |

→ **HIDL/AIDL 経由で fd/handle を返すメソッドは無く、code 番号も存在しない。この経路は完全に閉じた。**

---

## 1. HIDL サービス一覧（TASK 1）

### 1.1 実行結果

```
adb shell lshal | grep -i nexus          → (空)
adb shell lshal -a | grep -i nexus       → (空)
adb shell lshal -l | grep -i nexus       → (空)
adb shell ps -A | grep -i nexus          → (空)
   system 246 nxserver / 247 nxssd / 627 nxlic   ← プロセス名に nexus は無い
ls /vendor/lib*/bcm.hardware.nexus*      → bcm.hardware.nexus-V1-ndk.so のみ
find / -iname '*nexus*.so'               → V1-ndk.so, libnexus.so, libnexusir.so, libGLES_nexus.so
                                            (@1.0/@1.1/@1.2/@1.3 は **無し**)
```

 registered HIDL バージョン: **0 個**。`lshal` の全 103 行を確認したが `bcm.hardware.nexus` は 1 行も無い（bcm 付きは bluetooth/health の passthrough `-bcm` サフィックスのみ）。

### 1.2 なぜ「実在」と見えたか（前提の訂正）

`bugreport/br/FS/data/tombstones/tombstone_14:302-344`（および 13/15/17）に `@1.0`〜`@1.3` のマッピングが出ているが、これは **旧ビルドのクラッシュログ** である。

- tombstone_14 build fingerprint: `KDDI/C02AS5/C02AS5:11/RTT4.240113.001/0557` （**Android 11 / 2024-01**）
- tombstone_14 timestamp: `2025-06-29 04:59:05+0900`
- クラッシュプロセス: `/vendor/bin/hw/android.hardware.tv.cec@1.0-service.bcm`（HDMI-CEC HAL、Nexus クライアントとして libnexus.so をロード）
- 現行端末: Android 14 / `UTT1.250730.001`。tombstone は OTA をまたいで `/data/tombstones` に残存していたもの。

つまり HIDL Nexus HAL は **旧 Android 11 ビルドに存在したが、現行 Android 14 ビルドでは AIDL (`bcm.hardware.nexus-V1-ndk.so`) へ移行・削除された**。

### 1.3 VINTF による決定的確認

```
/vendor/etc/vintf/manifest.xml:143-146
    <hal format="aidl">
        <name>bcm.hardware.nexus</name>
        <fqname>INexus/default</fqname>
    </hal>
```

`format="aidl"` のみ。HIDL の `<hal format="hidl">` エントリは無い。`manifest_nexus_frag.xml` は現行ビルドに存在しない（パスの参照のみ残存）。

`/dev/block/by-name/` は `system` / `vendor` のみで A/B スロット無し → 旧ビルドの vendor パーティションも残留していない。

---

## 2. HIDL トランザクション総当たり（TASK 2）— 実行不能の理由

対象サービスが存在しないため、`bcm.hardware.nexus@1.0::INexus/default` / `@1.3::INexus/default` への `getService` は必ず null を返す。根拠は 3 独立経路:

1. `lshal`（hwservicemanager への実 hwbinder クライアント）が全登録サービスを列挙したが nexus は 0 件。
2. VINTF manifest が `format="aidl"` のみを宣言。
3. `/vendor/lib` に HIDL `.so` が存在せず、passthrough (`HIDL_FETCH_INexus`) もあり得ない。

`lshal` 自身が正規の hwbinder getService を行うため、独自の hwbinder ハーネスを組んで `getService` を再確認しても結果は同一（null）と判断し、無意味な ioctl 連発を避けるため生ハーネスの新規作成は行わなかった。既存 `tools/nshal.c` は AIDL NDK `.so` の JNI ローダであり、HIDL hwbinder ハーネスでは無い（再利用不可）。

> 前提.doc: もし旧 Android 11 ビルドの実機イメージ / `bcm.hardware.nexus@1.3.so` が別途入手できれば、その時点で初めて transaction code 1..60 × 4 引数バリアントの総当たりが可能。現行端末では不可能。

---

## 3. AIDL untested code 31..80（TASK 3）

PHASE5 が code 1..30 × 3 バリアントを実施済み。本フェーズで 31..80 を同一手法で追加:

```
for c in 31..80:
  service call bcm.hardware.nexus.INexus/default $c                # 空
  service call bcm.hardware.nexus.INexus/default $c i32 0          # i32
  service call bcm.hardware.nexus.INexus/default $c i32 0 s16 test # i32 + s16
```

結果: **150 / 150 が同一**

```
Result: Parcel(Error: 0xffffffb6 "Not a data message")
```

`0xffffffb6 = -74 = EBADMSG`。Parcel に fd も 16 バイト以上の handle らしきデータも一切入らない。生ログ: `docs/phase26_raw_aidl_31_80.txt`（300 行）。

ちなみに PHASE5 の 1..30 でも非デフォルトデータは code 5 の 36 バイト固定構造体と code 2 の 20 バイト errno 構造体のみで、これらは Nexus IPC のエラー/状態構造体であり TS ではない。

---

## 4. 静的解析（TASK 4）

### 4.1 AIDL HAL `.so`（実在、取得済み）

`adb pull /vendor/lib/bcm.hardware.nexus-V1-ndk.so` → `refs/device/bcm.hardware.nexus-V1-ndk.so` (58,124 B)。

`readelf -Ws` の `BpNexus`（= AIDL プロキシ = 公開メソッド一式）から 22 メソッドを抽出:

```
setMaxPerf(string,int)      registerDspCb/HpdCb/StdCb(cb)
getSuspendMode              getSuspendModeLocked
resetSuspendMode            assignSuspendMode
initSavedSuspendMode        doesTvSupportDolby
lpmUserPreferences          rmlmk
client(int, void*)          getPwr / setPwr(NexusPowerState)
setWOL(string)              loadPak
acquireWL / releaseWL       forcedPCM
getInterfaceVersion / getInterfaceHash
```

`strings`  grep（`getFd|getHandle|sharedHandle|objectId|AcquireObject|ParcelFileDescriptor|demux|stream|recpump|shared|memory|buffer`）に hit したのは型名 `INexus`, `INexusDspCb/HpdCb/StdCb`, `NexusPowerState` と AIDL ランタイム文字列だけで、**fd/handle/object/demux/stream 系のメソッド名は 0 件**。`NexusStatus` / `NexusSuspendMode` / `NexusPowerState` が返すのは全て enum/int 構造体。

### 4.2 `libnexus.so`（7.9 MB、`refs/device/libnexus.so`）

`strings | grep -iE 'getFd|getHandle|sharedHandle|objectId|AcquireObject|recpump|ParcelFileDescriptor|shared memory|demux|NEXUS_Demux|NEXUS_Stream|HANDLE'` は多数 hit するが、全て **ファイル再生用のミドルウェア**（`bavi_demux_*`, `bmpeg2pes_demux`, `bgem_alloc_with_handle`, `BHAB_GetInterruptEventHandle`, `BKNI_CreateEvent` 等）とカーネル HAB メッセージハンドルで、Binder 経由でアプリに fd/handle を渡す API ではない。PHASE4/25 の結論（Nexus クライアントの所有権が Magnum/AuthenticatedJoin に依存）と整合。

### 4.3 OSS 調査

- Vantiva `USW8052SKB_3.4.6-A` Open Source Report に `vendor/broadcom/bcm_platform/hals/nexus/1.1` 等が Apache-2.0 モジュールとして記載されているが、これは **ライセンス一覧でありソース提供ではない**。
- `vendor/broadcom/bcm_platform/hals/nexus/1.x` の `INexus.hal` / 実装ソースは公開リポジトリに存在しない（Broadcom proprietary）。
- XDA の AT&T TV C71KW-400 (BCM7271) スレッドにも `bcm.hardware.nexus@1.3::INexus/default` の起動失敗ログのみで、メソッド一覧の公開は無し。

→ HIDL `.so` の `onTransact` switch を逆アセンブルしてメソッド名を確定する手段は、現行端末にも OSS にも存在しない。

---

## 5. 次の一手（推奨）

HIDL/AIDL 経路は閉じた。リアルタイム TS 取得の残り候補は PHASE25 checkpoint の 3 つに絞られる:

1. **bipc wire cmd id 列挙 + AuthenticatedJoin auth 構造体** — `/dev/nxmw` は request/response チャネルとして動作確認済み（Config_GetJoinSettings の 312B ラウンドトリップ成功）。ボトルネックは `AuthenticatedJoin rc=0xa (BERR_UNKNOWN)` の認証構造体。`nxjoin5.c` の auth フィールドを NEXUS API リファレンス（PHASE4）と突き合わせて正しい struct を組むのが最短。
2. **NEXUS_Platform_AcquireObject 系の呼び出し規約** — objectId をHAL経由ではなく bipc 経由で得る道。HIDL の出番は無い。
3. （最後の手段）GPL ソース請求 (`docs/GPL_REQUEST_GUIDE.md`, draft 済み) で Broadcom から HIDL/BSP ソースを取得し、旧ビルド相当の `.so` を静的解析する。

**非推奨**: 未知 ioctl 連発、録画/DLNA/USB、reboot、LISTEN — いずれも禁止または既に negative result 確定。

---

## 6. 端末健全性

- 開始時: `sys.boot_completed=1`, Android 14, uptime `08:28:50 up 1:23`, load 0.11/0.30/0.34
- 終了時: `sys.boot_completed=1`, uptime `08:31:22 up 1:26`, load 0.25/0.32/0.35
- AIDL 150 呼び出し後も nxserver (pid 246) は生存、adb 接続維持。再起動・プロセス死・新規 tombstone なし。

## 成果物

- `docs/PHASE26_HIDL_NEXUS.md`（本文）
- `docs/phase26_raw_aidl_31_80.txt`（AIDL 31..80 × 3 バリアントの生ログ 300 行）
- `refs/device/bcm.hardware.nexus-V1-ndk.so`（静的解析用に pull した既存 AIDL HAL）
