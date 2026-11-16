# Phase 27 — bipc wire cmd 列挙 / AuthenticatedJoin auth 構造体 / 汎用 ioctl 転送の有無

Date: 2026-11-17
Device: `adb -s 192.168.1.119:5555`, shell `uid=2000`, SELinux enforcing,
`5.4.286-ab0600`, armv7a userspace.
Safety: no LISTEN/ACCEPT, no recording, no reboot, no unknown-ioctl sweep.
Only two *known* ioctls were issued in the forwarding probe (`0x40014202` CONNECT,
`0x6538a0`, `0x650002`). Health checked after every group; final
`sys.boot_completed=1`, `up 1:34`, adb online.

## 結論（先に）

| 項目 | 結果 |
|---|---|
| Task1 bipc cmd id | **cmd は 0..61 の 62 種、すべて libnxclient.so のラッパ関数に固定。** r2 (cmd id) が関数引数から来る呼び出しは 1 件も無く、**IOCTL / RAW 相当の汎用転送 cmd は存在しない**。 |
| Task1 汎用 ioctl 転送 | libnexus の `proxy_ioctl_fd` 経由の `0x65xxxxxx` 空間も試行。**CONNECT 済みクライアント fd では `0x6538a0` も `0x650002` も EACCES (errno=13)**。リモート転送不可。 |
| Task2 auth 行列 | **24 通り（authMode 0/1/2 × pwfile あり/なし × env あり/なし × offset +0x30/+0x88）すべて結果同一**。auth 構造体の値は結果を変えない。 |
| Task2 offset | 入力 `NxClientJoinSettings` の `authMode` は **+0x30 が正**（`GetDefaultJoinSettings` の `str [r4,#0x30]` で確定）。+0x88 は PHASE25 の**応答バッファにエコーされた値の位置**であり構造体フィールドではない。 |
| Task2 rc=0 | `NxClient_Config_GetJoinSettings` の**第 1 引数を 0 にすると rc=0** になる（PHASE25 が渡したポインタ値では rc=2）。ただし返る 312B は +0x30=1 の 1 ワードだけで **name は空 → 健全な構造体ではない**。 |
| Task3 既定 auth | `NEXUS_Platform_GetDefaultClientAuthenticationSettings` は **260B を zero clear するだけ**（Magnum_Init を呼び失敗する副作用あり）。`NxClient_Join` はその値を libnxclient+0xb4f0 の静的テンプレートで上書きするが、**テンプレートも実測で全ゼロ**。 |
| Task3 AuthenticatedJoin | 既定/静的テンプレートどちらも rc=**0x8**（Join 前）／ rc=**0xa**（Join 後）。変化なし。 |
| 真 join / TS | **不成立。** `Platform_GetConfiguration` は零、`Recpump_Open(0)=NULL` のまま。PHASE3/25 の判定を維持。 |

---

## Task 1 — bipc wire プロトコルと cmd id

### 1.1 要求ヘッダ（libnxclient.so 逆アセンブルで確定）

`bipc_client_send` @0x8d90 は要求バッファ `client+0x1c` に 16B ヘッダを書き、
`b_safe_write(fd, client+0x1c, payload+16)` で送信する。

| off | size | 意味 | 根拠 |
|---|---|---|---|
| 0x1c | 4 | `total_size = payload_size + 16` | `add r2,r4,#16` → `str r2,[r5,#0x1c]` |
| 0x20 | 4 | target handle（`-1` = create、通常は server object id） | `ldm r4,{r0,r1}` の r1 を `str r7,[r5,#0x20]` |
| 0x24 | 4 | **cmd id** | `mov r2,#6` 等を `str r6,[r5,#0x24]` |
| 0x28 | 4 | 定数 32 | `mov r3,#32` → `str r3,[r5,#0x28]` |

