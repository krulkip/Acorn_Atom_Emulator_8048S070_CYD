//-----------------------------------------------------------------------------
// fakemem.c
// Instrumented Atom 8255 / keyboard bring-up
//-----------------------------------------------------------------------------

#include "fakemem.h"
#include "atommc_stub.h"
#include "fake6522.h"
#include "atom_kbd.h"
#include "atom_hw_kbd.h"
#include "atom_serial.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

extern uint8_t irq_mode;

//-----------------------------------------------------------------------------
uint8_t fakemem[1<<16];

// Board-side audio hook implemented by the Arduino sketch.
// The real Atom loudspeaker is 8255 Port-C bit 2.
extern void atom_audio_set_level(uint8_t level);
extern void atom_v085_fire_b002_read(uint16_t pc, uint8_t value);
extern void atom_v085_fire_b002_write(uint16_t pc, uint8_t oldValue, uint8_t newValue);

// V066 cassette hardware hooks implemented by the Arduino sketch.
extern uint8_t atom_tape_clock_level(void);  // 8255 PC4, free-running 2.4 kHz
extern uint8_t atom_tape_input_level(void);  // 8255 PC5, cassette input
extern uint8_t atom_tape_fast_available(void);
extern uint8_t atom_tape_fast_get_byte(uint8_t *out);
extern uint8_t atom_cat_fast_end_pending(void);
extern void atom_tape_rewind(void);
extern uint8_t atom_tape_auto_mount_load(uint8_t x);

// V067 output-side cassette hooks.
extern void atom_tape_save_begin(void);
extern uint8_t atom_tape_fast_save_put(uint8_t b);
extern uint8_t atom_tape_fast_ossave(uint8_t x);
extern void atom_tape_output_portc(uint8_t value, uint16_t next_pc);
extern void atom_cat_rom_enter(void);
extern void atom_cat_rom_exit(void);
extern uint8_t atom_mount_try_command(void);
extern void atom_cat_preprocess_oscli(void);
extern uint16_t *fake6502_pc;
extern uint8_t *fake6502_a;
extern uint8_t *fake6502_x;
extern uint8_t *fake6502_status;

static uint8_t atom_speaker_level = 0xFFu;

static void atom_sync_speaker_from_portc(void)
{
    uint8_t level = (fakemem[0xB002] & 0x04u) ? 1u : 0u;
    if (level != atom_speaker_level) {
        atom_speaker_level = level;
        atom_audio_set_level(level);
    }
}

uint16_t fakemem_access_address;
uint8_t fakemem_access_data;
uint8_t fakemem_access_mode;

uint16_t last_read_addr = 0;
uint8_t  last_read_value = 0;

// Last Port-A write is functional state: the video renderer uses its
// upper nibble to determine the current 6847 graphics mode.
uint8_t atom_last_b000_write = 0;

// -----------------------------------------------------------------------------
// ASBK-style #A000 utility-ROM banking.
//
// The original switch card selects one of eight 4 KB EPROMs with a write to
// $BFFF.  The ASBK switching software separately stores the selected bank in
// ordinary RAM at $9FFF, so $9FFF deliberately remains normal emulated RAM.
//
// For CPU performance the selected bank is copied into the normal fakemem
// $A000-$AFFF window only when the bank changes.  6502 reads therefore retain
// V113's existing fast ordinary-memory path with no per-read banking overhead.
// -----------------------------------------------------------------------------
static uint8_t atom_arom_banks[ATOM_AROM_BANK_COUNT][0x1000];
static uint8_t atom_arom_loaded[ATOM_AROM_BANK_COUNT];
uint8_t atom_arom_bank = 0;

void atom_arom_clear_banks(void)
{
    memset(atom_arom_banks, 0xFF, sizeof(atom_arom_banks));
    memset(atom_arom_loaded, 0, sizeof(atom_arom_loaded));
    atom_arom_bank = 0;
}

uint8_t *atom_arom_bank_ptr(uint8_t bank)
{
    if (bank >= ATOM_AROM_BANK_COUNT)
        return NULL;
    return atom_arom_banks[bank];
}

void atom_arom_mark_loaded(uint8_t bank, uint8_t loaded)
{
    if (bank < ATOM_AROM_BANK_COUNT)
        atom_arom_loaded[bank] = loaded ? 1u : 0u;
}

uint8_t atom_arom_is_loaded(uint8_t bank)
{
    if (bank >= ATOM_AROM_BANK_COUNT)
        return 0;
    return atom_arom_loaded[bank];
}

void atom_arom_select(uint8_t bank)
{
    // Original ASBK software cycles bank values 0..7.
    bank &= 0x07u;
    atom_arom_bank = bank;
    memcpy(&fakemem[0xA000], atom_arom_banks[bank], 0x1000);
}

//-----------------------------------------------------------------------------
// Initialize the 6502 Memory

void fakemem_init(uint16_t reset_vector_unused)
{
    // Atom handles its own vectors from ROM — do nothing here.
}

