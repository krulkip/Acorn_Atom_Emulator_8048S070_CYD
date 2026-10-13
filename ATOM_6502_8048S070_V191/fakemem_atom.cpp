#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include "display_api.h"

#include "fakemem.h"
#include "fake6502.h"
#include "fakemem_atom.h"
#include "atommc_stub.h"

#define ATOM_VRAM_START 0x8000
#define ATOM_VRAM_END   0x81FF
#define ATOM_COLS 32
#define ATOM_ROWS 16

#define ATOM_MODE1_VRAM_START 0x8000
#define ATOM_MODE1_VRAM_END   0x83FF
#define ATOM_MODE1_WIDTH      128
#define ATOM_MODE1_HEIGHT     64
#define ATOM_MODE1_BYTES_ROW  16

#define ATOM_MODE2_VRAM_START 0x8000
#define ATOM_MODE2_VRAM_END   0x85FF
#define ATOM_MODE2_WIDTH      128
#define ATOM_MODE2_HEIGHT     96
#define ATOM_MODE2_BYTES_ROW  16

#define ATOM_MODE3_VRAM_START 0x8000
#define ATOM_MODE3_VRAM_END   0x8BFF
#define ATOM_MODE3_WIDTH      128
#define ATOM_MODE3_HEIGHT     192
#define ATOM_MODE3_BYTES_ROW  16

#define ATOM_MODE4_VRAM_START 0x8000
#define ATOM_MODE4_VRAM_END   0x97FF
#define ATOM_MODE4_WIDTH      256
#define ATOM_MODE4_HEIGHT     192
#define ATOM_MODE4_BYTES_ROW  32
#define ATOM_VDG_NATIVE_W     256
#define ATOM_VDG_NATIVE_H     192

// D-ROM COLOUR support: after CLEAR 1, COLOUR n clears VDG bit $20,
// selecting the MC6847 64x64 four-colour graphics mode ($B000 high nibble $10).
// Four logical pixels are packed into each byte, two bits per pixel.
#define ATOM_COLOUR1_VRAM_START 0x8000
#define ATOM_COLOUR1_VRAM_END   0x83FF
#define ATOM_COLOUR1_WIDTH      64
#define ATOM_COLOUR1_HEIGHT     64
#define ATOM_COLOUR1_BYTES_ROW  16

// Genuine Atom colour counterparts selected by the D-ROM COLOUR command.
// COLOUR clears VDG bit $20, so monochrome $70/$B0/$F0 become
// colour $50/$90/$D0 respectively.  These are four-colour 2-bpp modes.
#define ATOM_COLOUR2_VRAM_START 0x8000
#define ATOM_COLOUR2_VRAM_END   0x87FF
#define ATOM_COLOUR2_WIDTH      128
#define ATOM_COLOUR2_HEIGHT     64
#define ATOM_COLOUR2_BYTES_ROW  32

#define ATOM_COLOUR3_VRAM_START 0x8000
#define ATOM_COLOUR3_VRAM_END   0x8BFF
#define ATOM_COLOUR3_WIDTH      128
#define ATOM_COLOUR3_HEIGHT     96
#define ATOM_COLOUR3_BYTES_ROW  32

#define ATOM_COLOUR4_VRAM_START 0x8000
#define ATOM_COLOUR4_VRAM_END   0x97FF
#define ATOM_COLOUR4_WIDTH      128
#define ATOM_COLOUR4_HEIGHT     192
#define ATOM_COLOUR4_BYTES_ROW  32

extern uint8_t fakemem[1 << 16];   // from fakemem.c
// tft is declared by display_api.h
extern uint8_t atom_last_b000_write; // from fakemem.c

// Track last drawn chars to reduce flicker
static uint8_t last_vram[ATOM_ROWS * ATOM_COLS];
static uint8_t last_mode1_vram[ATOM_MODE1_BYTES_ROW * ATOM_MODE1_HEIGHT];
static uint8_t last_mode2_vram[ATOM_MODE2_BYTES_ROW * ATOM_MODE2_HEIGHT];
static uint8_t last_mode3_vram[ATOM_MODE3_BYTES_ROW * ATOM_MODE3_HEIGHT];
static uint8_t last_mode4_vram[ATOM_MODE4_BYTES_ROW * ATOM_MODE4_HEIGHT];
static uint8_t last_colour1_vram[ATOM_COLOUR1_BYTES_ROW * ATOM_COLOUR1_HEIGHT];
static uint8_t last_colour2_vram[ATOM_COLOUR2_BYTES_ROW * ATOM_COLOUR2_HEIGHT];
static uint8_t last_colour3_vram[ATOM_COLOUR3_BYTES_ROW * ATOM_COLOUR3_HEIGHT];
static uint8_t last_colour4_vram[ATOM_COLOUR4_BYTES_ROW * ATOM_COLOUR4_HEIGHT];
static uint8_t last_video_mode = 0xFF;
static uint8_t last_colour_css = 0xFF;

// ---------------- ROM loading ----------------

