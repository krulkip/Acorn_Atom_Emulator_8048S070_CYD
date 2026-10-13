#pragma once
// Set to 1 to show TFT board-selection messages during compilation.
// Normally leave disabled to keep compiler output clean.
#ifndef TFT_SETUP_BUILD_MESSAGES
#define TFT_SETUP_BUILD_MESSAGES 0
#endif

#ifndef USER_SETUP_LOADED
  #define USER_SETUP_LOADED
#endif

// bring the board choice in for BOTH the sketch and the library:
#include "tft_board_select.h"   // <-- edit only this to switch boards

// ---------------- Common to all boards ----------------
#define USER_SETUP_INFO "Universal ESP32 setup"
#define TFT_WIDTH   240
#define TFT_HEIGHT  320

// HSPI + shared pins (your configs)
#define USE_HSPI_PORT
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1

// Speeds
#define SPI_FREQUENCY        55000000
#define SPI_READ_FREQUENCY   20000000
#define SPI_TOUCH_FREQUENCY   2500000

// Fonts
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
// #define SUPPORT_TRANSACTIONS
// ---------- Selection (no silent default) ----------
#if   defined(BOARD_CYD)               // ESP32-2432S028R (ILI9341_2)
  #define ILI9341_2_DRIVER
  #define TFT_BL            21
  #define TFT_BACKLIGHT_ON  HIGH
  #define TFT_INVERSION_OFF
  #define ATOM_AUDIO_PIN     26
  #if TFT_SETUP_BUILD_MESSAGES
  #pragma message("[tft_setup] Selected BOARD_CYD (ILI9341_2)")
  #endif
  
#elif   defined(BOARD_CYD2USB)         // ESP32-2432S028R (ST7789)
  #define ST7789_DRIVER
  #define TFT_BL            21
  #define TFT_BACKLIGHT_ON  HIGH
  #define TFT_INVERSION_OFF
  #define ATOM_AUDIO_PIN     26
  #if TFT_SETUP_BUILD_MESSAGES
  #pragma message("[tft_setup] Selected BOARD_CYD2USB (ST7789)")
  #endif
  
#elif defined(BOARD_JC2432W328)   // JC2432W328 ST7789
  #define ST7789_DRIVER
  #define TFT_BL            27
  #define TFT_BACKLIGHT_ON  HIGH
  #define TFT_RGB_ORDER     TFT_BGR
  #define TFT_INVERSION_OFF
  #define ATOM_AUDIO_PIN     26

  // Onboard microSD: dedicated VSPI bus, separate from TFT_eSPI HSPI.
  #define ATOM_HAS_SD         1
  #define ATOM_SD_SCLK       18
  #define ATOM_SD_MISO       19
  #define ATOM_SD_MOSI       23
  #define ATOM_SD_CS          5

  #if TFT_SETUP_BUILD_MESSAGES
  #pragma message("[tft_setup] Selected BOARD_JC2432W328 (ST7789)")
  #endif

#else
  #error "[tft_setup] No board selected. Edit tft_board_select.h"
#endif
