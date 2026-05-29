# ESP32-CAM Prusa Connect

This repository contains ESP-IDF firmware for an AI Thinker-style ESP32-CAM module that connects to Wi-Fi, hosts a local configuration page, streams video, and uploads snapshots to Prusa Connect.

## What it does
- Connects to Wi-Fi with first-boot setup AP fallback
- Scans nearby Wi-Fi networks and supports blank passwords for open networks
- Hosts a local camera setup page at the device IP and `http://esp32cam.local`
- Streams MJPEG video and snapshots
- Uploads images to Prusa Connect
- Shows last upload status and a manual test-upload result
- Provides reboot and factory reset controls
- Includes info buttons explaining camera setting values in plain English

## Build and flash
1. Install ESP-IDF and open a shell with the ESP-IDF environment activated.
2. Run:
   ```sh
   idf.py set-target esp32
   idf.py build
   idf.py -p COMx flash monitor
   ```
3. Replace `COMx` with your serial port.

## Repository
Source code is available in this GitHub repository.

## License
This project is licensed under the MIT License. See [LICENSE](../LICENSE).

## Notes
- Use the setup page on first boot to configure Wi-Fi and camera settings.
- The default setup SSID is `ESP32CAM-Setup` with password `esp32cam`.
- Leave the Wi-Fi password blank for open networks.
- Prusa Connect token and fingerprint fields are masked on the settings page.
- Use `Test upload now` to verify Prusa Connect credentials without waiting for the automatic interval.