static bool load_arom_bank(uint8_t bank, const char *path)
{
    uint8_t *dst = atom_arom_bank_ptr(bank);
    if (!dst) {
        Serial.printf("[AROM] Invalid bank %u for %s\n", bank, path);
        return false;
    }

    File f = SPIFFS.open(path, "r");
    if (!f) {
        Serial.printf("[AROM] Bank %u: cannot open %s; bank left empty\n",
                      bank, path);
        atom_arom_mark_loaded(bank, 0);
        return false;
    }

    size_t n = f.read(dst, 0x1000);
    f.close();

    if (n != 0x1000) {
        Serial.printf("[AROM] Bank %u: %s wrong size (%u bytes); bank left empty\n",
                      bank, path, (unsigned)n);
        memset(dst, 0xFF, 0x1000);
        atom_arom_mark_loaded(bank, 0);
        return false;
    }

    atom_arom_mark_loaded(bank, 1);
    Serial.printf("[AROM] Bank %u loaded: %-22s -> $A000-$AFFF\n",
                  bank, path);
    return true;
}

static bool load_rom_block(const char *path, uint16_t base)
{
    File f = SPIFFS.open(path, "r");
    if (!f) {
        Serial.printf("[ATOM] Cannot open %s\n", path);
        return false;
    }

    size_t n = f.read(&fakemem[base], 0x1000);
    f.close();

    if (n != 0x1000) {
        Serial.printf("[ATOM] ROM %s wrong size (%u bytes)\n",
                      path, (unsigned)n);
        return false;
    }

    Serial.printf("[ATOM] Loaded %-24s at $%04X-$%04X\n",
                  path, base, base + 0x0FFF);
    return true;
}


static bool load_ram_image(const char *path, uint16_t base, size_t expected)
{
    File f = SPIFFS.open(path, "r");
    if (!f) {
        Serial.printf("[ASBK-S] Cannot open %s\n", path);
        return false;
    }

    size_t file_size = f.size();
    if (file_size != expected) {
        Serial.printf("[ASBK-S] %s wrong size (%u, expected %u)\n",
                      path, (unsigned)file_size, (unsigned)expected);
        f.close();
        return false;
    }

    size_t n = f.read(&fakemem[base], expected);
    f.close();

    if (n != expected) {
        Serial.printf("[ASBK-S] Short read %s (%u/%u)\n",
                      path, (unsigned)n, (unsigned)expected);
        return false;
    }

    Serial.printf("[ASBK-S] Loaded %-20s at $%04X-$%04X\n",
                  path, base, (unsigned)(base + expected - 1));
    return true;
}

uint8_t atom_reload_asbk_s(void)
{
    // V178: Atom mode restores the proven V141/V139 ASBK-S V24 behaviour.
    // BREAK runs the real Kernel reset, which clears the resident $1000
    // workspace, so reload V24 only after RESET has reached OSRDCH.
    return load_ram_image("/ASBK-S_v24.bin", 0x1000, 0x0661);
}


void fakemem_init_atom()
{
    memset(fakemem, 0, sizeof(fakemem));
    atommmc_stub_reset();

    // V178: restore the proven V141/V139 resident ASBK-S V24 image.
    // It is RAM at $1000-$1660, not mapped ROM. LL97 at $1005 is invoked
    // later only when the machine is staying in Atom mode. SHIFT+BREAK/BBC
    // bypasses LL97; the preceding real Atom reset clears this workspace.
    load_ram_image("/ASBK-S_v24.bin", 0x1000, 0x0661);

    // V100: Atom BASIC RND uses the five-byte state at $0008-$000C.
    // Real power-up RAM is not guaranteed to be all zero; an all-zero state
    // locks the BASIC LFSR permanently at RND=0.  Give cold-start RAM a
    // deterministic non-zero power-up seed.  BASIC advances it normally.
    // This is intentionally done once here, not on every RND call.
    fakemem[0x0008] = 0x78;
    fakemem[0x0009] = 0x56;
    fakemem[0x000A] = 0x34;
    fakemem[0x000B] = 0x12;
    fakemem[0x000C] = 0x90;
    Serial.printf("[RND] Cold-start seed $08-$0C = %02X %02X %02X %02X %02X\n",
                  fakemem[8], fakemem[9], fakemem[10], fakemem[11], fakemem[12]);

    // V116: full 8-bank #A000 Utility-ROM set.
    // The original switch-card action remains STA $BFFF; bank 0 is selected
    // at power-up.  $9FFF remains ordinary RAM.
    atom_arom_clear_banks();
    bool utilLoaded = load_arom_bank(0, "/Atom_pcharme.rom");
    bool asbkLoaded = load_arom_bank(1, "/ASBK-I_v1.03.rom");
    load_arom_bank(2, "/DISATOM.rom");
    load_arom_bank(3, "/SALFAA_2.6.rom");
    load_arom_bank(4, "/GAGS_2.3.rom");
    load_arom_bank(5, "/PCBOX_v1.02.rom");
    load_arom_bank(6, "/TOOLBUG.rom");
    load_arom_bank(7, "/AXR-1.rom");

    // Power-up with P-Charme selected.
    atom_arom_select(0);

    load_rom_block("/Atom_Basic.rom",         0xC000);
    load_rom_block("/Atom_FloatingPoint.rom", 0xD000);

    // V117 Step 3 diagnostic: AtoMMC2 V3.02E at #E000, with the documented patched
    // Atom Kernel at #F000.  The controller stub only supports the startup
    // utility handshake at this stage; filesystem commands come later.
    load_rom_block("/ATMMC3E.rom",            0xE000);
    load_rom_block("/Atom_Kernel.rom",        0xF000);

    // V151 diagnostic: Atom-targeted BBC BASIC III image.
    // Load only; normal V139 Atom/ASBK/AtoMMC startup is otherwise unchanged.
    load_ram_image("/ATBASIC3",               0x4000, 0x4000);

    Serial.printf("[AROM] Selected bank %u at power-up\n", atom_arom_bank);
    if (utilLoaded) {
        Serial.printf("[AROM] Bank 0 signature A000/A001 = %02X %02X\n",
                      fakemem[0xA000], fakemem[0xA001]);
        Serial.printf("[AROM] Interpreter entry $A002: %02X %02X %02X %02X %02X %02X\n",
                      fakemem[0xA002], fakemem[0xA003], fakemem[0xA004],
                      fakemem[0xA005], fakemem[0xA006], fakemem[0xA007]);

        if (fakemem[0xA000] == 0x40 && fakemem[0xA001] == 0xBF)
            Serial.println("[AROM] Bank 0 Utility-ROM signature valid; D-ROM can chain to $A002");
        else
            Serial.println("[AROM] WARNING: bank 0 Utility-ROM signature is not $40 $BF");
    } else {
        Serial.println("[AROM] No /Atom_pcharme.rom - bank 0 is empty");
    }

    if (asbkLoaded) {
        uint8_t *b1 = atom_arom_bank_ptr(1);
        Serial.printf("[AROM] Bank 1 ASBK-I signature A000/A001 = %02X %02X\n",
                      b1[0], b1[1]);
    }

    memset(last_vram, 0xFF, sizeof(last_vram));  // force full redraw on first frame
    memset(last_mode1_vram, 0xFF, sizeof(last_mode1_vram));
    memset(last_mode2_vram, 0xFF, sizeof(last_mode2_vram));
    memset(last_mode3_vram, 0xFF, sizeof(last_mode3_vram));
    memset(last_mode4_vram, 0xFF, sizeof(last_mode4_vram));
    memset(last_colour1_vram, 0xFF, sizeof(last_colour1_vram));
    memset(last_colour2_vram, 0xFF, sizeof(last_colour2_vram));
    memset(last_colour3_vram, 0xFF, sizeof(last_colour3_vram));
    memset(last_colour4_vram, 0xFF, sizeof(last_colour4_vram));
    last_video_mode = 0xFF;
    last_colour_css = 0xFF;

    // Debug: show reset & VRAM contents
    uint16_t rst = fakemem[0xFFFC] | (fakemem[0xFFFD] << 8);
    Serial.printf("[ATOM] Memory init complete. RESET=$%04X\n", rst);
    Serial.printf("[VRAM] 8000=%02X 8001=%02X\n", fakemem[0x8000], fakemem[0x8001]);
}

