#include "info_display.h"

#include <Arduino.h>
#include "display_api.h"   // your TFT_eSPI setup with BOARD_JC2432W328

extern "C" {
  #include "fake6502.h"
}

// Shared emulator display is declared in display_api.h.

static bool s_initialized = false;
static volatile bool s_blink_flag = false;

// Called once from bitboard_app_main()
void idisplay_init(void)
{
  Serial.println("[TFT] idisplay_init() called");

  if (s_initialized) return;
  s_initialized = true;

  // RGB display is initialized once in Arduino setup().
  tft.setRotation(0);        // adjust if you want portrait/landscape different
  tft.fillScreen(TFT_BLUE);  // big obvious color to see SOMETHING
  tft.setTextColor(TFT_YELLOW, TFT_BLUE);
  tft.setTextFont(2);
  tft.setTextSize(1);

  tft.setCursor(10, 10);
  tft.println("Bitboard 6502");

  tft.setCursor(10, 30);
  tft.println("PC    SP   A  X  Y  ST");
}

// Optional activity hook (for later)
void idsplay_blink_led(uint16_t delay_ms)
{
  (void)delay_ms;
  s_blink_flag = true;
}

static void draw_reg_line()
{
  // Clear the value area only
  tft.fillRect(10, 40, 220, 20, TFT_BLUE);
  tft.setCursor(10, 40);

  tft.printf("PC=%04X  ", *fake6502_pc);
  tft.printf("SP=%02X  ", *fake6502_sp);
  tft.printf("A=%02X ",   *fake6502_a);
  tft.printf("X=%02X ",   *fake6502_x);
  tft.printf("Y=%02X ",   *fake6502_y);
  tft.printf("ST=%02X",   *fake6502_status);
}

static void draw_inst_and_state()
{
  // INST
  tft.fillRect(10, 80, 220, 20, TFT_BLUE);
  tft.setCursor(10, 80);
  tft.printf("INST: %lu", (unsigned long)instructions);

  // STATE
  tft.fillRect(10, 100, 220, 20, TFT_BLUE);
  tft.setCursor(10, 100);

  const char* state = "UNKNOWN";
  switch (fake6502_running_status) {
    case 0: state = "RUN";   break;
    case 1: state = "BREAK"; break;
    case 2: state = "STEP";  break;
  }
  tft.printf("STATE: %s", state);
}

static void draw_activity()
{
  static bool on = false;

  if (!s_blink_flag) return;
  s_blink_flag = false;

  uint16_t color = on ? TFT_DARKGREY : TFT_YELLOW;
  tft.fillCircle(230, 15, 4, color);
  on = !on;
}

// Called from Arduino loop()
void idisplay_update(void)
{
  if (!s_initialized) return;

  draw_reg_line();
  draw_inst_and_state();
  draw_activity();
}
