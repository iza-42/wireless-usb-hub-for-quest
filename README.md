# Wireless USB hub for Quest

**[Leer en español](README.es.md)**

Turn a Meta Quest headset into a wireless USB hub: plug a USB device into the headset and use it on your PC over Wi-Fi, as if it were plugged into the computer.

It was built to run an **HTC VIVE Facial Tracker** from a Quest 2 for wireless face tracking (SRanipal / VRCFaceTracking), but it forwards ordinary USB devices too: flash drives, cameras, controllers, and so on.

- **No root**, no paid license, no device limit.
- Uses the standard **USB/IP** protocol, so the PC side is the free, signed [usbip-win2](https://github.com/vadimgrn/usbip-win2) client.
- Supports **control, bulk, interrupt and isochronous** transfers. Isochronous support is the part the existing free Android USB/IP server lacks, and it is what webcams and audio devices use.

```
 USB device ──► Quest (Quest USB/IP app) ══ Wi-Fi / USB/IP :3240 ══► PC (usbip-win2) ──► Windows sees a local USB device
```

## Status

Tested on a Quest 2 running Horizon OS (Android 14 base) with:

| Device | Result |
|---|---|
| Kingston DataTraveler 3.0 flash drive | Works: mounts as a normal drive |
| HTC VIVE Facial Tracker (`0bb4:0321`) | Works: SRanipal detects it, VRCFaceTracking shows the camera, ~57 fps |

Other Quest models should work but are untested. Please open an issue with your results.

## Requirements

- A Meta Quest with **developer mode** enabled (Meta Horizon phone app → Devices → Developer mode).
- The PC and the Quest on the same network. The Windows Mobile Hotspot works well.
- On the PC: [usbip-win2](https://github.com/vadimgrn/usbip-win2/releases) (tested with 0.9.8.1).
- For the face tracker: the VIVE **SRanipal** runtime, plus VRCFaceTracking or whatever app you use.
- A USB-C OTG adapter or cable if your device doesn't have a USB-C plug.

## Install

1. Download `quest-usbip.apk` from [Releases](../../releases). You can also build it yourself, see [Building](#building).
2. Sideload it with adb (or SideQuest):
   ```
   adb install -g quest-usbip.apk
   ```
   `-g` grants the camera permissions up front. You can also accept them when the app first opens.
3. Install **usbip-win2** on the PC.

## Usage

1. On the Quest, open **Quest USB/IP** (Library → Unknown Sources). The window shows the headset's IP address and the shared devices.
2. Plug the USB device into the Quest and accept the USB permission prompt.
3. On the PC, list the devices and attach the one you want:
   ```
   usbip list -r <quest-ip>
   usbip attach -r <quest-ip> -b <busid>
   ```
   You can also use the `wusbip` GUI that ships with usbip-win2.
4. When you're done: `usbip detach -p <port>` (see `usbip port`).

### VIVE Facial Tracker

[`scripts/connect-face-tracker.ps1`](scripts/connect-face-tracker.ps1) finds the tracker, attaches it and starts SRanipal:

```
powershell -ExecutionPolicy Bypass -File scripts\connect-face-tracker.ps1 -Quest 192.168.137.16
```

Then start VRCFaceTracking as usual.

Notes:
- The tracker **sends no video until SRanipal turns it on** with vendor commands. A plain webcam app like ffmpeg will open it and receive 0 bytes. This is expected.
- It streams **uncompressed 400×400 YUY2 at 60 fps, about 18 MB/s (≈150 Mbit/s)**. That traffic shares your Wi-Fi with Virtual Desktop or Air Link. Use a good 5 GHz network or the hotspot and keep the PC close.

## How it works

- `UsbIpService` (Java) runs as a foreground service. It requests USB permission for each attached device, opens it through `UsbManager` and passes the usbdevfs file descriptor plus raw descriptors to native code. It also holds a wake lock and a low-latency Wi-Fi lock so the headset keeps serving while it is on your face.
- `usbip.c` (JNI) implements a USB/IP 1.1.1 server on TCP **3240**. It handles `OP_REQ_DEVLIST` and `OP_REQ_IMPORT`, then forwards `CMD_SUBMIT` / `CMD_UNLINK` as **asynchronous usbdevfs URBs** (`USBDEVFS_SUBMITURB` / `REAPURBNDELAY` / `DISCARDURB`). Going below the Java `UsbRequest` API is what makes isochronous transfers possible without root. It also:
  - runs `SET_CONFIGURATION`, `SET_INTERFACE` and `CLEAR_FEATURE(HALT)` locally through the matching ioctls;
  - splits iso transfers larger than 128 packets, the per-URB limit of usbdevfs;
  - detaches kernel drivers and claims every interface.

### Horizon OS permission gotcha

Android only grants USB access to a **UVC (video class)** device if the app holds `android.permission.CAMERA`. Horizon OS adds **`horizonos.permission.USB_CAMERA`** on top of that. Without both, `UsbManager.requestPermission` is denied instantly with no dialog, and logcat shows `UsbUserPermissionManager: USB Camera permission required for USB video class devices`. The app declares and requests both.

## Building

Requirements: JDK 17, Android SDK (platform 34) and NDK 26.3.11579264.

```
echo sdk.dir=C:/path/to/Android/sdk > local.properties
./gradlew assembleDebug
```

The APK ends up in `app/build/outputs/apk/debug/`. GitHub Actions builds it on every push and attaches it to tagged releases.

To log every control transfer, build with `-DVERBOSE_CONTROL` (add it to `LOCAL_CFLAGS` in `app/src/main/jni/Android.mk`) and watch `adb logcat -s QuestUsbIp`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| "USB permission DENIED" in the app | Make sure both camera permissions are granted (Settings → Apps → Quest USB/IP), or reinstall with `adb install -g`. Then press "Volver a pedir permisos USB" (request USB permissions again). |
| `usbip list` shows nothing / times out | Check the Quest IP shown in the app. Allow inbound TCP 3240 on the Quest's network. Keep the app open after a reboot, since it does not start on its own. |
| Device appears but another app grabs it | Close other USB apps on the Quest (e.g. VirtualHere). |
| Lag / dropped frames | It's Wi-Fi bandwidth. Use 5 GHz, move closer, lower the Virtual Desktop bitrate. |

## License

[MIT](LICENSE)