//-----------------------------------------------------------------------------
uint8_t read6502(uint16_t addr)
{
    static unsigned erom_game_reads = 0;
    if (addr >= 0xE000u && addr <= 0xEFFFu && fake6502_pc &&
        *fake6502_pc >= 0x2900u && *fake6502_pc < 0x4000u &&
        erom_game_reads < 32u) {
        printf("[EROM-GAME] PC=$%04X read $%04X\n", *fake6502_pc, addr);
        ++erom_game_reads;
    }
    /*
     * V079 page-dispatched fast path.
     *
     * Nearly every 6502 opcode/operand/RAM access is ordinary memory.  The
     * previous implementation made every such read evaluate the complete
     * cassette/COS/VIA/serial/8255 hook chain.  Dispatch first by page so
     * ordinary memory returns immediately.
     */

    uint8_t page = (uint8_t)(addr >> 8);

    // $0000-$5FFF: ordinary RAM.
    if (page < 0x60)
        return fakemem[addr];

    // V166: $6000-$60FF is ordinary memory for both Atom and BBC BASIC.
    // The 6522 VIA has been moved to the real Acorn Atom address $B800.
    if (page == 0x60)
        return fakemem[addr];

    // $6100-$AFFF: ordinary memory, except no hooks live here.
    if (page < 0xB0)
        return fakemem[addr];

    // $B000 page contains the Atom 8255 at $B000-$B003.
    if (page == 0xB0) {
        if ((addr & 0xFFFCu) == 0xB000u) {
            switch ((uint8_t)(addr & 3u)) {
                case 0:
                    return fakemem[0xB000];

                case 1: {
                    uint8_t v = atom_kbd_read_port_b();
                    return v;
                }

                case 2: {
                    extern uint32_t clockticks6502;
                    const uint32_t frame_cycles = 16667u;
                    const uint32_t vbl_cycles   = 2048u;
                    uint32_t phase = clockticks6502 % frame_cycles;
                    uint8_t vbl_bit = (phase < vbl_cycles) ? 0x00u : 0x80u;
                    uint8_t tape_clock =
                        atom_tape_clock_level() ? 0x10u : 0x00u;
                    uint8_t tape_input =
                        atom_tape_input_level() ? 0x20u : 0x00u;

                    // V165: restore V141 physical REPT on 8255 PC6, active low.
                    uint8_t rept_bit = atom_hw_kbd_rept_pressed() ? 0x00u : 0x40u;

                    uint8_t v = (uint8_t)(vbl_bit | rept_bit |
                                          tape_clock | tape_input |
                                          (fakemem[0xB002] & 0x0Fu));
                    if (*fake6502_pc >= 0x356Cu && *fake6502_pc <= 0x3572u)
                        atom_v085_fire_b002_read(*fake6502_pc, v);
                    return v;
                }

                case 3:
                default:
                    return fakemem[0xB003];
            }
        }

        // Rest of $B000-$B0FF is ordinary mapped memory.
        return fakemem[addr];
    }

    // V116 Step 3: AtoMMC2 controller register window at $B400-$B404.
    if (addr >= 0xB400 && addr <= 0xB404)
        return atommmc_stub_read(addr);

    // V166: real Acorn Atom 6522 VIA register block at $B800-$B80F.
    if (addr >= 0xB800u && addr <= 0xB80Fu)
        return fake6522_read(addr);

    // $B100-$BEFF: ordinary memory apart from the hooks above.
    if (page < 0xBF)
        return fakemem[addr];

    // $BF00-$BFFF: only BFF0/BFF1 are emulator serial hooks.
    if (page == 0xBF) {
        if (addr == 0xBFF0)
            return atom_serial_avail();
        if (addr == 0xBFF1)
            return atom_serial_pop();
        return fakemem[addr];
    }

    // $C000-$F8FF: BASIC/FP/Kernel ROM with no read hooks.
    if (page < 0xF9)
        return fakemem[addr];

    /*
     * Only $F9xx-$FBxx contain the cassette/COS hooks introduced by the
     * emulator.  Restrict all expensive tests to those three pages.
     */
    if (page <= 0xFB) {
        uint8_t return_data = fakemem[addr];

        // V073 fast virtual cassette leader skip.
        if (addr == 0xFBA0 && atom_tape_fast_available() &&
            !atom_cat_fast_end_pending()) {
            *fake6502_status &= (uint8_t)~0x80u;
        }

        // V073d deterministic end of fast ROM-native *CAT.
        if (addr == 0xFB8E && atom_cat_fast_end_pending()) {
            *fake6502_status &= (uint8_t)~0x40u;
            return_data = 0x60;
        }

        // V073 fast OSBGET byte injection.
        if (addr == 0xFBEE && atom_tape_fast_available()) {
            uint8_t b;
            if (atom_tape_fast_get_byte(&b)) {
                *fake6502_a = b;
                fakemem[0x00DC] = (uint8_t)(fakemem[0x00DC] + b);
                return_data = 0x60;
            }
        }

        // V105b: $F96E is the genuine Atom cassette OSLOAD routine entry.
        // The documented interface guarantees that X points to the valid
        // five-byte zero-page LOAD control block here; 0,X/1,X point to the
        // CR-terminated filename.  Auto-mount only if a matching ATM exists.
        // atomTapeBuildFromAtm() rewinds a successful new mount itself, so
        // do NOT rewind the old/current cassette when lookup fails.
        if (addr == 0xF96E) {
            atom_tape_auto_mount_load(*fake6502_x);
        }

        // V104a true Turbo SAVE: OSSAVE receives the complete file control
        // block via X. If direct SD save succeeds (or was handled), substitute
        // RTS and skip the cassette prompt/leader/modulation path entirely.
        // If direct save cannot start, retain the proven V067 physical SAVE
        // capture as a fallback.
        if (addr == 0xFAE5) {
            if (atom_tape_fast_ossave(*fake6502_x))
                return_data = 0x60;
            else
                atom_tape_save_begin();
        }

        if (addr == 0xFA2A)
            atom_cat_rom_enter();

        if (addr == 0xFA36)
            atom_cat_rom_exit();

        if (addr == 0xF926 && atom_mount_try_command())
            return_data = 0x60;

        return return_data;
    }

    // $FC00-$FFFF: otherwise ordinary Kernel ROM. This includes the hot
    // FE7x-FE9x keyboard polling code, which returns directly.
    //
    // V109d: $FFF7 is the documented OSCLI entry.  Preprocess only our
    // directory extension before the genuine ROM command interpreter sees
    // the command buffer.  The opcode at $FFF7 itself remains untouched.
    if (addr == 0xFFF7)
        atom_cat_preprocess_oscli();

    return fakemem[addr];
}

