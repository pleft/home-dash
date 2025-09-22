//                            USER DEFINED SETTINGS
//   Set driver type, fonts to be loaded, pins used and SPI control method etc.
//
//   See the User_Setup_Select.h file if you wish to be able to define multiple
//   setups and then easily select which setup file is used by the compiler.
//
//   If this file is edited correctly then all the library example sketches should
//   run without the need to make any more changes for a particular hardware setup!
//   Note that some sketches are designed for a particular TFT pixel width/height

// Define to disable all #warnings in library (can be put in User_Setup_Select.h)
//#define DISABLE_ALL_LIBRARY_WARNINGS

// ##################################################################################
//
// Section 1. Call up the right driver file and any options for it
//
// ##################################################################################

// === BatController (ST7735S 160x128) ===
#define USER_SETUP_INFO "BatController ST7735 160x128"

#define ST7735_DRIVER
#define TFT_WIDTH   128
#define TFT_HEIGHT  160

#define LOAD_GLCD    // Font 1 (classic 8x6) - always good to have
#define LOAD_FONT2   // Font 2 (small 16px) - what we're using in the sketch

// pick ONE tab; GREENTAB is common for 160x128
// #define ST7735_GREENTAB
#define ST7735_BLACKTAB
// #define ST7735_REDTAB

// 👇 THIS is the key line for your colors:
#define TFT_RGB_ORDER TFT_BGR

// (keep your working pins)
#define TFT_MOSI 21
#define TFT_SCLK 22
#define TFT_CS   -1
#define TFT_DC    5
#define TFT_RST  17
#define TFT_BL    0

#define SPI_FREQUENCY 27000000
