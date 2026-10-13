
// console.cpp - Arduino Serial-based console for bitboard 6502 on CYD

#include <Arduino.h>
#include "display_api.h"
//extern TFT_eSPI tft;   // use the global tft from the .ino

extern "C" {
  #include "fake6502.h"
  #include "fakemem.h"
}

// --- helpers --------------------------------------------------------

static void print_help()
{
  Serial.println();
  Serial.println(F("Commands:"));
  Serial.println(F("  h - help"));
  Serial.println(F("  r - show registers"));
  Serial.println(F("  c - continue (run)"));
  Serial.println(F("  b - break (pause)"));
  Serial.println(F("  s - single-step"));
  Serial.println();
  // ---- TFT mirror ----
  //tft.fillScreen(TFT_BLACK);
  //tft.setTextDatum(TL_DATUM);
  //tft.setTextColor(TFT_WHITE, TFT_BLACK);

  int y = 10;
  //tft.setCursor(10, y); tft.println("Commands:");              y += 16;
  //tft.setCursor(10, y); tft.println("h - help");                y += 16;
  //tft.setCursor(10, y); tft.println("r - show registers");      y += 16;
  //tft.setCursor(10, y); tft.println("c - continue (run)");      y += 16;
  //tft.setCursor(10, y); tft.println("b - break (pause)");       y += 16;
  //tft.setCursor(10, y); tft.println("s - single-step");         y += 16;
  //tft.setCursor(10, y); tft.println("m XXXX - mem dump");       y += 16;
}

static void print_registers()
{
  Serial.print(F("PC="));
  Serial.printf("%04X ", *fake6502_pc);

  Serial.print(F("SP="));
  Serial.printf("%02X ", *fake6502_sp);

  Serial.print(F("A="));
  Serial.printf("%02X ", *fake6502_a);

  Serial.print(F("X="));
  Serial.printf("%02X ", *fake6502_x);

  Serial.print(F("Y="));
  Serial.printf("%02X ", *fake6502_y);

  Serial.print(F("ST="));
  Serial.printf("%02X ", *fake6502_status);

  Serial.print(F(" INST="));
  Serial.println((unsigned long)instructions);
}

static void dump_memory(uint16_t addr)
{
  Serial.printf("Memory @%04X:\n", addr);
  for (int row = 0; row < 4; ++row) {  // 4 lines of 16 bytes = 64 bytes
    uint16_t base = addr + row * 16;
    Serial.printf("%04X: ", base);
    for (int i = 0; i < 16; ++i) {
      Serial.printf("%02X ", fakemem[base + i]);
    }
    Serial.println();
  }
}


static void handle_command(char cmd, const char *arg)
{
  (void)arg;

  switch (cmd) {
    case 'h':
    case 'H':
      print_help();
      break;

    case 'r':
    case 'R':
      print_registers();
      break;

    case 'm':
    case 'M':
      if (!arg || !*arg) {
        Serial.println(F("Usage: m XXXX   (hex address)"));
      } else {
        // parse hex address
        uint32_t val = strtoul(arg, nullptr, 16);
        dump_memory((uint16_t)val);
      }
      break;  

    case 'c':
    case 'C':
      fake6502_running_status = 0;  // run
      Serial.println(F("RUN"));
      break;

    case 'b':
    case 'B':
      fake6502_running_status = 1;  // break
      Serial.println(F("BREAK"));
      break;

    case 's':
    case 'S':
      Serial.println(F("STEP"));
      fake6502_running_status = 2;  // single-step once
      // main loop in bitboard_6502.c will set it back to 1 after step
      print_registers();
      break;

    default:
      Serial.print(F("Unknown command '"));
      Serial.print(cmd);
      Serial.println(F("'. Type 'h' for help."));
      break;
  }
}

// --- FreeRTOS task entrypoint (called from C code) ------------------

// C linkage so bitboard_6502.c can call this
extern "C" void console_task(void *pvParameters)
{
  (void)pvParameters;

  char line[64];
  size_t pos = 0;

  print_help();

  for (;;) {
    while (Serial.available() > 0) {
      char c = (char)Serial.read();

      if (c == '\r' || c == '\n') {
        if (pos > 0) {
          line[pos] = 0;
          // strip leading spaces
          char *p = line;
          while (*p == ' ' || *p == '\t') p++;
          if (*p) {
            char cmd = *p++;
            while (*p == ' ' || *p == '\t') p++;
            handle_command(cmd, p);
          }
          pos = 0;
        }
      } else {
        if (pos < sizeof(line) - 1) {
          line[pos++] = c;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