// ---------------- Atom char → ASCII ----------------

// Atom/6847 alphanumeric screen code → host character.
// The Atom ROM converts ASCII before storing it in screen RAM:
//   $00='@', $01-$1A='A'-'Z', $1B='[', $1C='\\',
//   $1D=']', $1E=up-arrow, $1F='_', $20=' '.
// Bit 7 is the inverse-video flag and does not change the glyph.
static char atom_char_to_ascii(uint8_t ch)
{
    ch &= 0x7F;  // ignore inverse bit for glyph selection

    if (ch == 0x00) return '@';

    if (ch >= 0x01 && ch <= 0x1A) {
        return 'A' + (ch - 1);
    }

    switch (ch) {
        case 0x1B: return '[';
        case 0x1C: return '\\';
        case 0x1D: return ']';
        case 0x1E: return '^';   // placeholder; atom_video_update draws an actual up-arrow
        case 0x1F: return '_';
        case 0x20: return ' ';
        case 0x40: return ' ';   // Atom CLEAR 0 fills text VRAM with $40 (blank)
        default: break;
    }

    // The remaining text codes used by the Atom ROM line up with ASCII
    // closely enough for the CYD's built-in font.
    if (ch >= 0x21 && ch < 0x7F) {
        return (char)ch;
    }

    return ' ';
}

// Fix boot row from ">>[cursor]" to ">[cursor] "
static void atom_fix_screen_offsets()
{
    for (int row = 0; row < ATOM_ROWS; row++) {
        uint16_t base = ATOM_VRAM_START + row * ATOM_COLS;

        uint8_t c0 = fakemem[base + 0];
        uint8_t c1 = fakemem[base + 1];
        uint8_t c2 = fakemem[base + 2];

        // Boot row: ">>[cursor]" -> ">[cursor] "
        if (c0 == 0x3E && c1 == 0x3E && c2 == 0xA0) {
            fakemem[base + 1] = 0xA0;
            fakemem[base + 2] = 0x20;

            int idx = row * ATOM_COLS;
            last_vram[idx + 0] = 0xFF;
            last_vram[idx + 1] = 0xFF;
            last_vram[idx + 2] = 0xFF;
            return;
        }
    }
}

// ---------------- Video update ----------------

