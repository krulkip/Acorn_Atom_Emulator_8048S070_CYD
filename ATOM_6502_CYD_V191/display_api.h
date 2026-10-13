#pragma once
#include <TFT_eSPI.h>

// CYD uses the TFT_eSPI display object defined in the main sketch.
// All emulator translation units must see the exact same concrete type.
extern TFT_eSPI tft;
