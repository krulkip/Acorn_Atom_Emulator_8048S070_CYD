#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// The concrete ESP32-S3 RGB panel class is defined once in the main .ino.
// Other emulator translation units only need the common LovyanGFX device API.
extern lgfx::LGFX_Device& tft;