// Colour Mode 1 is selected by the Atom Floating Point ROM's COLOUR command
// after CLEAR 1.  It is a 64x64, four-colour bitmap using the same 1024-byte
// VRAM span as monochrome Mode 1: 16 bytes per row, four 2-bit pixels per byte.
// Pixel pairs are stored left-to-right in bits 7:6, 5:4, 3:2, 1:0.
//
// V062 adds the Atom's real MC6847 CSS selection.  On the Atom, 8255
// Port-C bit 3 ($B002 bit $08) is wired to the VDG CSS input.
//
// CSS=0: 00 green, 01 yellow, 10 blue,    11 red
// CSS=1: 00 buff,  01 cyan,   10 magenta, 11 orange
//
// "Buff" is represented by a warm off-white RGB value.  The exact analogue
// shade on a real TV depends on the colour encoder/display, but the logical
// MC6847 colour identity is preserved here.
static uint16_t atom_colour_palette(uint8_t pixel)
{
    const bool css = (fakemem[0xB002] & 0x08) != 0;

    if (!css) {
        switch (pixel & 0x03) {
            case 0: return TFT_GREEN;
            case 1: return TFT_YELLOW;
            case 2: return TFT_BLUE;
            default:return TFT_RED;
        }
    }

    switch (pixel & 0x03) {
        case 0: return tft.color565(255, 235, 180); // buff
        case 1: return TFT_CYAN;
        case 2: return TFT_MAGENTA;
        default:return TFT_ORANGE;
    }
}

static void atom_video_update_colour1()
{
    int panelW = tft.width();
    int panelH = tft.height();

    int scaleX = panelW / ATOM_VDG_NATIVE_W;
    int scaleY = panelH / ATOM_VDG_NATIVE_H;
    int scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale < 1) scale = 1;

    int outW = ATOM_VDG_NATIVE_W * scale;
    int outH = ATOM_VDG_NATIVE_H * scale;
    int x0 = (panelW - outW) / 2;
    int y0 = (panelH - outH) / 2;

    // 64x64 logical pixels expand to the native 256x192 raster.
    const int pixelW = 4 * scale;
    const int pixelH = 3 * scale;

    int idx = 0;
    for (int row = 0; row < ATOM_COLOUR1_HEIGHT; ++row) {
        uint16_t base = ATOM_COLOUR1_VRAM_START + row * ATOM_COLOUR1_BYTES_ROW;

        for (int bx = 0; bx < ATOM_COLOUR1_BYTES_ROW; ++bx, ++idx) {
            uint8_t value = fakemem[base + bx];
            if (value == last_colour1_vram[idx]) continue;
            last_colour1_vram[idx] = value;

            int logicalX0 = bx * 4;
            for (int px = 0; px < 4; ++px) {
                uint8_t colour = (value >> (6 - px * 2)) & 0x03;
                int x = x0 + (logicalX0 + px) * pixelW;
                int y = y0 + row * pixelH;
                tft.fillRect(x, y, pixelW, pixelH, atom_colour_palette(colour));
            }
        }
    }
}

// Colour Mode 2a: 128x64, four colours, 2 bpp.  The D-ROM extends the
// CLEAR 2 memory area through $87FF when COLOUR is selected.  Four pixels
// are packed per byte, and each logical pixel expands to 2x3 native pixels.
static void atom_video_update_colour2()
{
    int panelW = tft.width();
    int panelH = tft.height();
    int scaleX = panelW / ATOM_VDG_NATIVE_W;
    int scaleY = panelH / ATOM_VDG_NATIVE_H;
    int scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale < 1) scale = 1;

    int outW = ATOM_VDG_NATIVE_W * scale;
    int outH = ATOM_VDG_NATIVE_H * scale;
    int x0 = (panelW - outW) / 2;
    int y0 = (panelH - outH) / 2;
    const int pixelW = 2 * scale;
    const int pixelH = 3 * scale;

    int idx = 0;
    for (int row = 0; row < ATOM_COLOUR2_HEIGHT; ++row) {
        uint16_t base = ATOM_COLOUR2_VRAM_START + row * ATOM_COLOUR2_BYTES_ROW;
        for (int bx = 0; bx < ATOM_COLOUR2_BYTES_ROW; ++bx, ++idx) {
            uint8_t value = fakemem[base + bx];
            if (value == last_colour2_vram[idx]) continue;
            last_colour2_vram[idx] = value;
            int logicalX0 = bx * 4;
            for (int px = 0; px < 4; ++px) {
                uint8_t colour = (value >> (6 - px * 2)) & 0x03;
                int x = x0 + (logicalX0 + px) * pixelW;
                int y = y0 + row * pixelH;
                tft.fillRect(x, y, pixelW, pixelH, atom_colour_palette(colour));
            }
        }
    }
}

// Colour Mode 3a: 128x96, four colours, 2 bpp, $8000-$8BFF.
// Each logical pixel expands to 2x2 native pixels.
static void atom_video_update_colour3()
{
    int panelW = tft.width();
    int panelH = tft.height();
    int scaleX = panelW / ATOM_VDG_NATIVE_W;
    int scaleY = panelH / ATOM_VDG_NATIVE_H;
    int scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale < 1) scale = 1;

    int outW = ATOM_VDG_NATIVE_W * scale;
    int outH = ATOM_VDG_NATIVE_H * scale;
    int x0 = (panelW - outW) / 2;
    int y0 = (panelH - outH) / 2;
    const int pixelW = 2 * scale;
    const int pixelH = 2 * scale;

    int idx = 0;
    for (int row = 0; row < ATOM_COLOUR3_HEIGHT; ++row) {
        uint16_t base = ATOM_COLOUR3_VRAM_START + row * ATOM_COLOUR3_BYTES_ROW;
        for (int bx = 0; bx < ATOM_COLOUR3_BYTES_ROW; ++bx, ++idx) {
            uint8_t value = fakemem[base + bx];
            if (value == last_colour3_vram[idx]) continue;
            last_colour3_vram[idx] = value;
            int logicalX0 = bx * 4;
            for (int px = 0; px < 4; ++px) {
                uint8_t colour = (value >> (6 - px * 2)) & 0x03;
                int x = x0 + (logicalX0 + px) * pixelW;
                int y = y0 + row * pixelH;
                tft.fillRect(x, y, pixelW, pixelH, atom_colour_palette(colour));
            }
        }
    }
}

