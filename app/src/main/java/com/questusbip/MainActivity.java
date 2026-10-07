package com.questusbip;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** Status screen: starts the service, requests camera/mic permission, shows server state. */
public class MainActivity extends Activity {
    private static final String USB_CAMERA = "horizonos.permission.USB_CAMERA";
    private TextView statusView;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private final Runnable refresh = new Runnable() {
        @Override
        public void run() {
            statusView.setText("IP de las gafas: " + wifiIp() + "\n\n" + Native.status()
                    + "\nÚltimo evento: " + UsbIpService.lastEvent);
            handler.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        int pad = 32;
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        Button retry = new Button(this);
        retry.setText("Volver a pedir permisos USB");
        retry.setOnClickListener(v -> startService(new Intent(this, UsbIpService.class).setAction(UsbIpService.ACTION_RESCAN)));
        root.addView(retry);

        statusView = new TextView(this);
        statusView.setTypeface(Typeface.MONOSPACE);
        statusView.setTextSize(14);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(statusView);
        root.addView(scroll);
        setContentView(root);

        // UVC cameras (face tracker) and USB audio are only granted to apps holding these.
        String[] needed = {Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO, USB_CAMERA};
        for (String p : needed) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(needed, 1);
                break;
            }
        }
        startForegroundService(new Intent(this, UsbIpService.class));
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        startService(new Intent(this, UsbIpService.class).setAction(UsbIpService.ACTION_RESCAN));
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        startService(new Intent(this, UsbIpService.class).setAction(UsbIpService.ACTION_RESCAN));
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.post(refresh);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refresh);
        super.onPause();
    }

    private String wifiIp() {
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
        int ip = wm.getConnectionInfo().getIpAddress();
        return String.format("%d.%d.%d.%d", ip & 0xff, ip >> 8 & 0xff, ip >> 16 & 0xff, ip >> 24 & 0xff);
    }
}
