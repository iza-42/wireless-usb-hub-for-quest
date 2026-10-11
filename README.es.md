# Hub USB inalámbrico para Quest

**[Read in English](README.md)**

Convierte unas Meta Quest en un hub USB inalámbrico: enchufás un aparato USB en las gafas y lo usás en la PC por Wi-Fi, como si estuviera conectado directo a la computadora.

Nació para usar el **HTC VIVE Facial Tracker** en una Quest 2 y tener face tracking inalámbrico (SRanipal / VRCFaceTracking), pero también sirve con aparatos USB comunes: pendrives, cámaras, controles, etc.

- **Sin root**, sin licencias pagas y sin límite de dispositivos.
- Usa el protocolo estándar **USB/IP**, así que en la PC se usa el cliente gratuito y firmado [usbip-win2](https://github.com/vadimgrn/usbip-win2).
- Soporta transferencias **control, bulk, interrupt e isócronas**. Las isócronas son las que usan las webcams y los aparatos de audio, y son justo lo que no tiene el servidor USB/IP gratuito que ya existe para Android.

```
 Aparato USB ──► Quest (app Quest USB/IP) ══ Wi-Fi / USB/IP :3240 ══► PC (usbip-win2) ──► Windows lo ve como un USB local
```

## Estado

Probado en una Quest 2 con Horizon OS (basado en Android 14) con:

| Aparato | Resultado |
|---|---|
| Pendrive Kingston DataTraveler 3.0 | Funciona: aparece como un disco normal |
| HTC VIVE Facial Tracker (`0bb4:0321`) | Funciona: SRanipal lo detecta, VRCFaceTracking muestra la cámara, ~57 fps |

Otros modelos de Quest deberían funcionar, pero no están probados. Si lo probás, abrí un issue con el resultado.

## Requisitos

- Unas Meta Quest con el **modo desarrollador** activado (app Meta Horizon del celular → Dispositivos → Modo desarrollador).
- La PC y la Quest en la misma red. El hotspot móvil de Windows funciona bien.
- En la PC: [usbip-win2](https://github.com/vadimgrn/usbip-win2/releases) (probado con 0.9.8.1).
- Para el face tracker: el runtime **SRanipal** de VIVE, más VRCFaceTracking o la app que uses.
- Un adaptador o cable USB-C OTG si tu aparato no tiene ficha USB-C.

## Instalación

1. Bajá `quest-usbip.apk` desde [Releases](../../releases). También podés compilarlo vos, ver [Compilar](#compilar).
2. Instalalo con adb (o SideQuest):
   ```
   adb install -g quest-usbip.apk
   ```
   `-g` concede los permisos de cámara de entrada. Si no, los aceptás al abrir la app por primera vez.
3. Instalá **usbip-win2** en la PC.

## Uso

1. Enchufá el aparato USB en la Quest. La app arranca sola en segundo plano, sin abrir ventana. La primera vez, Horizon OS puede preguntarte con qué app abrir el aparato: elegí **Quest USB/IP** y marcá "siempre". También podés abrir **Quest USB/IP** vos (Biblioteca → Fuentes desconocidas) para ver la IP de las gafas y los aparatos compartidos.
2. Si aparece el permiso USB, aceptalo.
3. En la PC, listá los aparatos y conectá el que quieras:
   ```
   usbip list -r <ip-de-la-quest>
   usbip attach -r <ip-de-la-quest> -b <busid>
   ```
   También podés usar `wusbip`, la interfaz gráfica que viene con usbip-win2.
4. Para desconectarlo: `usbip detach -p <puerto>` (lo ves con `usbip port`).
5. Desenchufá el aparato. Cuando no queda ningún aparato USB, la app se cierra sola a los 15 s para ahorrar batería. Si la abrís sin nada enchufado, se cierra a los 60 s.

### VIVE Facial Tracker

[`scripts/connect-face-tracker.ps1`](scripts/connect-face-tracker.ps1) busca el tracker, lo conecta y abre SRanipal:

```
powershell -ExecutionPolicy Bypass -File scripts\connect-face-tracker.ps1 -Quest 192.168.137.16
```

Después abrí VRCFaceTracking como siempre.

Notas:
- El tracker **no manda video hasta que SRanipal lo activa** con comandos propios del fabricante. Una app de webcam común como ffmpeg lo abre pero recibe 0 bytes. Es normal.
- Transmite **400×400 YUY2 sin comprimir a 60 fps, unos 18 MB/s (≈150 Mbit/s)**. Ese tráfico comparte el Wi-Fi con Virtual Desktop o Air Link. Usá una buena red de 5 GHz o el hotspot, y tené la PC cerca.

## Cómo funciona

- `UsbIpService` (Java) corre como servicio en primer plano. Pide el permiso USB de cada aparato conectado, lo abre con `UsbManager` y le pasa al código nativo el file descriptor de usbdevfs y los descriptores crudos. También mantiene un wake lock y un Wi-Fi lock de baja latencia, para que las gafas sigan sirviendo mientras las tenés puestas. Cuando no queda ningún aparato USB conectado, se detiene y el proceso se cierra, así esos locks no gastan la batería.
- `usbip.c` (JNI) implementa un servidor USB/IP 1.1.1 en TCP **3240**. Atiende `OP_REQ_DEVLIST` y `OP_REQ_IMPORT`, y reenvía `CMD_SUBMIT` / `CMD_UNLINK` como **URBs asíncronos de usbdevfs** (`USBDEVFS_SUBMITURB` / `REAPURBNDELAY` / `DISCARDURB`). Trabajar por debajo de la API Java `UsbRequest` es lo que permite transferencias isócronas sin root. Además:
  - resuelve `SET_CONFIGURATION`, `SET_INTERFACE` y `CLEAR_FEATURE(HALT)` localmente con los ioctl correspondientes;
  - divide las transferencias isócronas de más de 128 paquetes, que es el límite por URB de usbdevfs;
  - desconecta los drivers del kernel y reclama todas las interfaces.

### El permiso escondido de Horizon OS

Android solo da acceso USB a un aparato **UVC (clase video)** si la app tiene `android.permission.CAMERA`. Horizon OS además exige **`horizonos.permission.USB_CAMERA`**. Si falta alguno de los dos, `UsbManager.requestPermission` se rechaza al instante sin mostrar ningún aviso, y en logcat aparece `UsbUserPermissionManager: USB Camera permission required for USB video class devices`. La app declara y pide los dos.

## Compilar

Hace falta: JDK 17, Android SDK (platform 34) y NDK 26.3.11579264.

```
echo sdk.dir=C:/ruta/al/Android/sdk > local.properties
./gradlew assembleDebug
```

El APK queda en `app/build/outputs/apk/debug/`. GitHub Actions lo compila en cada push y lo adjunta a las releases con tag.

Para registrar cada transferencia de control, compilá con `-DVERBOSE_CONTROL` (agregalo a `LOCAL_CFLAGS` en `app/src/main/jni/Android.mk`) y mirá `adb logcat -s QuestUsbIp`.

## Problemas comunes

| Síntoma | Solución |
|---|---|
| "Permiso USB DENEGADO" en la app | Revisá que los dos permisos de cámara estén concedidos (Ajustes → Apps → Quest USB/IP), o reinstalá con `adb install -g`. Después tocá "Volver a pedir permisos USB". |
| `usbip list` no muestra nada o tarda y falla | Revisá la IP que muestra la app. Permití TCP 3240 entrante en la red de la Quest. La app solo corre mientras hay un aparato USB enchufado. Volvé a enchufarlo o abrí la app. |
| El aparato aparece pero otra app lo toma | Cerrá otras apps USB de la Quest (por ejemplo VirtualHere). |
| Lag o cuadros perdidos | Es el ancho de banda del Wi-Fi. Usá 5 GHz, acercate y bajá el bitrate de Virtual Desktop. |

## Licencia

[MIT](LICENSE)
