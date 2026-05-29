# ESP32-CAM Prusa Connect

This project is an ESP-IDF firmware build for an AI Thinker-style ESP32-CAM module.

## What it does
- Connects to Wi-Fi
- Hosts a local camera setup page
- Streams MJPEG video and snapshots
- Uploads images to Prusa Connect

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

## Notes
- Use the setup page on first boot to configure Wi-Fi and camera settings.
- The default setup SSID is `ESP32CAM-Setup` with password `esp32cam`.
