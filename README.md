# ESP32-CAM Prusa Connect

ESP-IDF firmware for an AI Thinker style ESP32-CAM. It:

- Connects to Wi-Fi.
- Hosts a local web page at the ESP32 IP address.
- Lets you edit Wi-Fi, Prusa Connect token/fingerprint/endpoint, upload interval, resolution, image quality, orientation, and common ESP32 camera sensor options.
- Provides local `/jpg` snapshots and `/stream` MJPEG video.
- Uploads snapshots to Prusa Connect with the Camera API.

## Build and flash

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with your serial port.

## First boot

On first boot the device starts an access point:

- SSID: `ESP32CAM-Setup`
- Password: `esp32cam`
- Setup page: `http://192.168.4.1`

Enter your Wi-Fi credentials, save, and reboot. Once connected, the serial monitor prints the assigned IP address. Open that IP in a browser to change settings later.

## Prusa Connect setup

In Prusa Connect, add/register a camera for your printer and copy the camera `token` and `fingerprint` into the ESP32 web page.

Default upload endpoint:

```text
https://connect.prusa3d.com/c/snapshot
```

The Prusa Camera API accepts JPG snapshots with `token` and `fingerprint` headers.

## Camera module

This project uses the common AI Thinker ESP32-CAM pin map:

| Signal | GPIO |
| --- | --- |
| PWDN | 32 |
| RESET | -1 |
| XCLK | 0 |
| SIOD | 26 |
| SIOC | 27 |
| Y9 | 35 |
| Y8 | 34 |
| Y7 | 39 |
| Y6 | 36 |
| Y5 | 21 |
| Y4 | 19 |
| Y3 | 18 |
| Y2 | 5 |
| VSYNC | 25 |
| HREF | 23 |
| PCLK | 22 |

If your board is different, edit the pin constants near the top of [main/main.c](main/main.c).
