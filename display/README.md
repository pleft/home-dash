# Display Configuration

## Setup Instructions

### 1. Copy Configuration File
Replace the TFT_eSPI library configuration with this custom one:

**Copy this file to:**
```
~/Arduino/libraries/TFT_eSPI/User_Setup.h
```

**Steps:**
1. Navigate to your Arduino libraries directory
2. Go to `TFT_eSPI` folder
3. Replace the existing `User_Setup.h` with this file
4. Recompile your project

**Note:** This configuration is specifically for CircuitMess BatController's ST7735S 160x128 display with ESP32.