応答は 8B ヘッダ `{total_size, BERR_code}` + payload。`BERR_code` が
`nxclient_p_config_get_join_settings` の戻り値になる（`ldr r7,[r6,#0x13c]`）。
例: cmd 6 の payload 316B（先頭 4B = 呼び出し側第 1 引数、残り 312B =
settings）、応答 312B + status。cmd 13 (`nxclient_p_create`) は
76B ヘッダ + 312B settings = 392B 要求、392B 応答（先頭に server handle）。

### 1.2 cmd id 全表（62 件、`bl 0x8d90` 呼び出しサイトを全走査）

| cmd | 関数 | cmd | 関数 |
|---|---|---|---|
| 0 | acknowledge_standby | 31 | get_status |
| 1 | alloc | 32 | get_surface_client_composition |
| 2 | check_complete_init | 33 | get_thermal_configuration |
| 3 | config_get_connect_list | 34 | get_thermal_configuration_list |
| 4 | config_get_connect_settings | 35 | get_thermal_status |
| 5 | config_get_input_client_server_filter | 36 | grow_heap |
| 6 | **config_get_join_settings** | 37 | hdmi_output_get_crc_data |
| 7 | config_get_surface_client_composition | 38 | load_audio_dolby_tuning_file |
| 8 | config_lookup_client | 39 | load_default_dolby_tuning_profile_settings |
| 9 | config_refresh_connect | 40 | load_hdcp_keys |
| 10 | config_set_input_client_server_filter | 41 | reconfig |
| 11 | config_set_surface_client_composition | 42 | refresh_connect |
| 12 | connect | 43 | register_acknowledge_standby |
| 13 | **create** (join 本体) | 44 | screenshot |
| 14 | destroy | 45 | set_audio_processing_settings |
| 15 | disconnect | 46 | set_audio_settings |
| 47 | set_audio_system_settings | 48 | set_client_mode |
| 49 | set_display_settings | 50 | set_hdmi_avi_info_frame |
| 51 | set_hdmi_input_repeater | 52 | set_picture_quality_settings |
| 53 | set_slave_display_graphics | 54 | set_standby_settings |
| 55 | set_surface_client_composition | 56 | set_thermal_configuration |
| 57 | set_watchdog_timeout | 58 | shrink_heap |
| 59 | unregister_acknowledge_standby | 60 | write_closedcaption |
| 16 | display_get_crc_data | 61 | write_teletext |
| 17 | display_set_cgms | | |
| 18 | display_set_cgms_b | | |
| 19 | display_set_macrovision | | |
| 20 | display_set_wss | | |
| 21 | free | | |
| 22 | get_audio_processing_settings | | |
| 23 | get_audio_settings | | |
| 24 | get_audio_status | | |
| 25 | get_audio_system_settings | | |
| 26 | get_callback_status | | |
| 27 | get_display_settings | | |
| 28 | get_display_status | | |
| 29 | get_picture_quality_settings | | |
| 30 | get_standby_status | | |

全 62 呼び出しサイトで cmd id は **即値定数**。引数から cmd id を取る汎用
ディスパッチャは存在しない（`bipc_client_send` 自身の assert パス以外に
`mov r2, <register>` は 0 件）。よって「cmd=IOCTL / RAW で任意 NEXUS ioctl を
転送する」手段は bipc プロトコル層には無い。

### 1.3 libnexus の proxy ioctl 空間（代替候補、結果は否定的）

`libnexus.so` は `proxy_ioctl_fd` (vaddr 0x78e4e4) を持ち、各 `*_proxy` 関数が
`ioctl(proxy_ioctl_fd, 0x65xxxxxx, ...)` でサーバへ RPC する。 cmd 表の一部:

```
0x650001 GetDefaultMemoryConfigurationSettings  0x651200 frontend_init
0x650002 GetPlatformCapabilities                 0x653000 surface_init
0x650003 GetPlatformConfigCapabilities           0x653400 sync_channel_init
0x650011 Platform_GetDefaultSettings              0x653800 transport_init
0x650023 Platform_Init_tagged                    0x653a00 video_decoder_init
0x650036 Platform_Uninit                          0x6538a0 (= Recpump_Open, direct)
```

