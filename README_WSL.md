# WSL Build and Upload Commands

This document contains the complete set of commands needed to build and upload the Home Dashboard project using WSL via PowerShell.

## Prerequisites

- Windows 10/11 with WSL2 installed
- USB/IP for Windows (usbipd-win) installed
- ESP32 connected and shared to WSL via USB/IP
- Arduino-CLI installed in WSL
- Required libraries: `NimBLE-Arduino`, `TFT_eSPI`

## USB Device Sharing (Run as Administrator in PowerShell)

```powershell
# List USB devices to find your ESP32
usbipd list

# Bind ESP32 device (replace 1-4 with your device's bus ID)
usbipd bind --busid 1-4

# Attach device to WSL
usbipd attach --wsl --busid 1-4
```

## Build and Upload Commands

### 1) Clear build directory
```bash
wsl -e bash -c "cd /path/to/home-dash && rm -rf build && mkdir build"
```

### 2) Compile code (with TFT disabled for smaller size)
```bash
wsl -e bash -c "cd /path/to/home-dash && export PATH=`$PATH:/path/to/home-dash/bin && arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=no_ota --build-property compiler.cpp.extra_flags='-DHAS_TFT=0' --build-path build home-dash.ino"
```

### 3) Upload firmware
```bash
wsl -e bash -c "cd /path/to/home-dash && ~/.arduino15/packages/esp32/tools/esptool_py/5.0.0/esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x10000 build/home-dash.ino.bin"
```

### 4) Create LittleFS filesystem image
```bash
wsl -e bash -c "cd /path/to/home-dash && ~/.arduino15/packages/esp32/tools/mklittlefs/3.0.0-gnu12-dc7f933/mklittlefs -c data -b 4096 -s 0x1E0000 data.littlefs.bin"
```

### 5) Upload LittleFS filesystem
```bash
wsl -e bash -c "cd /path/to/home-dash && ~/.arduino15/packages/esp32/tools/esptool_py/5.0.0/esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x210000 data.littlefs.bin"
```

### Alternative: Upload both firmware and filesystem at once
```bash
wsl -e bash -c "cd /path/to/home-dash && ~/.arduino15/packages/esp32/tools/esptool_py/5.0.0/esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x10000 build/home-dash.ino.bin 0x210000 data.littlefs.bin"
```

### 6) Clean up temporary files
```bash
wsl -e bash -c "cd /path/to/home-dash && rm -f data.littlefs.bin"
```

### 7) Monitor serial output
```bash
wsl -e bash -c "cd /path/to/home-dash && ./bin/arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200"
```

## Notes

- These commands use PowerShell `wsl` approach to avoid idling issues when running commands directly in WSL terminal
- Replace `/path/to/home-dash` with your actual project path
- Replace `/dev/ttyUSB0` with your actual ESP32 port if different
- The ESP32 must be connected and shared to WSL via USB/IP before running upload commands
- Uses `no_ota` partition scheme for 4MB ESP32: 2MB app + 1.9MB filesystem
- TFT support disabled by default to reduce firmware size
- Uses LittleFS filesystem (not SPIFFS) for web assets

## Expected Results

- **Compilation**: ~67% program storage (1.41MB), ~24% dynamic memory usage
- **Upload**: Firmware uploaded to ESP32 and device reset
- **Filesystem Upload**: LittleFS filesystem with web interface files (including 64KB app.js) uploaded to ESP32
- **Boot**: ESP32 boots successfully with LittleFS mounted and web interface accessible

## Troubleshooting

- **Upload fails**: Check that ESP32 is connected and shared to WSL
- **USB/IP issues**: Verify USB/IP commands were run as administrator
- **Wrong port**: Check that the ESP32 port is correct (`/dev/ttyUSB0`)
- **Upload mode**: Ensure the ESP32 is in the correct mode for uploading
- **"No more free space" error**: Use `no_ota` partition scheme (not `min_spiffs`) for 4MB ESP32
- **LittleFS mount errors**: Ensure using `mklittlefs` (not `mkspiffs`) and correct partition size
- **Firmware too big**: Use `HAS_TFT=0` flag to disable TFT support and reduce size
- **Filesystem mismatch**: Code uses LittleFS, so create filesystem with `mklittlefs` tool
