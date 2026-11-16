#!/usr/bin/env python3
"""
Minimal DLNA/UPnP MediaServer simulator (standard library only).

Purpose: software-only TS extraction experiment. The STB's recordext app supports
a DLNA recording destination and uploads recorded TS to a DMS identified by UUID.
This simulator advertises a fake MediaServer over SSDP, serves the UPnP device /
ContentDirectory descriptions over HTTP, logs all SOAP/event traffic, and saves
any large binary HTTP POST body (the TS upload) to uploads/.

Because the STB (192.168.1.119) and this host (192.168.100.100) are on DIFFERENT
subnets, SSDP multicast from the STB will not reach us. To work around that we
also send unicast NOTIFY ssdp:alive datagrams directly to the STB's UDP port
1900 -- many UPnP control points accept a unicast NOTIFY on that port and then
fetch the LOCATION URL over HTTP (which *does* route across the subnets).
"""

import datetime
import os
import re
import socket
import struct
import threading
import time
import uuid

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
HOST_IP = os.environ.get("DLNA_HOST_IP", "192.168.100.100")
HTTP_PORT = int(os.environ.get("DLNA_HTTP_PORT", "18080"))
STB_IP = os.environ.get("DLNA_STB_IP", "192.168.1.119")
SSDP_MCAST_GRP = "239.255.255.250"
SSDP_PORT = 1900
OUR_UUID = os.environ.get("DLNA_UUID", "a1b2c3d4-5e6f-4789-9abc-def012345678")
FRIENDLY_NAME = "STB-2 Test DMS"

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
UPLOAD_DIR = os.path.join(BASE_DIR, "uploads")
LOG_PATH = os.path.join(BASE_DIR, "server.log")

os.makedirs(UPLOAD_DIR, exist_ok=True)

LOCATION = "http://%s:%d/desc.xml" % (HOST_IP, HTTP_PORT)
SERVER_HEADER = "Linux/3.14 UPnP/1.0 TestDMS/1.0"

_log_lock = threading.Lock()


def log(msg):
    """Append a timestamped line to server.log and echo to stdout."""
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    line = "[%s] %s" % (ts, msg)
    with _log_lock:
        try:
            with open(LOG_PATH, "a", encoding="utf-8", errors="replace") as f:
                f.write(line + "\n")
        except Exception:
            pass
        try:
            print(line, flush=True)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# XML documents
# ---------------------------------------------------------------------------
DESC_XML = """<?xml version="1.0" encoding="utf-8"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <URLBase>http://{host}:{port}</URLBase>
  <device>
    <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
    <friendlyName>{name}</friendlyName>
    <manufacturer>Test</manufacturer>
    <manufacturerURL>http://{host}:{port}/</manufacturerURL>
    <modelDescription>Minimal DLNA DMS simulator</modelDescription>
    <modelName>TestDMS</modelName>
    <modelNumber>1.0</modelNumber>
    <modelURL>http://{host}:{port}/</modelURL>
    <serialNumber>00000001</serialNumber>
    <UDN>uuid:{uuid}</UDN>
    <UPC>000000000000</UPC>
    <dlna:X_DLNADOC xmlns:dlna="urn:schemas-dlna-org:device-1-0">DMS-1.50</dlna:X_DLNADOC>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:ContentDirectory</serviceId>
        <SCPDURL>/cd.xml</SCPDURL>
        <controlURL>/cd/control</controlURL>
        <eventSubURL>/cd/events</eventSubURL>
      </service>
      <service>
        <serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>
        <SCPDURL>/cm.xml</SCPDURL>
        <controlURL>/cm/control</controlURL>
        <eventSubURL>/cm/events</eventSubURL>
      </service>
    </serviceList>
  </device>
</root>
""".format(host=HOST_IP, port=HTTP_PORT, name=FRIENDLY_NAME, uuid=OUR_UUID)

