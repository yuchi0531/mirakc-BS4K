# Phase 12 — Round 20: Software-only TS extraction via DLNA (minimal DMS simulator)

**Date:** 2026-11-17
**Device:** `adb -s 192.168.1.119:5555` (online, STB-2 / C02AS5 / ST4302)
**Host:** `192.168.100.100/24` (`ens18`), default gateway `192.168.100.3`
**Constraints obeyed:** no reboot, no device system/vendor/block writes, no flashing,
no exploits, no crashes. Host-only writes under `tools/dlna/` and this document;
helper-APK source under `tools/helperapk/`. A local Python server was run on the host.

---

## Result (TL;DR)

**No TS upload was achieved, and no autonomous HTTP or SSDP request from the STB ever
reached our DMS.** The decisive blocker is the network topology:

- The STB is on **192.168.1.0/24**, the host is on **192.168.100.0/24**.
- ICMP and **TCP** cross the subnet boundary through the gateway (verified both ways).
- **UDP does not cross in either direction:**
  - the STB's SSDP multicast (239.255.255.250:1900) never reaches the host, and
  - host → STB unicast UDP to port 1900 is silently dropped (verified with a distinctive
    marker datagram that a local STB-side listener bound to `0.0.0.0:1900` never saw).
- A local SSDP responder + NOTIFY announcer was therefore built into the helper APK and
  run **on the STB itself**. It proved the STB's SSDP stack works (it received NOTIFYs
  from a real DMS at 192.168.1.196 and answered an M-SEARCH from it), but the STB never
  fetched our `desc.xml` from the host. Most likely the STB ignores NOTIFYs sourced from
  its own IP (`DLNA_IS_OWN` logic), which an unprivileged app cannot spoof around.
- Despite this, the DLNA **recording reservation itself was created successfully**:
  `schedule.add` with `template[0]="DLNA"` and our UUID at indices 6/7 returned
  `{"result":true,"response":{"id":"[0,1]"}}`, and `schedule.list` shows the row with
  `tsid:16625`. But `request.checkdms` / `StbcoreUploader` still report
  `InvalidDms uuid=` because our DMS is not in the STB's discovered-DMS list.

---

## A. Minimal DLNA/UPnP MediaServer simulator

Created `tools/dlna/dlnaserver.py` — **Python 3 standard library only** (`socket`,
`http.server`, `threading`, `uuid`, `re`, `struct`). No external dependencies.

Fixed identity:

```
UUID      a1b2c3d4-5e6f-4789-9abc-def012345678
LOCATION  http://192.168.100.100:18080/desc.xml
```

Components:

1. **SSDP responder** — UDP socket on `0.0.0.0:1900`, joins `239.255.255.250` on
   `192.168.100.100`. Answers `M-SEARCH` for `MediaServer:1`, `MediaServer:2`,
   `ssdp:all` and device queries with an HTTP 200 OK containing:
   `CACHE-CONTROL: max-age=1800`, `EXT:`, `LOCATION:`, `SERVER:`,
   `ST: urn:schemas-upnp-org:device:MediaServer:1`,
   `USN: uuid:<uuid>::urn:schemas-upnp-org:device:MediaServer:1`.
   Also sends periodic **unicast NOTIFY ssdp:alive** to the STB (192.168.1.119:1900).
2. **HTTP server** (`ThreadingHTTPServer` on `0.0.0.0:18080`):
   - `GET /desc.xml` — UPnP 1.0 device description, `MediaServer:1`, friendly name,
     `UDN=uuid:<uuid>`, `ContentDirectory:1` (SCPDURL/controlURL/eventSubURL) and
     `ConnectionManager:1`, plus `dlna:X_DLNADOC=DMS-1.50`.
   - `GET /cd.xml` — ContentDirectory SCPD declaring `Browse` and `CreateObject`
     (plus the required state variables). `GET /cm.xml` — ConnectionManager SCPD.
   - `POST /cd/control` — logs the **full SOAP body** and returns valid SOAP responses:
     `BrowseResponse` (empty DIDL-Lite, 0 results), `CreateObjectResponse` (object ID),
     `GetProtocolInfoResponse`, `GetSystemUpdateIDResponse`.
   - `SUBSCRIBE`/`UNSUBSCRIBE /cd/events` — accepts and returns SID/TIMEOUT.
   - **Any other POST/PUT** — the body is treated as a TS upload: saved verbatim to
     `tools/dlna/uploads/upload_NNN_<ts>_<srcip>_<ctype>.bin` and acknowledged (200).
     No path restriction; every request (all headers + body) is logged.
3. **Logging** — every packet/request, with millisecond timestamp and source IP, is
   appended to `tools/dlna/server.log` and echoed to stdout.

### Local verification (all passed)

```
GET /desc.xml                 -> 200, 1531 bytes
GET /cd.xml                   -> 200, 3525 bytes
POST /cd/control  (Browse)    -> 200, valid BrowseResponse (empty DIDL-Lite)
POST /upload  (16-byte blob)  -> 200, saved to uploads/  [test file later removed]
M-SEARCH (self-test)          -> 200 OK with correct LOCATION/USN from 192.168.100.100:1900
```

