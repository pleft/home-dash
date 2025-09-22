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

### 2) Compile code
```bash
wsl -e bash -c "cd /path/to/home-dash && arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --build-path build home-dash.ino"
```

### 3) Upload code
```bash
wsl -e bash -c "cd /path/to/home-dash && arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --build-path build home-dash.ino"
```

### 4) Create data folder littlefs binary
```bash
wsl -e bash -c "cd /path/to/home-dash && ~/.arduino15/packages/esp32/tools/mklittlefs/3.0.0-gnu12-dc7f933/mklittlefs -c data -b 4096 -s 0x20000 data.littlefs.bin"
```

### 5) Upload data littlefs binary
```bash
wsl -e bash -c "cd /path/to/home-dash && ~/.arduino15/packages/esp32/tools/esptool_py/5.0.0/esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x3D0000 data.littlefs.bin"
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

## Expected Results

- **Compilation**: ~66% program storage, ~17% dynamic memory usage
- **Upload**: Firmware uploaded to ESP32 and device reset
- **Data Upload**: LittleFS filesystem with web interface files uploaded to ESP32

## Troubleshooting

- If upload fails, check that ESP32 is connected and shared to WSL
- Verify USB/IP commands were run as administrator
- Check that the ESP32 port is correct (`/dev/ttyUSB0`)
- Ensure the ESP32 is in the correct mode for uploading