CD_XML = """<?xml version="1.0" encoding="utf-8"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <actionList>
    <action>
      <name>Browse</name>
      <argumentList>
        <argument><name>ObjectID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument>
        <argument><name>BrowseFlag</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_BrowseFlag</relatedStateVariable></argument>
        <argument><name>Filter</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Filter</relatedStateVariable></argument>
        <argument><name>StartingIndex</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Index</relatedStateVariable></argument>
        <argument><name>RequestedCount</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
        <argument><name>SortCriteria</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SortCriteria</relatedStateVariable></argument>
        <argument><name>Result</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>
        <argument><name>NumberReturned</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
        <argument><name>TotalMatches</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
        <argument><name>UpdateID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_UpdateID</relatedStateVariable></argument>
      </argumentList>
    </action>
    <action>
      <name>CreateObject</name>
      <argumentList>
        <argument><name>ContainerID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument>
        <argument><name>Elements</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>
        <argument><name>ObjectID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument>
        <argument><name>Result</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>
      </argumentList>
    </action>
  </actionList>
  <serviceStateTable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_ObjectID</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_BrowseFlag</name><dataType>string</dataType><allowedValueList><allowedValue>BrowseMetadata</allowedValue><allowedValue>BrowseDirectChildren</allowedValue></allowedValueList></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_Filter</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_Index</name><dataType>ui4</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_Count</name><dataType>ui4</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_SortCriteria</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_Result</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_UpdateID</name><dataType>ui4</dataType></stateVariable>
    <stateVariable sendEvents="yes"><name>ContentDirectoryUpdateID</name><dataType>ui4</dataType></stateVariable>
  </serviceStateTable>
</scpd>
"""

CM_XML = """<?xml version="1.0" encoding="utf-8"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <actionList>
    <action><name>GetProtocolInfo</name><argumentList>
      <argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>
      <argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>
    </argumentList></action>
  </actionList>
  <serviceStateTable>
    <stateVariable sendEvents="yes"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="yes"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>
  </serviceStateTable>
</scpd>
"""

EMPTY_DIDL = ("&lt;DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
              "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
              "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\"&gt;"
              "&lt;/DIDL-Lite&gt;")

_upload_counter = [0]
_upload_lock = threading.Lock()