`nexus_proxy_platform_init(fd)` 自体が `ioctl(fd, 0x650000,
{0x44539be1, global_fd})` で proxy fd を登録する設計で、
`NEXUS_Platform_P_Magnum_Init` が platform fd に対して呼ぶ。

**実測 (v=400/401):** `/dev/nxmw` を CONNECT して fd を得、
`proxy_ioctl_fd` にその fd を直接書き込んで
`NEXUS_GetPlatformCapabilities_tagged_proxy` を呼ぶと
戻り値は垃圾 (0xfa82b242) でバッファは全零。さらに同じ fd へ
`ioctl(fd, 0x650002, ...)`, `ioctl(fd, 0x6538a0, ...)` を 1 回ずつ発行すると
**両方 rc=-1 / errno=13 (EACCES)**。

→ proxy 空間もクライアント (CONNECT) チャネルでは拒否される。
PHASE3 §35.6 の「transport 系 ioctl はサーバチャネル限定」判定は
proxy ioctl 空間にも及ぶ。**111 コマンドのリモート転送は不可能**。

---

## Task 2 — auth 構造体の全パターン

### 2.1 offset の訂正

- 入力 `NxClientJoinSettings` (312B) の `authMode` は **+0x30**。
  `NxClient_GetDefaultJoinSettings` @0x7fcc が `mov r0,#2` /
  `str r0,[r4,#0x30]` で書き、password 成功時は 0x808c で +0x30=1、
  失敗時は 0x8084 で +0x30=3。PHASE4 の +0x30 は正しい。
- タスク文の +0x88 は、PHASE25 が `Config_GetJoinSettings(settings2,
  settings2)` と呼んだ際の**応答バッファ内で authMode がエコーされていた位置**。
  入力構造体のフィールドではない。実測では +0x88 を書き換えても応答は不変。
- `NEXUS_ClientAuthenticationSettings` (260B) は別構造体。**全ゼロ**で初期化される
  （下記 Task 3）。

### 2.2 行列結果（`tools/nxphase27.c`, harness は `tools/libnxjni27.so`）

12 通り（+0x30 指定）× 12 通り（+0x88 指定）＝ **24 プロセスを個別実行**。
全組み合わせで以下が同一:

```
GetDefaultJoinSettings rc=0x2 (env無し) / 0x1 (env有り, +0x34="testpw")
NxClient_Join rc=0x0
CGJS(firstArg=0) rc=0x0  nonzero312=1  name=[]  auth@0x30=1  auth@0x88=0
CGJS(firstArg=1..7) rc=0x2 (payload は同じ)
Platform_GetConfiguration: cfg 全零
Recpump_Open(0)=0x0
AuthJoin(getter_zeroed)=0xa
```

**authMode 0/1/2、password file あり/なし、`NXCLIENT_PASSWORD` あり/なし、
+0x30/+0x88 のどの組み合わせも結果を変えない。**
`NxClient_Join` は AuthJoin に **libnxclient 内の静的テンプレート**を渡すため
（`NEXUS_Platform_GetDefaultClientAuthenticationSettings` の戻り値を
`memcpy` で上書き、0x66a0-0x66b4）、join settings 側の auth/password は
AuthJoin に影響しない。

### 2.3 `Config_GetJoinSettings` の第 1 引数

PHASE25 は第 1 引数に 4096B バッファの**ポインタ値**を渡し rc=2 を得た。
本セッションで 0..7 を試した結果:

- firstArg=0 → rc=0、応答 312B は `01 00 00 00` (+0x30) のみ。
- firstArg=1..7 → rc=2、応答 payload は同じ。

