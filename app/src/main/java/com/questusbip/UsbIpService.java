package com.questusbip;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.util.Log;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Keeps the USB/IP server alive in the background, asks for USB permission for
 * every attached device and hands granted devices to the native server.
 */
public class UsbIpService extends Service {
    static final String TAG = "QuestUsbIp";
    static final String ACTION_PERMISSION = "com.questusbip.USB_PERMISSION";
    static final String ACTION_RESCAN = "com.questusbip.RESCAN";
    static final int PORT = 3240;

    /** Last Java-side event, shown in the activity. */
    static volatile String lastEvent = "";

    private UsbManager usb;
    private Handler handler;
    private final Map<String, UsbDeviceConnection> connections = new HashMap<>();
    private final Map<String, Integer> slots = new HashMap<>();
    private final Set<String> asked = new HashSet<>();
    private PowerManager.WakeLock wakeLock;
    private WifiManager.WifiLock wifiLock;

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            UsbDevice dev = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            String action = intent.getAction();
            if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                if (dev != null) {
                    asked.remove(dev.getDeviceName());
                    unshare(dev.getDeviceName());
                }
            } else if (ACTION_PERMISSION.equals(action)) {
                boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                event("Permiso USB " + (granted ? "concedido" : "DENEGADO") + (dev != null ? " para " + describe(dev) : ""));
                if (granted && dev != null) share(dev);
            } else {
                scan();
            }
        }
    };

    private final Runnable scanLoop = new Runnable() {
        @Override
        public void run() {
            scan();
            handler.postDelayed(this, 3000);
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        usb = (UsbManager) getSystemService(USB_SERVICE);
        handler = new Handler(Looper.getMainLooper());
        startForeground(1, buildNotification());

        int r = Native.startServer(PORT);
        if (r != 0) event("No se pudo abrir el puerto " + PORT + " (error " + r + ")");

        IntentFilter f = new IntentFilter();
        f.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        f.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        f.addAction(ACTION_PERMISSION);
        registerReceiver(receiver, f);

        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "questusbip:server");
        wakeLock.acquire();
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "questusbip:wifi");
        wifiLock.acquire();

        handler.post(scanLoop);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_RESCAN.equals(intent.getAction())) {
            asked.clear();
            scan();
        }
        return START_STICKY;
    }

    private void scan() {
        for (UsbDevice dev : usb.getDeviceList().values()) {
            String name = dev.getDeviceName();
            if (slots.containsKey(name)) continue;
            if (usb.hasPermission(dev)) {
                share(dev);
            } else if (asked.add(name)) {
                event("Pidiendo permiso para " + describe(dev));
                Intent i = new Intent(ACTION_PERMISSION).setPackage(getPackageName());
                int piFlags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
                usb.requestPermission(dev, PendingIntent.getBroadcast(this, 0, i, piFlags));
            }
        }
    }

    private void share(UsbDevice dev) {
        String name = dev.getDeviceName();
        if (slots.containsKey(name)) return;
        UsbDeviceConnection conn = usb.openDevice(dev);
        if (conn == null) {
            event("No se pudo abrir " + describe(dev));
            return;
        }
        int slot = Native.addDevice(conn.getFileDescriptor(), conn.getRawDescriptors(), name);
        if (slot < 0) {
            conn.close();
            event("Demasiados dispositivos");
            return;
        }
        connections.put(name, conn);
        slots.put(name, slot);
        event("Compartiendo " + describe(dev));
    }

    private void unshare(String name) {
        Integer slot = slots.remove(name);
        if (slot != null) Native.removeDevice(slot);
        UsbDeviceConnection conn = connections.remove(name);
        if (conn != null) conn.close();
    }

    private static String describe(UsbDevice dev) {
        String product = dev.getProductName();
        return String.format("%04x:%04x%s", dev.getVendorId(), dev.getProductId(),
                product != null ? " (" + product + ")" : "");
    }

    private static void event(String msg) {
        Log.i(TAG, msg);
        lastEvent = msg;
    }

    private Notification buildNotification() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel("usbip", "Servidor USB/IP", NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, "usbip")
                .setSmallIcon(android.R.drawable.stat_sys_upload)
                .setContentTitle("Quest USB/IP")
                .setContentText("Compartiendo dispositivos USB en el puerto " + PORT)
                .setContentIntent(open)
                .setOngoing(true)
                .build();
    }

    @Override
    public void onDestroy() {
        handler.removeCallbacks(scanLoop);
        unregisterReceiver(receiver);
        for (String name : new HashSet<>(slots.keySet())) unshare(name);
        if (wakeLock.isHeld()) wakeLock.release();
        if (wifiLock.isHeld()) wifiLock.release();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