def soap_envelope(inner):
    return ('<?xml version="1.0" encoding="utf-8"?>\r\n'
            '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
            '<s:Body>' + inner + '</s:Body></s:Envelope>')


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------
class DLNAHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "TestDMS/1.0"
    sys_version = ""

    def log_message(self, fmt, *args):
        # Redirect BaseHTTPRequestHandler's access log into our own log with src IP.
        log("HTTP %s - %s" % (self.client_address[0], fmt % args))

    def _src(self):
        return self.client_address[0]

    def _read_body(self):
        length = self.headers.get("Content-Length")
        if length is None:
            return b""
        try:
            n = int(length)
        except ValueError:
            return b""
        return self.rfile.read(n)

    def _send(self, status, body, content_type="text/xml; charset=utf-8", extra_headers=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Server", SERVER_HEADER)
        self.send_header("Connection", "close")
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _log_request(self, body):
        log("REQUEST %s %s from %s" % (self.command, self.path, self._src()))
        for k, v in self.headers.items():
            log("  HDR %s: %s" % (k, v))
        if body:
            try:
                text = body.decode("utf-8", errors="replace")
            except Exception:
                text = repr(body[:2000])
            # Log SOAP bodies fully; truncate only enormous binary uploads here
            # (those are saved to disk anyway).
            if len(text) > 16000:
                text = text[:16000] + "\n...[TRUNCATED %d bytes]" % (len(text) - 16000)
            log("  BODY(%d bytes): %s" % (len(body), text))

    def _save_upload(self, body):
        with _upload_lock:
            _upload_counter[0] += 1
            n = _upload_counter[0]
        ext = self.headers.get("Content-Type", "application/octet-stream")
        ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        safe_ct = re.sub(r"[^A-Za-z0-9_.-]", "_", ext)[:40]
        fname = "upload_%03d_%s_%s_%s.bin" % (n, ts, self._src().replace(".", "_"), safe_ct)
        path = os.path.join(UPLOAD_DIR, fname)
        with open(path, "wb") as f:
            f.write(body)
        log("SAVED UPLOAD %s (%d bytes) -> %s" % (self.path, len(body), path))
        return path

    # ---- Standard methods -------------------------------------------------
    def do_GET(self):
        self._log_request(b"")
        path = self.path.split("?", 1)[0]
        if path in ("/desc.xml", "/description.xml", "/"):
            if path == "/":
                body = ("<html><body>Test DMS. desc=<a href='/desc.xml'>/desc.xml</a></body></html>")
                return self._send(200, body, "text/html; charset=utf-8")
            return self._send(200, DESC_XML)
        if path == "/cd.xml":
            return self._send(200, CD_XML)
        if path == "/cm.xml":
            return self._send(200, CM_XML)
        # Any other GET: treat as a request for content; return empty DIDL.
        log("GET unknown path %s -> 404" % path)
        return self._send(404, "Not Found", "text/plain; charset=utf-8")

    def do_POST(self):
        body = self._read_body()
        self._log_request(body)
        path = self.path.split("?", 1)[0]

        # Save large binary bodies (TS uploads) regardless of URL, but still
        # answer SOAP control requests properly.
        soap_action = self.headers.get("SOAPACTION", self.headers.get("Soapaction", ""))
        ctype = self.headers.get("Content-Type", "")
        is_soap = ("xml" in ctype.lower()) or ("ContentDirectory" in soap_action) or \
                  ("ConnectionManager" in soap_action)

        if path == "/cd/control" or is_soap:
            text = body.decode("utf-8", errors="replace")
            action = None
            m = re.search(r"<(\w+:)?(Browse|CreateObject|GetProtocolInfo|GetSystemUpdateID)\b", text)
            if m:
                action = m.group(2)
            log("SOAP action detected: %s (SOAPACTION=%s)" % (action, soap_action))
            if action == "CreateObject":
                with _upload_lock:
                    _upload_counter[0] += 1
                    oid = "obj_%d" % _upload_counter[0]
                inner = ('<u:CreateObjectResponse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1">'
                         '<ObjectID>%s</ObjectID><Result>%s</Result>'
                         '</u:CreateObjectResponse>' % (oid, EMPTY_DIDL))
            elif action == "GetProtocolInfo":
                inner = ('<u:GetProtocolInfoResponse xmlns:u="urn:schemas-upnp-org:service:ConnectionManager:1">'
                         '<Source>http-get:*:video/mp2t:*,http-get:*:video/mpeg:*</Source>'
                         '<Sink></Sink></u:GetProtocolInfoResponse>')
            elif action == "GetSystemUpdateID":
                inner = ('<u:GetSystemUpdateIDResponse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1">'
                         '<Id>0</Id></u:GetSystemUpdateIDResponse>')
            else:  # Browse (default)
                inner = ('<u:BrowseResponse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1">'
                         '<Result>%s</Result><NumberReturned>0</NumberReturned>'
                         '<TotalMatches>0</TotalMatches><UpdateID>0</UpdateID>'
                         '</u:BrowseResponse>' % EMPTY_DIDL)
            return self._send(200, soap_envelope(inner),
                              extra_headers={"EXT": "", "SOAPACTION": soap_action})

        # Non-SOAP POST -> assume TS upload. Save and acknowledge.
        saved = self._save_upload(body)
        return self._send(200, "", "text/plain; charset=utf-8",
                          extra_headers={"X-Saved-As": os.path.basename(saved)})

    def do_PUT(self):
        body = self._read_body()
        self._log_request(body)
        if body:
            self._save_upload(body)
        return self._send(200, "", "text/plain; charset=utf-8")

    def do_SUBSCRIBE(self):
        self._log_request(b"")
        log("SUBSCRIBE accepted for %s" % self.path)
        # Minimal valid event subscription response.
        self.send_response(200)
        self.send_header("SID", "uuid:" + str(uuid.uuid4()))
        self.send_header("TIMEOUT", "Second-1800")
        self.send_header("Content-Length", "0")
        self.send_header("Server", SERVER_HEADER)
        self.send_header("Connection", "close")
        self.end_headers()

    do_UNSUBSCRIBE = do_SUBSCRIBE

    def do_NOTIFY(self):
        body = self._read_body()
        self._log_request(body)
        return self._send(200, "", "text/plain; charset=utf-8")


# ---------------------------------------------------------------------------
# SSDP
# ---------------------------------------------------------------------------
def build_msearch_response():
    return (
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "EXT:\r\n"
        "LOCATION: %s\r\n"
        "OPT: \"http://schemas.upnp.org/upnp/1/0/\"; ns=01\r\n"
        "SERVER: %s\r\n"
        "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n"
        "USN: uuid:%s::urn:schemas-upnp-org:device:MediaServer:1\r\n"
        "\r\n"
    ) % (LOCATION, SERVER_HEADER, OUR_UUID)


def build_notify(st, nts="ssdp:alive"):
    usn = "uuid:%s" % OUR_UUID
    if st.startswith("urn:"):
        usn += "::" + st
    return (
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: %s:%d\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: %s\r\n"
        "NT: %s\r\n"
        "NTS: %s\r\n"
        "SERVER: %s\r\n"
        "USN: %s\r\n"
        "\r\n"
    ) % (SSDP_MCAST_GRP, SSDP_PORT, LOCATION, st, nts, SERVER_HEADER, usn)


def ssdp_responder():
    """Listen on UDP 1900, answer M-SEARCH for MediaServer / ssdp:all."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("", SSDP_PORT))
    except OSError as e:
        log("SSDP bind error: %s" % e)
        return
    try:
        mreq = struct.pack("4s4s", socket.inet_aton(SSDP_MCAST_GRP),
                           socket.inet_aton(HOST_IP))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        log("SSDP joined multicast group %s on %s" % (SSDP_MCAST_GRP, HOST_IP))
    except OSError as e:
        log("SSDP IP_ADD_MEMBERSHIP error: %s" % e)

    log("SSDP responder listening on 0.0.0.0:%d" % SSDP_PORT)
    while True:
        try:
            data, src = sock.recvfrom(65535)
        except OSError as e:
            log("SSDP recv error: %s" % e)
            time.sleep(0.5)
            continue
        text = data.decode("utf-8", errors="replace")
        first = text.split("\r\n", 1)[0]
        log("SSDP UDP from %s:%d: %s" % (src[0], src[1], first))
        # Log the ST header if present.
        m = re.search(r"(?im)^ST:\s*(.+)$", text)
        st = m.group(1).strip() if m else ""
        if first.upper().startswith("M-SEARCH"):
            log("  M-SEARCH ST=%s" % st)
            lower = st.lower()
            if ("mediaserver" in lower) or ("ssdp:all" in lower) or \
               ("schemas-upnp-org:device" in lower):
                resp = build_msearch_response().encode("utf-8")
                try:
                    sock.sendto(resp, src)
                    log("  -> sent M-SEARCH response to %s:%d" % (src[0], src[1]))
                except OSError as e:
                    log("  -> sendto error: %s" % e)
        else:
            log("  (non M-SEARCH SSDP datagram)")


def unicast_notify_sender():
    """Send unicast SSDP NOTIFY ssdp:alive directly to the STB (cross-subnet hack)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    targets = [("%s" % STB_IP, SSDP_PORT)]
    # Also to the local multicast group for completeness (harmless on this subnet).
    sts = [
        "urn:schemas-upnp-org:device:MediaServer:1",
        "urn:schemas-upnp-org:device:MediaServer:2",
        "urn:schemas-upnp-org:service:ContentDirectory:1",
        "ssdp:all",
        "upnp:rootdevice",
        OUR_UUID,
    ]
    first = True
    while True:
        for st in sts:
            pkt = build_notify(st).encode("utf-8")
            for ip, port in targets:
                try:
                    sock.sendto(pkt, (ip, port))
                    if first:
                        log("UNICAST NOTIFY %s -> %s:%d" % (st, ip, port))
                except OSError as e:
                    log("UNICAST NOTIFY error %s:%d: %s" % (ip, port, e))
        first = False
        # Re-announce periodically so a discovery window always catches one.
        time.sleep(20)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    log("=== DLNA simulator starting ===")
    log("HOST_IP=%s HTTP_PORT=%d STB_IP=%s UUID=%s" %
        (HOST_IP, HTTP_PORT, STB_IP, OUR_UUID))
    log("LOCATION=%s" % LOCATION)
    log("LOG=%s UPLOAD_DIR=%s" % (LOG_PATH, UPLOAD_DIR))

    threading.Thread(target=ssdp_responder, daemon=True, name="ssdp-responder").start()
    threading.Thread(target=unicast_notify_sender, daemon=True, name="unicast-notify").start()

    httpd = ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), DLNAHandler)
    httpd.daemon_threads = True
    log("HTTP server listening on 0.0.0.0:%d" % HTTP_PORT)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log("KeyboardInterrupt, shutting down")
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