// Colour Mode 4a: 128x192, four colours, 2 bpp, $8000-$97FF.
// Each logical pixel expands to 2x1 native pixels.
static void atom_video_update_colour4()
{
    int panelW = tft.width();
    int panelH = tft.height();
    int scaleX = panelW / ATOM_VDG_NATIVE_W;
    int scaleY = panelH / ATOM_VDG_NATIVE_H;
    int scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale < 1) scale = 1;

    int outW = ATOM_VDG_NATIVE_W * scale;
    int outH = ATOM_VDG_NATIVE_H * scale;
    int x0 = (panelW - outW) / 2;
    int y0 = (panelH - outH) / 2;
    const int pixelW = 2 * scale;
    const int pixelH = 1 * scale;

    int idx = 0;
    for (int row = 0; row < ATOM_COLOUR4_HEIGHT; ++row) {
        uint16_t base = ATOM_COLOUR4_VRAM_START + row * ATOM_COLOUR4_BYTES_ROW;
        for (int bx = 0; bx < ATOM_COLOUR4_BYTES_ROW; ++bx, ++idx) {
            uint8_t value = fakemem[base + bx];
            if (value == last_colour4_vram[idx]) continue;
            last_colour4_vram[idx] = value;
            int logicalX0 = bx * 4;
            for (int px = 0; px < 4; ++px) {
                uint8_t colour = (value >> (6 - px * 2)) & 0x03;
                int x = x0 + (logicalX0 + px) * pixelW;
                int y = y0 + row * pixelH;
                tft.fillRect(x, y, pixelW, pixelH, atom_colour_palette(colour));
            }
        }
    }
}


// Mode 1 is the Atom/6847 128x64 one-bit graphics mode.  The ROM uses
// 16 bytes per row and 64 rows, so VRAM is $8000-$83FF.  Within each
// byte the leftmost pixel is bit 7.  The 6847 native raster expands each
// logical Mode-1 pixel to 2 horizontal x 3 vertical raster pixels, yielding
// a 256x192 active image.  We keep that native geometry and centre it on
// whatever TFT_eSPI panel is selected.
static void atom_video_update_mode1()
{
    int panelW = tft.width();
    int panelH = tft.height();

    // Integer scaling of the native 256x192 raster.  On the 320x240 CYD
    // this is exactly 1x (256x192 centred).  Larger displays can use 2x,
    // 3x, ... without changing the Atom/6847 emulation.
    int scaleX = panelW / ATOM_VDG_NATIVE_W;
    int scaleY = panelH / ATOM_VDG_NATIVE_H;
    int scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale < 1) scale = 1;

    int outW = ATOM_VDG_NATIVE_W * scale;
    int outH = ATOM_VDG_NATIVE_H * scale;
    int x0 = (panelW - outW) / 2;
    int y0 = (panelH - outH) / 2;

    const int pixelW = 2 * scale;
    const int pixelH = 3 * scale;

    int idx = 0;
    for (int row = 0; row < ATOM_MODE1_HEIGHT; ++row) {
        uint16_t base = ATOM_MODE1_VRAM_START + row * ATOM_MODE1_BYTES_ROW;

        for (int bx = 0; bx < ATOM_MODE1_BYTES_ROW; ++bx, ++idx) {
            uint8_t value = fakemem[base + bx];
            if (value == last_mode1_vram[idx]) continue;
            last_mode1_vram[idx] = value;

            int logicalX0 = bx * 8;
            for (int bit = 0; bit < 8; ++bit) {
                bool on = (value & (0x80 >> bit)) != 0;
                int x = x0 + (logicalX0 + bit) * pixelW;
                int y = y0 + row * pixelH;
                tft.fillRect(x, y, pixelW, pixelH, on ? TFT_GREEN : TFT_BLACK);
            }
        }
    }
}

// Mode 2 is the Atom/6847 128x96 one-bit graphics mode.  It keeps the
// same 16 bytes/row and bit ordering as Mode 1, but uses 96 rows, so VRAM
// is $8000-$85FF.  Each logical pixel maps to 2x2 native 6847 raster
// pixels, yielding the same 256x192 active image as Mode 1.
static void atom_video_update_mode2()
{
    int panelW = tft.width();
    int panelH = tft.height();

    int scaleX = panelW / ATOM_VDG_NATIVE_W;
    int scaleY = panelH / ATOM_VDG_NATIVE_H;
    int scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale < 1) scale = 1;

    int outW = ATOM_VDG_NATIVE_W * scale;
    int outH = ATOM_VDG_NATIVE_H * scale;
    int x0 = (panelW - outW) / 2;
    int y0 = (panelH - outH) / 2;

    const int pixelW = 2 * scale;
    const int pixelH = 2 * scale;

    int idx = 0;
    for (int row = 0; row < ATOM_MODE2_HEIGHT; ++row) {
        uint16_t base = ATOM_MODE2_VRAM_START + row * ATOM_MODE2_BYTES_ROW;

        for (int bx = 0; bx < ATOM_MODE2_BYTES_ROW; ++bx, ++idx) {
            uint8_t value = fakemem[base + bx];
            if (value == last_mode2_vram[idx]) continue;
            last_mode2_vram[idx] = value;

            int logicalX0 = bx * 8;
            for (int bit = 0; bit < 8; ++bit) {
                bool on = (value & (0x80 >> bit)) != 0;
                int x = x0 + (logicalX0 + bit) * pixelW;
                int y = y0 + row * pixelH;
                tft.fillRect(x, y, pixelW, pixelH, on ? TFT_GREEN : TFT_BLACK);
            }
        }
    }
}


