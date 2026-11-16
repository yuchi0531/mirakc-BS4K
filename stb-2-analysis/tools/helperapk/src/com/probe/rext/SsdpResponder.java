package com.probe.rext;

import android.util.Log;

import java.net.DatagramPacket;
import java.net.InetAddress;
import java.net.MulticastSocket;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.Enumeration;

/**
 * Minimal SSDP responder that runs ON the STB so it can see the STB's own
 * M-SEARCH multicast (which never reaches our host across the subnet boundary).
 * It answers with a LOCATION URL pointing at the host DMS simulator.
 */
public class SsdpResponder extends Thread {
    public static final String TAG = "SSDPR";
    private static final String MCAST = "239.255.255.250";
    private static final int PORT = 1900;
    private static final String UUID = "a1b2c3d4-5e6f-4789-9abc-def012345678";
    private static final String LOCATION = "http://192.168.100.100:18080/desc.xml";
    private static final String SERVER = "Linux/3.14 UPnP/1.0 TestDMS/1.0";

    private volatile boolean running = true;
    private MulticastSocket sock;

    public SsdpResponder() {
        super("ssdp-responder");
        setDaemon(true);
    }

    public void stopResponder() {
        running = false;
        interrupt();
    }

    private String response() {
        return "HTTP/1.1 200 OK\r\n"
                + "CACHE-CONTROL: max-age=1800\r\n"
                + "EXT:\r\n"
                + "LOCATION: " + LOCATION + "\r\n"
                + "OPT: \"http://schemas.upnp.org/upnp/1/0/\"; ns=01\r\n"
                + "SERVER: " + SERVER + "\r\n"
                + "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n"
                + "USN: uuid:" + UUID + "::urn:schemas-upnp-org:device:MediaServer:1\r\n"
                + "\r\n";
    }

    private String buildNotify(String nt) {
        String usn = "uuid:" + UUID + (nt.startsWith("uuid:") ? "" : "::" + nt);
        return "NOTIFY * HTTP/1.1\r\n"
                + "HOST: 239.255.255.250:1900\r\n"
                + "CACHE-CONTROL: max-age=1801\r\n"
                + "LOCATION: " + LOCATION + "\r\n"
                + "NT: " + nt + "\r\n"
                + "NTS: ssdp:alive\r\n"
                + "SERVER: " + SERVER + "\r\n"
                + "USN: " + usn + "\r\n"
                + "CONTENT-LENGTH: 0\r\n"
                + "\r\n";
    }

    /** Periodically multicast ssdp:alive on the STB's own subnet so its Dlna
     *  discovery registers our DMS (the host is on a different subnet). */
    private void startAnnouncer() {
        new Thread(new Runnable() {
            @Override public void run() {
                String[] nts = {
                    "uuid:" + UUID,
                    "upnp:rootdevice",
                    "urn:schemas-upnp-org:device:MediaServer:1",
                    "urn:schemas-upnp-org:service:ContentDirectory:1",
                    "urn:schemas-upnp-org:service:ConnectionManager:1"
                };
                int round = 0;
                while (running) {
                    try {
                        for (String nt : nts) {
                            byte[] b = buildNotify(nt).getBytes(StandardCharsets.UTF_8);
                            sock.send(new DatagramPacket(b, b.length,
                                    InetAddress.getByName(MCAST), PORT));
                        }
                        round++;
                        Log.d(TAG, "announced round " + round + " (" + nts.length + " NOTIFYs)");
                    } catch (Throwable t) {
                        Log.e(TAG, "announce error: " + t);
                    }
                    try { Thread.sleep(4000); } catch (Throwable ignore) {}
                }
            }
        }, "ssdp-announcer").start();
    }

    @Override
    public void run() {
        MulticastSocket sock = null;
        try {
            sock = new MulticastSocket(null);
            sock.setReuseAddress(true);
            sock.bind(new InetSocketAddress(PORT));
            sock.setTimeToLive(2);
            InetAddress group = InetAddress.getByName(MCAST);
            try {
                sock.joinGroup(group);
                Log.d(TAG, "joined " + MCAST);
            } catch (Throwable je) {
                Log.e(TAG, "joinGroup failed: " + je);
            }
            Log.d(TAG, "listening on 0.0.0.0:" + PORT + " LOCATION=" + LOCATION);
            this.sock = sock;
            startAnnouncer();

            byte[] buf = new byte[8192];
            while (running) {
                DatagramPacket pkt = new DatagramPacket(buf, buf.length);
                try {
                    sock.receive(pkt);
                } catch (java.net.SocketTimeoutException ste) {
                    continue;
                } catch (Throwable te) {
                    if (running) Log.e(TAG, "receive error: " + te);
                    break;
                }
                String text = new String(pkt.getData(), pkt.getOffset(), pkt.getLength(),
                        StandardCharsets.UTF_8);
                String first = text.split("\r\n", 1)[0];
                Log.d(TAG, "UDP from " + pkt.getAddress().getHostAddress() + ":"
                        + pkt.getPort() + " " + first);
                if (first.toUpperCase().startsWith("M-SEARCH")) {
                    String low = text.toLowerCase();
                    if (low.contains("mediaserver") || low.contains("ssdp:all")
                            || low.contains("schemas-upnp-org:device") || low.contains("rootdevice")) {
                        byte[] r = response().getBytes(StandardCharsets.UTF_8);
                        try {
                            sock.send(new DatagramPacket(r, r.length, pkt.getAddress(), pkt.getPort()));
                            Log.d(TAG, "-> replied to " + pkt.getAddress().getHostAddress()
                                    + ":" + pkt.getPort());
                        } catch (Throwable se) {
                            Log.e(TAG, "send error: " + se);
                        }
                    } else {
                        Log.d(TAG, "M-SEARCH ST not matched, ignoring");
                    }
                }
            }
        } catch (Throwable t) {
            Log.e(TAG, "fatal: " + t);
        } finally {
            if (sock != null) {
                try { sock.leaveGroup(InetAddress.getByName(MCAST)); } catch (Throwable ignore) {}
                sock.close();
            }
            Log.d(TAG, "responder stopped");
        }
    }
}