Evidence: `tools/dlna/server.log`, `tools/dlna/server.stdout`.

---

## B. Discovery triggering

Network reality, measured:

```
$ ip route
default via 192.168.100.3 dev ens18
192.168.100.0/24 dev ens18 proto kernel scope link src 192.168.100.100

$ ping -c 2 192.168.1.119     -> 0% loss, TTL 62 (2 hops: gateway forwards)
$ adb shell nc ... | host:18080 -> HTTP 200 (logged from 192.168.1.119)  [TCP works]
$ host -> STB UDP :1900 marker -> NOT observed by STB-side 0.0.0.0:1900 listener [UDP blocked]
```

Triggers executed (all `Starting service` confirmed):

```
am startservice -a com.rovicorp.tokyo.recordext.channelupdate --ez force_update true
am startservice -a com.rovicorp.tokyo.recordext.scheduleapi      # StbDmsManager path
am startservice -a com.rovicorp.tokyo.recordext.schedulereload
am startservice -a com.rovicorp.tokyo.recordext.kabutoaccess
```

Helper APK `cmd.txt` batches (via `/sdcard/Android/data/com.probe.rext/files/cmd.txt`)
called `request.checkdms` / `checkdms.request` / `storage.check` with our UUID. This
produced the first useful trace confirming discovery is invoked:

```
D STB-API_TRACE_START: class:Dlna, instance:..., method:findServer
D STB-API_TRACE_END:   class:Dlna, instance:..., method:findServer
E StbcoreUploader: buildUploadInfo: InvalidDms uuid= mode=0 recordIds=[]
```

Dex analysis of `apk/recordext/classes.dex` (dexdump) confirmed:

- `StbDmsManager.awake()` calls `Dlna.findServer(true)` — the DMS-list refresh point.
- `JSDialogueApi.requestCheckDms(JSONObject)` reads keys `nid`, `sid`, `startdate`,
  `starttime`, `duration`, **`dmsid`** (our JSON schema was correct) and calls
  `StbDmsManager.requestCheckDms(TvChannel, long, int, String)`.
- `Dlna.findServer(Z)Ljava/util/ArrayList` is `public`; its results are matched against
  `dmsIdList` by `ServerInfo.getDmsUuid()`.

### STB-side SSDP observation (helper APK extension)

Added `tools/helperapk/src/com/probe/rext/SsdpResponder.java` to the existing helper:
a `MulticastSocket(1900)` with `SO_REUSEADDR`, joined to `239.255.255.250`, plus a
periodic **multicast NOTIFY announcer** advertising our UUID and host LOCATION.
Manifest gained `INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`,
`CHANGE_WIFI_MULTICAST_STATE`, `WAKE_LOCK`; `MainActivity` gained a `hold_ms` extra so
the responder stays alive during the experiment, and `build.sh` now compiles all Java
sources. Rebuilt and reinstalled `rext-helper.apk`.

What the STB-side listener actually saw:

```
SSDPR: joined 239.255.255.250 ; listening on 0.0.0.0:1900 LOCATION=http://192.168.100.100:18080/desc.xml
SSDPR: announced round 1..6 (5 NOTIFYs each)
SSDPR: UDP from 192.168.1.196:44984 M-SEARCH * HTTP/1.1  (real DMS on the LAN)
SSDPR: -> replied to 192.168.1.196:44984
SSDPR: UDP from 192.168.1.196:... NOTIFY * HTTP/1.1  (uuid 7e0ee826-1cb4-11e2-8dfc-001CFC764B1C,
       Location http://192.168.1.196:57777/XD/..., Server UPnP/1.0 XA401/1.0)
SSDPR: UDP from 192.168.1.119:40438 M-SEARCH * HTTP/1.1  (ST: urn:...:WANIPConnection:1 — router, not MS)
SSDPR: -> our responder ignored it (ST not MediaServer)
```

This proves (a) the STB's SSDP multicast is receivable by a local app, (b) a **real DMS
exists on 192.168.1.196**, and (c) the STB's own M-SEARCH seen locally was for a router,
not a MediaServer. Our local multicast NOTIFYs were received by our own socket
(60 looped-back copies) but **the STB never fetched `http://192.168.100.100:18080/desc.xml`**.

---

## C. Upload attempt

The helper's `cmd.txt` was used to exercise the same `postMessage` transport proven in
Phase 8. Exact results:

