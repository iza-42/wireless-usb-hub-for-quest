package com.questusbip;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/**
 * Invisible target for USB_DEVICE_ATTACHED: starts the server in the background
 * without opening a panel in the headset.
 */
public class UsbAttachActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        startForegroundService(new Intent(this, UsbIpService.class).setAction(UsbIpService.ACTION_RESCAN));
        finish();
    }
}
