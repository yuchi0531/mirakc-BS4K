# Phase 8 — Round 16: Helper APK exercises the exported recordext AIDL service

**Date:** 2026-11-17
**Device:** `adb -s 192.168.1.119:5555` (online)
**Goal:** Build/install a minimal debuggable APK with a real `ProcessRecord`, bind to the
exported, no-permission AIDL service, and call `IRecordExtInterface` via raw `Parcel`
transact — something `app_process` from shell could not do (no ProcessRecord).

## Result (TL;DR)

**SUCCESS.** The helper APK bound to `com.kddi.pd2.c02as5_recordext /
com.rovicorp.tokyo.recordext.ServiceProcess` and all five interface methods worked.
`postMessage()` returns real JSON. `schedule.add` is parsed correctly (once the required
`template` key is supplied) but **fails with `errCode 10013 / EPG_API_ERROR_GENERIC`** on
this STB — there is no EPG/DMS/candidate data loaded, so **no recording was created** and
`schedule.list` stayed empty. No file path, recording ID, DMS UUID, or URL was returned by
any command.

A second bind (optional) to neosystem2 `UploadService` via `Messenger` also worked; it
replies `arg1=0` for `what=0` and `arg1=-1000` for `what=1`.

---

## 1. Static facts confirmed before coding (from the target APK)

Decoded `apk/recordext.apk` with `aapt2 dump xmltree` and `dexdump`:

```
service com.rovicorp.tokyo.recordext.ServiceProcess
  android:exported=true
  android:foregroundServiceType=0x1          (dataSync)
  intent-filter actions: ...scheduleapi, ...channelupdate, ...kabutoaccess, ...
  NO android:permission
```

`IRecordExtInterface` (DESCRIPTOR `com.rovicorp.tokyo.recordext.IRecordExtInterface`):

| Method | Signature | transact code | Parcel marshalling |
|---|---|---|---|
| registerEventListener | `(IRecordExtCallbackInterface)V` | 1 | writeStrongInterface |
| unregisterEventListener | `()V` | 2 | token only |
| getMediaType | `()Ljava/lang/String;` | 3 | token only → readString |
| getOperatorCode | `()Ljava/lang/String;` | 4 | token only → readString |
| postMessage | `(Ljava/lang/String;)Ljava/lang/String;` | 5 | token + writeString → readString |

`ServiceProcess.onBind(Intent)` dispatches on `getAction()`: `scheduleapi` returns
`mRecordExtService` (the `IRecordExtInterface$Stub`). No caller-identity check.

`postMessage` command switch (`ServiceProcess$8`) routes schedule.add →
`JSDialogueApi.scheduleAdd(JSONObject)` → `ScheduleRecordingManager.addOperation(...)` →
`ScheduleItem.createInstance(type, json)` → `TimerScheduleItem(json)` for `type=1`.
`TimerScheduleItem.<init>` calls `json.getJSONArray("template").get(0)` — **`template` is a
required key**; omitting it throws `JSONException`, caught at the dispatcher, producing a
`null` reply (this is why the very first add attempt returned `null`).

---

## 2. Build (tools/helperapk/)

Files created (all under `tools/helperapk/`):

- `AndroidManifest.xml` — package `com.probe.rext`, `android:debuggable="true"`, activity
  `.MainActivity` exported, **`<queries>` declaring `com.kddi.pd2.c02as5_recordext` and
  `jp.co.sei.streamcruiser.app.neosystem2`**.
- `src/com/probe/rext/MainActivity.java` — bind, raw Parcel transact, logcat + private-file
  + external-file logging, optional `cmd.txt` batch input, Messenger probe.
- `build.sh` — javac → d8 → aapt2 link → add classes.dex → zipalign → apksigner.
- `rext-helper.apk` — signed with `~/.android/debug.keystore` (alias androiddebugkey / android).

Build command:

```
$ cd tools/helperapk && ./build.sh
OK: .../tools/helperapk/rext-helper.apk
```

---

## 3. The critical gotcha: Android 11+ package visibility

The first two builds **failed to bind**. Diagnosis:

```
D REXT : queryIntentServices count=0        # implicit, setPackage(target)
W ActivityManager: Unable to start service Intent { act=...scheduleapi
                   cmp=com.kddi.pd2.c02as5_recordext/...ServiceProcess } U=0: not found
D REXT : bindService returned=false
```

The service is installed and exported (visible in `dumpsys package`), but a `targetSdk=34`
app cannot *see* it without a `<queries>` entry — package-visibility filtering makes both
`queryIntentServices` and `bindService` treat it as "not found". Adding:

```xml
<queries>
  <package android:name="com.kddi.pd2.c02as5_recordext" />
  <package android:name="jp.co.sei.streamcruiser.app.neosystem2" />
</queries>
```

fixed it immediately:

```
D REXT : queryIntentServices count=1
D REXT :   resolves to ServiceInfo{1600209 com.rovicorp.tokyo.recordext.ServiceProcess}
D REXT : bindService returned=true
D REXT : onServiceConnected name=ComponentInfo{com.kddi.pd2.c02as5_recordext/
         com.rovicorp.tokyo.recordext.ServiceProcess} binder=android.os.BinderProxy@...
```

This is why the earlier shell/`app_process` path failed (no ProcessRecord) and why a plain
third-party APK initially also failed (package visibility). Both had to be solved.

---

## 4. Install & run

```
$ adb -s 192.168.1.119:5555 install -r tools/helperapk/rext-helper.apk
Success
$ adb -s 192.168.1.119:5555 logcat -c
$ adb -s 192.168.1.119:5555 shell am force-stop com.probe.rext
$ adb -s 192.168.1.119:5555 shell am start -n com.probe.rext/.MainActivity
$ adb -s 192.168.1.119:5555 logcat -d -t 600 | grep REXT
```