内部 `nxclient_p_config_get_join_settings` @0x4d5c は第 1 引数を要求 payload
先頭 4B にそのまま格納するだけ（参照外しなし）。0 は「既定クライアント」として
解釈され rc=0 になるが、返る settings は name が空で **判定基準（name/authMode
が健全）を満たさない**。`Platform_GetConfiguration` / `Recpump_Open` も
変化なし。よって **rc=0 は真 join の証明にならない**。

---

## Task 3 — 既定 auth 構造体をそのまま AuthenticatedJoin へ

`NEXUS_Platform_GetDefaultClientAuthenticationSettings` @0x641b10 の実体:

```
push {r4,lr}; mov r4,r0
bl NEXUS_Platform_P_Magnum_Init      ; ここで失敗 (rc=8)
mov r0,r4; mov r1,#260
bl __aeabi_memclr4                   ; 260B をゼロクリア
mov r0,r4; pop {r4,pc}               ; 戻り値 = 呼び出し側ポインタ
```

実測 (v=100/101): 返った 260B は **全ゼロ**。`NxClient_Join` が使う
libnxclient+0xb4f0 の静的テンプレート（dlsym アドレスから base を算出して
ダンプ）も **全ゼロ**だった。

| 呼び出し | rc |
|---|---|
| `AuthJoin(GetDefault... のゼロ構造体)`、Join 前 | **0x8** |
| `AuthJoin(libnxclient 静的テンプレート)`、Join 前 | **0x8** |
| `AuthJoin(GetDefault... のゼロ構造体)`、`NxClient_Join` 後 | **0xa** |

(PHASE25 が 0xa と記録したのは Join 後の値。本セッションでは Join 前 0x8 も
追加で観測。**既定値を渡しても rc は改善しない**。)

---

## 最終判定と次の一手

**真 join は不成立、TS データパスなし。** PHASE3 第 34/36 章、PHASE25 の
結論を維持する。今回新たに確定したのは:

1. bipc の cmd 空間は 0..61 の固定 RPC セットで、汎用 ioctl 転送 cmd は無い。
2. libnexus の proxy ioctl 空間 (0x65xxxxxx) もクライアント fd では EACCES。
   よって「111 コマンドをリモート転送して 0x6538a0 を通す」道は無い。
3. auth 構造体は入力・既定値ともに全ゼロで、どの組み合わせも join を変えない。
   ボトルネックは認証情報ではなく **Magnum デバイス所有
   (`NEXUS_Platform_P_Magnum_Init` = rc 8) を nxserver が排他的に保持している
   こと** confirmed。

次の一手（優先順）:

1. STB-2 からの shell 経由 TS は設計的に不可と断定して良い。mirakc への
   TS 配送は `ts-delivery-route-map-2026-11-17` の他経路を優先。
2. どうしても STB-2 を使うなら、nxserver と同じ権限でプロセスを載せる
   （system uid / 署名付き APK）しか残っていない。root も LISTEN も無い
   現在の shell では不可。
3. 学術的に残る点は `nxclient_p_create` (cmd 13) 応答の server handle と
   `NxClient_Connect` の意味、および `proxy_ioctl_fd` を正しく登録できる
   権限（LISTEN 側）の特定のみ。実用上の前進は期待できない。

## 参照 / 成果物

- 逆元: `refs/device/libnxclient.so`, `refs/device/libnexus.so`
  (`llvm-objdump -d`, `readelf -sW`)
- Harness: `tools/nxphase27.c`, ビルド済 `tools/libnxjni27.so`
  （`clang --target=armv7a-linux-androideabi21 -shared -nostdlib -fPIC -O1
  -Wl,--allow-shlib-undefined -L libs -l:libdl.so -l:libc.so`）
- 過去: `docs/PHASE3_SHELL_JAVA_STBAPI.md` §29/31/35,
  `docs/PHASE4_NEXUS_API_REPORT.md` §1.3/§2, `docs/PHASE25_NXCONNECT.md`
- 後始末: 端末 `/data/local/tmp/libnxjni.so` は元バックアップへ復元、
  一時ログ/password file を削除。端末健全 (`1 / up 1:34`)。