// Mode 3 is the Atom/6847 128x192 one-bit graphics mode.  It keeps the
// same 16 bytes/row and bit ordering as Modes 1 and 2, but uses all 192
// native raster rows, so VRAM is $8000-$8BFF.  Each logical pixel maps
// to 2x1 native 6847 raster pixels, yielding a 256x192 active image.
static void atom_video_update_mode3()
{
    int panelW = tft.width();
    int panelH = tft.height();

    int scaleX = panelW / ATOM_VDG_NATIVE_W;
    int scaleY = panelH / ATOM_VDG_NATIVE_H;
    int scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale < 1) scale = 1;

    int outW = ATOM_VDG_NATIVE_W * scale;
    int outH = ATOM_VDG_NATIVE_H * scale;
    int x0 = (panelW - outW) / 2;
    int y0 = (panelH - outH) / 2;

    const int pixelW = 2 * scale;
    const int pixelH = 1 * scale;

    int idx = 0;
    for (int row = 0; row < ATOM_MODE3_HEIGHT; ++row) {
        uint16_t base = ATOM_MODE3_VRAM_START + row * ATOM_MODE3_BYTES_ROW;

        for (int bx = 0; bx < ATOM_MODE3_BYTES_ROW; ++bx, ++idx) {
            uint8_t value = fakemem[base + bx];
            if (value == last_mode3_vram[idx]) continue;
            last_mode3_vram[idx] = value;

            int logicalX0 = bx * 8;
            for (int bit = 0; bit < 8; ++bit) {
                bool on = (value & (0x80 >> bit)) != 0;
                int x = x0 + (logicalX0 + bit) * pixelW;
                int y = y0 + row * pixelH;
                tft.fillRect(x, y, pixelW, pixelH, on ? TFT_GREEN : TFT_BLACK);
            }
        }
    }
}

// Mode 4 is the Atom/6847 256x192 one-bit graphics mode.  It uses
// 32 bytes per row and all 192 native raster rows, so VRAM is
// $8000-$97FF.  This already matches the 6847 256x192 active raster,
// therefore each logical pixel maps directly to one native raster pixel.
static void atom_video_update_mode4()
{
    int panelW = tft.width();
    int panelH = tft.height();

    int scaleX = panelW / ATOM_VDG_NATIVE_W;
    int scaleY = panelH / ATOM_VDG_NATIVE_H;
    int scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale < 1) scale = 1;

    int outW = ATOM_VDG_NATIVE_W * scale;
    int outH = ATOM_VDG_NATIVE_H * scale;
    int x0 = (panelW - outW) / 2;
    int y0 = (panelH - outH) / 2;

    const int pixelW = 1 * scale;
    const int pixelH = 1 * scale;

    int idx = 0;
    for (int row = 0; row < ATOM_MODE4_HEIGHT; ++row) {
        uint16_t base = ATOM_MODE4_VRAM_START + row * ATOM_MODE4_BYTES_ROW;

        for (int bx = 0; bx < ATOM_MODE4_BYTES_ROW; ++bx, ++idx) {
            uint8_t value = fakemem[base + bx];
            if (value == last_mode4_vram[idx]) continue;
            last_mode4_vram[idx] = value;

            int logicalX0 = bx * 8;
            for (int bit = 0; bit < 8; ++bit) {
                bool on = (value & (0x80 >> bit)) != 0;
                int x = x0 + (logicalX0 + bit) * pixelW;
                int y = y0 + row * pixelH;
                tft.fillRect(x, y, pixelW, pixelH, on ? TFT_GREEN : TFT_BLACK);
            }
        }
    }
}

// Mode 0 semigraphics uses one 2x3 pixel block per text cell.  The Atom
// ROM keeps bit 6 set to select semigraphics and uses bits 5..0 as the six
// pixels in display order:
//
//   bit 5  bit 4     top row
//   bit 3  bit 2     middle row
//   bit 1  bit 0     bottom row
//
// CLEAR 0 fills VRAM with $40, i.e. semigraphics selected with all six
// pixels clear.  V060 deliberately renders these blocks monochrome; colour
// attributes are left for the next stage.
static void atom_draw_mode0_semigraphics_cell(int x, int y, uint8_t raw,
                                                int cellW, int cellH)
{
    const uint8_t pattern = raw & 0x3F;

    // Split the full host-side character cell into the Atom's genuine 2x3
    // semigraphics blocks.  This changes only presentation; the emulated
    // Mode-0 byte and video RAM layout remain exactly 32x16.
    for (int sy = 0; sy < 3; ++sy) {
        int y1 = y + (sy * cellH) / 3;
        int y2 = y + ((sy + 1) * cellH) / 3;

        for (int sx = 0; sx < 2; ++sx) {
            int x1 = x + (sx * cellW) / 2;
            int x2 = x + ((sx + 1) * cellW) / 2;
            int bit = 5 - (sy * 2 + sx);

            if (pattern & (1u << bit)) {
                tft.fillRect(x1, y1, x2 - x1, y2 - y1, TFT_GREEN);
            }
        }
    }
}