| Request | Reply |
|---|---|
| `request.checkdms` (dmsid = our UUID, nid 4 / sid 101 / startdate 20261118 / starttime 210000 / duration 3000) | `{"result":true}` |
| `schedule.add` … `template:["DLNA",…,UUID,UUID]`, startdate 20261118, starttime 210000 | `{"result":true,"response":{"id":"[0,0]","errCode":10022,"errMsg":"STBCORE_ERROR_OK_DMS_UNAVAILABLE"}}` |
| `schedule.add` … same but `startdate:261118, starttime:2100` | **`{"result":true,"response":{"id":"[0,1]"}}`** |
| `schedule.add` … nid 1 / sid 1 | `{"result":false,"response":{"id":"[0,2]","errCode":10003,"errMsg":"EPG_API_ERROR_NO_CHANNEL"}}` |
| `schedule.list` | contains the `DLNAPROBE` row: `nid 4, tsid 16625, sid 101, startdate 20261118, starttime 210000, duration 3000, template:["DLNA","ccid",0,null,null,null,"<uuid>","<uuid>",null], id "[0,0]"` |
| `storage.check` (`storageid:"DLNA:<uuid>"` / `"DLNA"` / uuid) | `{"result":false,"response":{"errCode":10013,"errMsg":"EPG_API_ERROR_GENERIC"}}` |

A DLNA recording reservation **was created** (`id [0,1]`). However:

- the reservation is for 2026-11-18 21:00 (tomorrow relative to the test date), so no
  recording/TS exists to upload now;
- `request.checkdms` still logs `InvalidDms uuid=` (empty) — the STB has not registered
  our DMS, so even an existing recording would not be addressed to our LOCATION.

No HTTP request from any STB process to `/cd/control` (CreateObject / upload) or to any
other path arrived at the DMS. `tools/dlna/uploads/` remained empty.

---

## D. Answers to the task questions

1. **Did the STB send SSDP M-SEARCH or any HTTP request to our DMS?**
   - To our **host DMS**: **No.** The host SSDP socket recorded zero datagrams from
     192.168.1.119; the only HTTP request from 192.168.1.119 was our own manual `nc`
     connectivity test (`GET /desc.xml`), not autonomous discovery. The reason is the
     cross-subnet UDP block (documented above).
   - On the **STB's own subnet**: yes — a local helper saw the STB emit an M-SEARCH for
     `WANIPConnection:1` and the real DMS at 192.168.1.196 emit NOTIFYs / an M-SEARCH.
     None of this was for, or reached, our DMS.
2. **Did any upload arrive?**
   - **No.** `tools/dlna/uploads/` is empty. No CreateObject SOAP call, no large binary
     POST, no event subscription from the STB was received.
3. **Logs saved:** `tools/dlna/server.log`, `tools/dlna/server.stdout`,
   `tools/dlna/logcat20.txt`, plus all commands/results inlined in this document.

---

## Files changed

- `tools/dlna/dlnaserver.py` (new) — minimal standard-library DLNA DMS simulator.
- `tools/dlna/server.log`, `tools/dlna/server.stdout`, `tools/dlna/logcat20.txt` (new evidence).
- `tools/dlna/uploads/` (new, empty apart from `.gitkeep`-style directory).
- `tools/helperapk/AndroidManifest.xml` (modified) — INTERNET / multicast / wake-lock permissions.
- `tools/helperapk/build.sh` (modified) — compile all Java sources.
- `tools/helperapk/src/com/probe/rext/MainActivity.java` (modified) — start SSDP responder,
  `hold_ms` keep-alive, stop responder on finish.
- `tools/helperapk/src/com/probe/rext/SsdpResponder.java` (new) — STB-local SSDP
  responder + NOTIFY announcer.
- `tools/helperapk/rext-helper.apk` (rebuilt).
- `docs/PHASE12_ROUND20.md` (this file).

---

## Remaining work / caveats / recommendations

1. **Subnet is the fundamental blocker.** SSDP is link-local; a DMS on 192.168.100.0/24
   cannot be discovered by a control point on 192.168.1.0/24 without a multicast relay
   (IGMP proxy / `smcroute`) on the gateway, which is out of scope and would touch
   network infrastructure. Host-only routes do not help because the gateway already
   routes unicast and the issue is multicast + inbound-UDP filtering.
2. **Even with discovery, content is the second blocker.** The DLNA upload path needs an
   existing recorded TS on the STB plus a registered DMS. Both were absent: no EPG
   candidate data (`candidate.list=[]`), no recording file, and our DMS was not in the
   discovered list. Triggering a real broadcast recording (locked tuner + provisioned
   EPG) would be required to produce a TS to upload.
3. **If continuing on-subnet:** place the DMS host on 192.168.1.0/24 (e.g. USB/network
   adapter or a secondary address reachable at L2) so SSDP multicast and UDP work
   natively; then re-run `request.checkdms` + `schedule.add` and watch `/cd/control` for
   `CreateObject` / upload POSTs.
4. The real DMS at `192.168.1.196` (UUID `7e0ee826-…`) is the STB's existing DLNA target;
   it was observed but deliberately not probed or interfered with, per the constraints.
5. The host DMS server was stopped after the experiment. It can be restarted with
   `cd tools/dlna && nohup python3 dlnaserver.py > server.stdout 2>&1 &`.