//-----------------------------------------------------------------------------
void write6502(uint16_t addr, uint8_t byte)
{
    static unsigned erom_game_writes = 0;
    if (addr >= 0xE000u && addr <= 0xEFFFu && fake6502_pc &&
        *fake6502_pc >= 0x2900u && *fake6502_pc < 0x4000u &&
        erom_game_writes < 32u) {
        printf("[EROM-GAME-WR] PC=$%04X write $%04X=$%02X\n", *fake6502_pc, addr, byte);
        ++erom_game_writes;
    }
    //
    // V166: $6000-$60FF is ordinary memory.  The 6522 is at $B800-$B80F.

    //
    // --- Atom 8255 / keyboard block at $B000-$B003 ---
    //
    if ((addr & 0xFFFC) == 0xB000) {
        uint8_t reg = (uint8_t)(addr & 0x0003);

        switch (reg) {
            case 0: // Port A row select
                fakemem[0xB000] = byte;
                atom_kbd_write_port_a(byte);

                atom_last_b000_write = byte;
                break;

            case 1: // Port B input on real Atom; ignore writes
                break;

            case 2: { // Port C data latch
                uint8_t old_portc = fakemem[0xB002];
                fakemem[0xB002] = byte;
                if (*fake6502_pc >= 0x3571u && *fake6502_pc <= 0x3578u)
                    atom_v085_fire_b002_write(*fake6502_pc, old_portc, byte);
                atom_sync_speaker_from_portc();
                atom_tape_output_portc(byte, *fake6502_pc);
                break;
            }

            case 3: // 8255 control register / bit set-reset (BSR)
                fakemem[0xB003] = byte;


                /*
                 * 8255 BSR mode: D7=0, D3..D1 select Port-C bit, D0
                 * selects SET(1) or RESET(0).  The Atom Kernel BEL routine
                 * writes $05/$04 here, which sets/resets PC2 and therefore
                 * drives the real loudspeaker.  PC3 is the VDG CSS line, so
                 * implementing BSR generically also preserves genuine Atom
                 * behaviour for that output.
                 */
                if ((byte & 0x80u) == 0) {
                    uint8_t bit = (uint8_t)((byte >> 1) & 0x07u);
                    uint8_t mask = (uint8_t)(1u << bit);
                    if (byte & 0x01u)
                        fakemem[0xB002] |= mask;
                    else
                        fakemem[0xB002] &= (uint8_t)~mask;

                    if (bit == 2u)
                        atom_sync_speaker_from_portc();
                }
                break;

            default:
                break;
        }

        return;
    }

    // V116 Step 3: AtoMMC2 controller register window at $B400-$B404.
    if (addr >= 0xB400 && addr <= 0xB404) {
        atommmc_stub_write(addr, byte);
        return;
    }

    // V166: real Acorn Atom 6522 VIA register block at $B800-$B80F.
    if (addr >= 0xB800u && addr <= 0xB80Fu) {
        fake6522_write(addr, byte);
        return;
    }

    // ASBK switch-card bank select.  A write to $BFFF selects EPROM A0..A7
    // into the $A000-$AFFF window.  Do NOT alter $9FFF here: the original
    // ASBK switching software maintains that RAM byte itself.
    if (addr == 0xBFFF) {
        atom_arom_select(byte);
        // Keep the last value visible in backing memory for diagnostics only.
        fakemem[0xBFFF] = byte;
        return;
    }

    fakemem[addr] = byte;
}

//-----------------------------------------------------------------------------