static void atom_video_update_mode0()
{
    atom_fix_screen_offsets();

    int idx = 0;

    // V111q: keep the proven top and left placement from V111o,
    // but tighten the horizontal character spacing and deliberately leave
    // a larger unused area on the right.
    //
    // Host Mode-0 geometry on the 800x480 LCD:
    //     left margin   = 24 px
    //     top margin    = 24 px
    //     text width    = 640 px
    //     text height   = 432 px
    //     cell size     = 20 x 27 px
    //     right margin  = 136 px
    //     bottom margin = 24 px
    //
    // The font remains exactly 3x3 (nominal 18x24 glyph), so reducing the
    // cell width to 20 px leaves only about 2 px between adjacent glyphs.
    // Atom video RAM and its genuine 32x16 layout are unchanged.
    const int wantedW = 640;
    const int wantedH = 432;
    const int textW = (tft.width() >= wantedW + 24)
                        ? wantedW
                        : (tft.width() / ATOM_COLS) * ATOM_COLS;
    const int textH = (tft.height() >= wantedH + 24)
                        ? wantedH
                        : (tft.height() / ATOM_ROWS) * ATOM_ROWS;
    const int cellW = textW / ATOM_COLS;     // 20 px on 800x480
    const int cellH = textH / ATOM_ROWS;     // 27 px on 800x480
    const int x0 = (tft.width() >= wantedW + 24) ? 24 : (tft.width() - textW) / 2;
    const int y0 = (tft.height() >= wantedH + 24) ? 24 : (tft.height() - textH) / 2;

    // Font 1 is nominally 6x8.  Keep the V111n/V111o 3x3 font scaling.
    int fontScaleX = cellW / 6;
    int fontScaleY = cellH / 8;
    if (fontScaleX < 1) fontScaleX = 1;
    if (fontScaleY < 1) fontScaleY = 1;

    const int glyphH = 8 * fontScaleY;
    const int glyphYOffset = (cellH - glyphH) / 2;

    tft.setTextFont(1);
    tft.setTextSize(fontScaleX, fontScaleY);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);

    for (int row = 0; row < ATOM_ROWS; row++) {
        uint16_t rowBase = ATOM_VRAM_START + row * ATOM_COLS;

        bool promptCursorRow =
            (fakemem[rowBase + 0] == 0x3E && fakemem[rowBase + 1] == 0xA0);

        int numericShift = 0;
        if (!promptCursorRow) {
            int firstVisible = 0;
            while (firstVisible < ATOM_COLS) {
                uint8_t raw = fakemem[rowBase + firstVisible];
                if (raw != 0x20 && raw != 0xA0) break;
                firstVisible++;
            }

            if (firstVisible < ATOM_COLS) {
                uint8_t raw = fakemem[rowBase + firstVisible];
                if (raw >= '0' && raw <= '9') {
                    numericShift = firstVisible;
                }
            }
        }

        for (int col = 0; col < ATOM_COLS; col++) {
            int srcCol;

            if (promptCursorRow) {
                if (col == 0) {
                    srcCol = 0;       // keep the '>'
                } else {
                    srcCol = col + 1; // skip cursor at source col 1
                }
            } else {
                srcCol = col + numericShift;
            }

            uint8_t raw = 0x20;
            char out = ' ';
            if (srcCol < ATOM_COLS) {
                raw = fakemem[rowBase + srcCol];
                out = atom_char_to_ascii(raw);
            }

            uint8_t cacheCode = raw & 0x7F;
            if (cacheCode == last_vram[idx]) {
                idx++;
                continue;
            }
            last_vram[idx] = cacheCode;

            int x = x0 + col * cellW;
            int y = y0 + row * cellH;

            // Clear the complete host cell so old glyph/semigraphics data
            // cannot remain when a character changes.
            tft.fillRect(x, y, cellW, cellH, TFT_BLACK);

            uint8_t code = raw & 0x7F;
            if (code & 0x40) {
                atom_draw_mode0_semigraphics_cell(x, y, code, cellW, cellH);
            } else if (code == 0x1E) {
                // Atom up-arrow, scaled directly into the host cell.
                const int cx = x + cellW / 2;
                const int top = y + cellH / 6;
                const int bottom = y + (cellH * 5) / 6;
                const int stemW = (cellW >= 20) ? 3 : 1;
                tft.fillRect(cx - stemW / 2, top + cellH / 5,
                             stemW, bottom - (top + cellH / 5), TFT_GREEN);

                const int headH = cellH / 4;
                for (int i = 0; i < headH; ++i) {
                    int half = (i * cellW / 3) / headH;
                    tft.fillRect(cx - half, top + i, half * 2 + 1, 1, TFT_GREEN);
                }
            } else {
                char cell[2] = { out, 0 };
                tft.setCursor(x, y + glyphYOffset);
                tft.print(cell);
            }

            idx++;
        }
    }
}

