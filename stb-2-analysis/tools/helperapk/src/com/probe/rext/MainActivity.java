package com.probe.rext;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.Message;
import android.os.Messenger;
import android.util.Log;
import android.widget.TextView;

import java.io.File;
import java.io.FileWriter;

/**
 * Minimal helper APK that binds to the exported, no-permission AIDL service
 * com.kddi.pd2.c02as5_recordext / com.rovicorp.tokyo.recordext.ServiceProcess
 * and exercises IRecordExtInterface via raw Parcel transact().
 */
public class MainActivity extends Activity {

    static final String TAG = "REXT";
    static final String DESC = "com.rovicorp.tokyo.recordext.IRecordExtInterface";
    static final String ACTION = "com.rovicorp.tokyo.recordext.scheduleapi";
    static final String PKG = "com.kddi.pd2.c02as5_recordext";
    static final String CLS = "com.rovicorp.tokyo.recordext.ServiceProcess";

    static final int CODE_REGISTER_LISTENER = 1;
    static final int CODE_UNREGISTER_LISTENER = 2;
    static final int CODE_GET_MEDIA_TYPE = 3;
    static final int CODE_GET_OPERATOR_CODE = 4;
    static final int CODE_POST_MESSAGE = 5;

    File outFile;
    File extFile;
    boolean finished = false;
    SsdpResponder ssdpResponder;
    long holdMs = 75000;