(Note: `adb install` first tried incremental install and fell back to streamed install —
both end in `Success`; harmless.)

Outputs are mirrored to:

- private: `/data/user/0/com.probe.rext/files/out.txt`
  (`adb shell run-as com.probe.rext cat /data/data/com.probe.rext/files/out.txt`)
- external: `/sdcard/Android/data/com.probe.rext/files/rext_out.txt`
- optional batch input: `/sdcard/Android/data/com.probe.rext/files/cmd.txt` (one JSON per line)

---

## 5. Raw outputs — final canonical run (2026-11-17 03:35:14)

Direct interface calls:

```
transact(3) ok=true result=                       # getMediaType()  -> "" (empty)
getMediaType =
transact(4) ok=true result=                       # getOperatorCode() -> "" (empty)
getOperatorCode =
```

`postMessage` commands and **exact JSON replies**:

| # | Request | Reply |
|---|---|---|
| 1 | `{"cmd":"schedule.list"}` | `{"result":true,"response":{"AutoRecList":[]}}` |
| 2 | `{"cmd":"candidate.list"}` | `{"result":true,"response":{"CandidateList":[]}}` |
| 3 | `{"cmd":"storage.check","request":{"storageid":"0"}}` | `{"result":false,"response":{"errCode":10013,"errMsg":"EPG_API_ERROR_GENERIC"}}` |
| 4 | `{"cmd":"getOperatorCode"}` | `{}` |
| 5 | `{"cmd":"request.checkdms"}` | `null` |
| 6 | `{"cmd":"schedule.suspendevent"}` | `null` |
| 7 | `{"cmd":"schedule.add","request":{"type":1,"nid":1,"tsid":1,"sid":1,"eid":0,"startdate":261118,"starttime":2100,"duration":5,"title":"PROBE","rating":0,"template":["VIEW"]}}` | `{"result":false,"response":{"id":"[0,0]","errCode":10013,"errMsg":"EPG_API_ERROR_GENERIC"}}` |
| 8 | `{"cmd":"schedule.list"}` (after add) | `{"result":true,"response":{"AutoRecList":[]}}` |

Additional batch (`cmd.txt`) probes:

```
{"cmd":"checkdms.request","request":{"nid":1,"sid":1,"startdate":261118,"starttime":2100,"duration":5,"dmsid":"0"}}  -> null
{"cmd":"storage.check","request":{"storageid":"1"}}      -> {"result":false,"response":{"errCode":10013,"errMsg":"EPG_API_ERROR_GENERIC"}}
{"cmd":"storage.check","request":{"storageid":" HDD "}}  -> (same 10013)
{"cmd":"schedule.listraw"}                                -> {"result":true,"response":{"AutoRecList":[]}}
{"cmd":"autorec.list"} / {"cmd":"timer.list"} / {"cmd":"repeat.list"} / {"cmd":"unknown.cmd"} -> {}
```

`schedule.add` was also tried with `sid` ∈ {1,2,3,100,101,200,300,400,500}, `type=0`, and
future `startdate` 261118 / 270101 — **every attempt returned the same 10013 error**.

### neosystem2 UploadService (Messenger)

```
UPLOAD bindService returned=true
UPLOAD connected ComponentInfo{jp.co.sei.streamcruiser.app.neosystem2/
         jp.co.sei.streamcruiser.core.upload.UploadService}
UPLOAD sent what=0 / what=1  (each with replyTo Messenger)
UPLOAD reply what=0 arg1=0 arg2=0 data=Bundle[{}] obj=null
UPLOAD reply what=1 arg1=-1000 arg2=0 data=Bundle[{}] obj=null
```

---

## 6. Filesystem / side-effect check

```
$ adb shell find /sdcard -newermt '8 minutes ago' -type f     # (none, except our own logs)
$ adb shell ls -la /sdcard/Recordings/                        # total 0
```

**No recording file, no new media file, no DMS artifact.** `schedule.add` did not produce a
recording row.

---

## 7. Interpretation

1. The exported `scheduleapi` AIDL interface is fully reachable from an installed,
   debuggable third-party APK. The two blockers were (a) needing a real process (solved by
   the APK) and (b) Android 11+ package visibility (solved with `<queries>`).
2. `getMediaType()` and `getOperatorCode()` return empty strings and `postMessage` reports
   empty lists plus `EPG_API_ERROR_GENERIC` — the STB has **no EPG candidate data and no
   operator/DMS provisioning** at present. `schedule.add` fails at
   `ScheduleRecordingManager.addOperation` because it cannot resolve a candidate, not
   because the JSON is malformed (the reply includes a generated `id:"[0,0]"`).
3. No command exposed a file path, recording ID, DMS UUID, or URL on this run. The
   `postMessage` transport itself is proven working; to obtain such values the device must
   first have EPG data (channel scan/`channelupdate`) and a reachable DMS/record target.
4. The `id` field format `[0,0]` is the `ScheduleId` string returned even on error — useful
   for future work: a successful add should return a non-`[0,0]` id which can then be fed to
   `schedule.delete`.

## 8. Remaining work / caveats

- To get a successful `schedule.add`, first populate EPG/candidates (e.g. trigger
  `com.rovicorp.tokyo.recordext.channelupdate` service action) or use a device with a locked
  tuner + provisioned DMS, then read a real `sid`/`nid` from `candidate.list`.
- The `channel` URI of `com.kddi.mcb.c02as_recordext.provider/channel` exposes only a `cid`
  column (Rovi channel IDs), not usable `sid`/`nid`.
- `request.checkdms` / `checkdms.request` need a valid DMS; they return `null` here.