void atom_video_update()
{
    // The low nibble of Port A is continuously used for keyboard-row scans.
    // Only the high nibble selects the VDG mode.
    uint8_t mode = atom_last_b000_write & 0xF0;
    uint8_t css = (fakemem[0xB002] & 0x08) ? 1u : 0u;

    // The Atom wires 8255 Port-C bit 3 to the MC6847 CSS input.  CSS can
    // therefore change without changing the VDG mode byte.  Force the
    // currently selected genuine Atom colour mode to redraw when it changes.
    bool colourMode = (mode == 0x10 || mode == 0x50 || mode == 0x90 || mode == 0xD0);
    if (colourMode && css != last_colour_css) {
        last_colour_css = css;
        if (mode == 0x10) memset(last_colour1_vram, 0xFF, sizeof(last_colour1_vram));
        if (mode == 0x50) memset(last_colour2_vram, 0xFF, sizeof(last_colour2_vram));
        if (mode == 0x90) memset(last_colour3_vram, 0xFF, sizeof(last_colour3_vram));
        if (mode == 0xD0) memset(last_colour4_vram, 0xFF, sizeof(last_colour4_vram));
    }

    if (mode != last_video_mode) {
        last_video_mode = mode;
        tft.fillScreen(TFT_BLACK);

        // Force Mode 0 to redraw all text cells.  Graphics modes start from
        // a freshly black TFT, so treating zero VRAM bytes as already drawn
        // avoids thousands of unnecessary black pixel writes after CLEAR;
        // any non-zero graphics bytes are still drawn immediately.
        memset(last_vram, 0xFF, sizeof(last_vram));
        memset(last_mode1_vram, 0x00, sizeof(last_mode1_vram));
        memset(last_mode2_vram, 0x00, sizeof(last_mode2_vram));
        memset(last_mode3_vram, 0x00, sizeof(last_mode3_vram));
        memset(last_mode4_vram, 0x00, sizeof(last_mode4_vram));
        // Colour value 0 is a real colour, not black, so force a complete
        // redraw whenever any Atom colour mode is entered.
        memset(last_colour1_vram, 0xFF, sizeof(last_colour1_vram));
        memset(last_colour2_vram, 0xFF, sizeof(last_colour2_vram));
        memset(last_colour3_vram, 0xFF, sizeof(last_colour3_vram));
        memset(last_colour4_vram, 0xFF, sizeof(last_colour4_vram));
        if (colourMode) last_colour_css = css;
    }

    if (mode == 0x10) {
        atom_video_update_colour1();
    } else if (mode == 0x50) {
        atom_video_update_colour2();
    } else if (mode == 0x90) {
        atom_video_update_colour3();
    } else if (mode == 0xD0) {
        atom_video_update_colour4();
    } else if (mode == 0x30) {
        atom_video_update_mode1();
    } else if (mode == 0x70) {
        atom_video_update_mode2();
    } else if (mode == 0xB0) {
        atom_video_update_mode3();
    } else if (mode == 0xF0) {
        atom_video_update_mode4();
    } else if (mode == 0x00) {
        // Preserve the proven V055 Mode-0 renderer exactly.
        atom_video_update_mode0();
    } else {
        // Unknown/unsupported VDG state.  The screen was already blanked
        // when the mode changed; do not reinterpret bitmap memory as text.
    }
}

// Debug scan: find "ACORN ATOM" in RAM
void atom_scan_for_banner()
{
    const uint8_t pattern[] = { 0x01, 0x03, 0x0F, 0x12, 0x0E, 0x20, 0x01, 0x14, 0x0F, 0x0D };
    const size_t  plen      = sizeof(pattern);

    for (uint16_t addr = 0x0000; addr < 0xC000; ++addr) {
        bool match = true;
        for (size_t i = 0; i < plen; ++i) {
            if (fakemem[addr + i] != pattern[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            Serial.printf("[ATOM] Found 'ACORN ATOM' pattern at $%04X\n", addr);

            // V131: present the enhanced machine simply as "ACORN ATOM+".
            // ATOMMC2 has already appended " + ATOMMC2" to the Kernel banner.
            // Preserve the actual '+' screen code by moving it left over the
            // intervening space, then erase only the old suffix.
            fakemem[addr + 10] = fakemem[addr + 11];
            for (uint16_t i = 11; i <= 19; ++i) fakemem[addr + i] = 0x20;
            return;
        }
    }
    Serial.println("[ATOM] Banner pattern not found in RAM.");
}

static void dump_vram_rows(int first_row, int row_count)
{
    for (int row = first_row; row < first_row + row_count && row < ATOM_ROWS; row++) {
        uint16_t base = ATOM_VRAM_START + row * ATOM_COLS;
        Serial.printf("[VRAM row %02d @ %04X] ", row, base);
        for (int col = 0; col < ATOM_COLS; col++) {
            Serial.printf("%02X ", fakemem[base + col]);
        }
        Serial.println();
    }
}

static void dump_vram_rows_ascii(int first_row, int row_count)
{
    for (int row = first_row; row < first_row + row_count && row < ATOM_ROWS; row++) {
        uint16_t base = ATOM_VRAM_START + row * ATOM_COLS;

        Serial.printf("[VRAM ascii row %02d] ", row);
        for (int col = 0; col < ATOM_COLS; col++) {
            char out = atom_char_to_ascii(fakemem[base + col]);
            if (out < 32 || out > 126) out = '.';
            Serial.print(out);
        }
        Serial.println();
    }
}

extern "C" void atom_dump_vram_top()
{
    dump_vram_rows(0, 12);
}

extern "C" void atom_dump_vram_ascii_top()
{
    dump_vram_rows_ascii(0, 12);
}