    final ServiceConnection conn = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            log("onServiceConnected name=" + name + " binder=" + service);
            final IBinder s = service;
            new Thread(new Runnable() {
                @Override public void run() { doCalls(s); }
            }, "rext-worker").start();
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            log("onServiceDisconnected name=" + name);
        }
    };

    // ---- Optional neosystem2 UploadService Messenger probe ----
    final Handler uploadReplyHandler = new Handler(Looper.getMainLooper()) {
        @Override public void handleMessage(Message msg) {
            log("UPLOAD reply what=" + msg.what + " arg1=" + msg.arg1 + " arg2=" + msg.arg2
                    + " data=" + (msg.getData() == null ? "null" : msg.getData().toString())
                    + " obj=" + msg.obj);
        }
    };

    final ServiceConnection uploadConn = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            log("UPLOAD connected " + name + " binder=" + service);
            try {
                Messenger m = new Messenger(service);
                Message msg = Message.obtain(null, 0);
                msg.replyTo = new Messenger(uploadReplyHandler);
                m.send(msg);
                log("UPLOAD sent what=0");
                Message msg1 = Message.obtain(null, 1);
                msg1.replyTo = new Messenger(uploadReplyHandler);
                m.send(msg1);
                log("UPLOAD sent what=1");
            } catch (Throwable t) {
                log("UPLOAD send threw=" + t);
            }
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            log("UPLOAD disconnected " + name);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView tv = new TextView(this);
        tv.setText("REXT probe running - see logcat tag REXT");
        setContentView(tv);

        // Optional: how long to keep the process (and the SSDP responder) alive.
        if (getIntent() != null) {
            String h = getIntent().getStringExtra("hold_ms");
            if (h != null && !h.trim().isEmpty()) {
                try { holdMs = Long.parseLong(h.trim()); } catch (Throwable ignore) {}
            }
        }
        log("holdMs=" + holdMs);

        // Start a local SSDP responder so the STB's own M-SEARCH (multicast on its
        // subnet) is answered with our host DMS LOCATION, enabling discovery.
        try {
            ssdpResponder = new SsdpResponder();
            ssdpResponder.start();
            log("SSDP responder thread started");
        } catch (Throwable t) {
            log("SSDP responder start threw=" + t);
        }

        File files = getFilesDir();
        outFile = new File(files, "out.txt");
        log("=== REXT PROBE START ===");
        log("filesDir=" + files.getAbsolutePath());
        try {
            File ext = getExternalFilesDir(null);
            if (ext != null) {
                extFile = new File(ext, "rext_out.txt");
                log("extDir=" + ext.getAbsolutePath());
            } else {
                log("extDir=NULL");
            }
        } catch (Throwable t) {
            log("extDir error=" + t);
        }

        Intent intent = new Intent(ACTION);
        intent.setPackage(PKG);
        try {
            java.util.List<android.content.pm.ResolveInfo> ris =
                    getPackageManager().queryIntentServices(intent, 0);
            log("queryIntentServices count=" + (ris == null ? -1 : ris.size()));
            if (ris != null) {
                for (android.content.pm.ResolveInfo ri : ris) {
                    log("  resolves to " + ri.serviceInfo);
                }
            }
        } catch (Throwable t) {
            log("queryIntentServices threw=" + t);
        }
        log("bindService intent=" + intent);
        boolean bound = false;
        try {
            bound = bindService(intent, conn, Context.BIND_AUTO_CREATE);
        } catch (Throwable t) {
            log("bindService threw=" + t);
        }
        log("bindService returned=" + bound);

        // Try neosystem2 UploadService (Messenger) in parallel.
        Intent up = new Intent("UploadService");
        up.setPackage("jp.co.sei.streamcruiser.app.neosystem2");
        try {
            log("UPLOAD bindService " + up);
            boolean ub = bindService(up, uploadConn, Context.BIND_AUTO_CREATE);
            log("UPLOAD bindService returned=" + ub);
        } catch (Throwable t) {
            log("UPLOAD bindService threw=" + t);
        }

        if (!bound) {
            log("BIND FAILED (returned false)");
            finishAfterDelay();
        }

        // Watchdog in case onServiceConnected never arrives.
        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override public void run() { finishAfterDelay(); }
        }, holdMs);
    }

    void doCalls(IBinder service) {
        // Optional: exact JSON commands supplied via /sdcard/Android/data/com.probe.rext/files/cmd.txt
        // (one JSON request per line) — avoids adb shell quote mangling.
        if (extFile != null) {
            File cmdFile = new File(extFile.getParentFile(), "cmd.txt");
            if (cmdFile.exists()) {
                try {
                    java.io.BufferedReader br = new java.io.BufferedReader(new java.io.FileReader(cmdFile));
                    String line;
                    while ((line = br.readLine()) != null) {
                        line = line.trim();
                        if (!line.isEmpty() && !line.startsWith("#")) pmsg(service, line);
                    }
                    br.close();
                } catch (Throwable t) {
                    log("cmd.txt read error=" + t);
                }
                log("cmd.txt batch DONE (keeping SSDP responder alive for holdMs)");
                // Do NOT finish here: the local SSDP responder must stay alive so the
                // STB can discover our DMS. The watchdog above will finish after holdMs.
                return;
            }
        }

        log("--- getMediaType (code 3) ---");
        log("getMediaType = " + callString(service, CODE_GET_MEDIA_TYPE, null));

        log("--- getOperatorCode (code 4) ---");
        log("getOperatorCode = " + callString(service, CODE_GET_OPERATOR_CODE, null));

        pmsg(service, "{\"cmd\":\"schedule.list\"}");
        pmsg(service, "{\"cmd\":\"candidate.list\"}");
        pmsg(service, "{\"cmd\":\"storage.check\",\"request\":{\"storageid\":\"0\"}}");
        pmsg(service, "{\"cmd\":\"getOperatorCode\"}");
        pmsg(service, "{\"cmd\":\"request.checkdms\"}");
        pmsg(service, "{\"cmd\":\"schedule.suspendevent\"}");

        // schedule.add: sids supplied via --es sids "1,2,3" (default 1)
        String sidsStr = getIntent() != null ? getIntent().getStringExtra("sids") : null;
        if (sidsStr == null || sidsStr.trim().isEmpty()) sidsStr = "1";
        String addOverride = getIntent() != null ? getIntent().getStringExtra("addjson") : null;
        if (addOverride != null && !addOverride.trim().isEmpty()) {
            pmsg(service, addOverride);
        } else {
            String[] sids = sidsStr.split(",");
            for (String sidRaw : sids) {
                String sid = sidRaw.trim();
                if (sid.isEmpty()) continue;
                // TimerScheduleItem requires "template" (JSONArray); element 0 "VIEW" => "V".
                String json = "{\"cmd\":\"schedule.add\",\"request\":{"
                        + "\"type\":1,\"nid\":1,\"tsid\":1,\"sid\":" + sid
                        + ",\"eid\":0,\"startdate\":261118,\"starttime\":2100"
                        + ",\"duration\":5,\"title\":\"PROBE\",\"rating\":0"
                        + ",\"template\":[\"VIEW\"]}}";
                pmsg(service, json);
            }
        }

        log("--- schedule.list AFTER add ---");
        pmsg(service, "{\"cmd\":\"schedule.list\"}");

        log("=== DONE ===");
        runOnUiThread(new Runnable() {
            @Override public void run() { finishAfterDelay(); }
        });
    }

    void pmsg(IBinder service, String json) {
        log("postMessage REQ " + json);
        String r = callString(service, CODE_POST_MESSAGE, json);
        log("postMessage RES " + r);
    }

    String callString(IBinder service, int code, String arg) {
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESC);
            if (arg != null) data.writeString(arg);
            boolean ok = service.transact(code, data, reply, 0);
            reply.readException();
            String result = reply.readString();
            log("transact(" + code + ") ok=" + ok + " result=" + result);
            return result;
        } catch (Throwable t) {
            log("transact(" + code + ") EXCEPTION=" + t);
            return "EXCEPTION:" + t;
        } finally {
            reply.recycle();
            data.recycle();
        }
    }

    void finishAfterDelay() {
        if (finished) return;
        finished = true;
        if (ssdpResponder != null) {
            try { ssdpResponder.stopResponder(); } catch (Throwable t) { log("ssdp stop threw=" + t); }
        }
        try { unbindService(conn); } catch (Throwable t) { log("unbind threw=" + t); }
        try { unbindService(uploadConn); } catch (Throwable t) { log("UPLOAD unbind threw=" + t); }
        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override public void run() { finish(); }
        }, 300);
    }

    void log(String s) {
        Log.d(TAG, s);
        append(outFile, s);
        if (extFile != null) append(extFile, s);
    }

    void append(File f, String s) {
        if (f == null) return;
        try {
            FileWriter fw = new FileWriter(f, true);
            fw.write(s);
            fw.write('\n');
            fw.close();
        } catch (Throwable t) {
            // best effort
        }
    }
}
