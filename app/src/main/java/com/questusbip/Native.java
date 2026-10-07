package com.questusbip;

/** JNI bridge to the USB/IP server in usbip.c. */
public final class Native {
    static { System.loadLibrary("questusbip"); }

    private Native() {}

    /** Starts listening on the given TCP port; returns 0 or -errno. */
    public static native int startServer(int port);

    /** Shares an opened device; returns its slot or -1 if the table is full. */
    public static native int addDevice(int fd, byte[] rawDescriptors, String deviceName);

    public static native void removeDevice(int slot);

    /** Human-readable server state plus recent log lines. */
    public static native String status();
}
