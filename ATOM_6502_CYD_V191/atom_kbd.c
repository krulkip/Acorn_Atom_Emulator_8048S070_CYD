// atom_kbd.c - Acorn Atom 8255 keyboard matrix emulation
// V041: matrix derived from Atom Kernel ROM scan at FE71 and checked
// against Atomulator's 8255 implementation.

#include <stdint.h>
#include "atom_kbd.h"
#include "fakemem.h"
#include "atom_hw_kbd.h"

// fake6502 register pointers; used only to distinguish the Atom ROM
// release scan (FE9F -> FE71) from the new-key scan (FEA7 -> FE71).
extern uint8_t *fake6502_sp;
extern uint8_t *fake6502_a;
extern uint16_t *fake6502_pc;
extern uint32_t clockticks6502;


#define KBD_FIFO_SIZE 64
#define KEY_SHIFT     0x0100u
#define KEY_CTRL      0x0200u
#define KEY_DIRECT    0x0400u  // V109: mapped serial keys may be claimed by direct 8255/FE71 scans
#define KEY_CODE_MASK 0x003Fu

static volatile uint16_t kbd_fifo[KBD_FIFO_SIZE];
static volatile uint8_t kbd_head = 0;
static volatile uint8_t kbd_tail = 0;

static volatile uint8_t current_row = 255;
static volatile uint8_t current_row_raw = 0;

// Active emulated physical key.
static int8_t  kbd_row = -1;
static uint8_t kbd_col = 0;
static uint8_t kbd_shift = 0;
static uint8_t kbd_ctrl = 0;
static volatile uint8_t forced_ctrl = 0;
static uint8_t hold_matches = 0; // retained for bootstrap only
static uint8_t queued_key_pending = 0;

// Host ESC is a real physical Atom matrix key at row 0 / column 5.
// BASIC polls this contact directly at C504 while programs are running,
// without calling OSRDCH, so ESC must be able to assert independently
// of the normal queued-key acquisition state machine.
static uint8_t immediate_escape = 0;

// V109 automatic direct-scanner held-key approximation.
//
// A serial terminal supplies characters, not key-down/key-up events.  Any
// host character that maps to an Atom matrix key therefore remains an ordinary
// FIFO character
// until a program actually scans its physical Atom matrix row.  At that
// point the direct scanner takes ownership: the FIFO entry is removed
// immediately and a short physical contact is asserted.  Subsequent host
// auto-repeat copies of that SAME key refresh the physical held state instead
// of entering the FIFO.  When repeats stop, the contact expires automatically.
//
// This gives direct-scanning games a held-key approximation without leaving
// queued movement/fire behind after release, while BASIC/OSRDCH continues to
// receive completely normal serial typing and repeat behaviour.
static volatile uint8_t  direct_key_owned = 0;
static volatile uint8_t  direct_key_active = 0;
static volatile uint8_t  direct_key_keynum = 0;
static volatile int8_t   direct_key_row = -1;
static volatile uint8_t  direct_key_col = 0;
static volatile uint8_t  direct_key_shift = 0;
static volatile uint8_t  direct_key_ctrl = 0;
static volatile uint32_t direct_key_until = 0;
static volatile uint32_t direct_repeat_window_until = 0;

// A quick tap should stay short; 20 ms tested correctly in V101a.
#define DIRECT_FIRST_PRESS_CYCLES   20000u
// Exact V101c timings: once terminal auto-repeat has begun, refresh enough
// to bridge normal repeat intervals, but release promptly after the final
// repeat character.
#define DIRECT_REPEAT_HOLD_CYCLES   70000u
// Keep ownership through the host terminal's delayed first auto-repeat. The
// physical contact itself is not held for this full period.
#define DIRECT_REPEAT_WINDOW_CYCLES 700000u

// V110: external callers of the Kernel FE71 scan want a discrete keypress,
// not V101c held-key ownership.  FE71 itself scans the physical matrix, so
// present exactly one FIFO-head key during that call and consume it only
// after FE71 has successfully acquired it at FEB1.
static volatile uint8_t external_fe71_pending = 0;
static volatile int8_t  external_fe71_row = -1;
static volatile uint8_t external_fe71_col = 0;
static volatile uint8_t external_fe71_shift = 0;
static volatile uint8_t external_fe71_ctrl = 0;
// V190 source: 0=none, 1=serial FIFO, 2=physical recent-key latch.
static volatile uint8_t external_fe71_source = 0;

// After a key is released the real OS must observe one complete scan with
// no key down before the next queued key may appear. FE71 performs 60 B001
// reads (10 rows x 6 columns) on a no-key scan.
static uint8_t release_gap_reads = 0;

// old force-anykey support retained for baseline diagnostics
static uint16_t force_press_reads = 0;

// bootstrap key support retained from V038
static int8_t  boot_row = -1;
static uint8_t boot_col = 0;
static uint8_t boot_matches = 0;
static uint8_t bootstrap_mode = 0;

static int fifo_empty(void) {
    return (kbd_head == kbd_tail);
}

static int fifo_full(void) {
    return ((uint8_t)((kbd_head + 1) & (KBD_FIFO_SIZE - 1)) == kbd_tail);
}

typedef struct {
    uint8_t valid;
    uint8_t keynum;   // ROM FE71 result Y: 0x00..0x3B
    uint8_t shift;
    uint8_t ctrl;
} atom_key_t;

// Convert the ROM key number into the physical 10x6 matrix position.
// FE71 starts Y=$3B at row 0 / column bit 5, decrements Y for each row,
// then moves to the next column bit.
static void keynum_to_matrix(uint8_t keynum, uint8_t *row, uint8_t *col)
{
    uint8_t index = (uint8_t)(0x3B - keynum);
    *row = (uint8_t)(index % 10);
    *col = (uint8_t)(5 - (index / 10));
}

// Translate host ASCII into a physical Atom key plus SHIFT/CTRL state.
// RESET clears Atom LOCK ($E7), so unshifted A-Z produce uppercase.
static atom_key_t atom_ascii_to_key(uint8_t c)
{
    atom_key_t k = {0, 0, 0, 0};

    // Treat host lower-case as the corresponding Atom letter key.
    if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);

    if (c >= 'A' && c <= 'Z') {
        k.valid = 1;
        k.keynum = (uint8_t)(c - 0x20);
        return k;
    }

    if (c >= '1' && c <= '9') {
        k.valid = 1;
        k.keynum = (uint8_t)(c - 0x20);
        return k;
    }

    switch (c) {
        case ' ':  k.valid=1; k.keynum=0x00; break;
        case '0':  k.valid=1; k.keynum=0x10; break;
        case 13:   k.valid=1; k.keynum=0x0D; break; // RETURN: row 6 col 1
        case 8:
        case 127:  k.valid=1; k.keynum=0x0F; break; // physical DEL/BACKSPACE
        case 27:   k.valid=1; k.keynum=0x3B; break; // ESC

        // Punctuation generated by the Atom ROM with LOCK off.
        case '!':  k.valid=1; k.keynum=0x11; k.shift=1; break;
        case '"': k.valid=1; k.keynum=0x12; k.shift=1; break;
        case '#':  k.valid=1; k.keynum=0x13; k.shift=1; break;
        case '$':  k.valid=1; k.keynum=0x14; k.shift=1; break;
        case '%':  k.valid=1; k.keynum=0x15; k.shift=1; break;
        case '&':  k.valid=1; k.keynum=0x16; k.shift=1; break;
        case '\'': k.valid=1; k.keynum=0x17; k.shift=1; break;
        case '(':  k.valid=1; k.keynum=0x18; k.shift=1; break;
        case ')':  k.valid=1; k.keynum=0x19; k.shift=1; break;
        case '*':  k.valid=1; k.keynum=0x1A; k.shift=1; break;
        case '+':  k.valid=1; k.keynum=0x1B; k.shift=1; break;
        case ',':  k.valid=1; k.keynum=0x1C; break;
        case '-':  k.valid=1; k.keynum=0x1D; break;
        case '.':  k.valid=1; k.keynum=0x1E; break;
        case '/':  k.valid=1; k.keynum=0x1F; break;
        case ':':  k.valid=1; k.keynum=0x1A; break;
        case ';':  k.valid=1; k.keynum=0x1B; break;
        case '<':  k.valid=1; k.keynum=0x1C; k.shift=1; break;
        case '=':  k.valid=1; k.keynum=0x1D; k.shift=1; break;
        case '>':  k.valid=1; k.keynum=0x1E; k.shift=1; break;
        case '?':  k.valid=1; k.keynum=0x1F; k.shift=1; break;
        case '@':  k.valid=1; k.keynum=0x20; break;
        case '[':  k.valid=1; k.keynum=0x01; break;
        case '\\': k.valid=1; k.keynum=0x02; break;
        case ']':  k.valid=1; k.keynum=0x03; break;
        case '^':  k.valid=1; k.keynum=0x04; break;
        default: break;
    }
    return k;
}

void atom_kbd_init(void) {
    kbd_head = 0;
    kbd_tail = 0;
    current_row = 255;
    current_row_raw = 0;
    kbd_row = -1;
    kbd_col = 0;
    kbd_shift = 0;
    kbd_ctrl = 0;
    hold_matches = 0;
    queued_key_pending = 0;
    immediate_escape = 0;
    direct_key_owned = 0;
    direct_key_active = 0;
    direct_key_keynum = 0;
    direct_key_row = -1;
    direct_key_col = 0;
    direct_key_shift = 0;
    direct_key_ctrl = 0;
    direct_key_until = 0;
    direct_repeat_window_until = 0;
    external_fe71_pending = 0;
    external_fe71_row = -1;
    external_fe71_col = 0;
    external_fe71_shift = 0;
    external_fe71_ctrl = 0;
    external_fe71_source = 0;
    release_gap_reads = 0;
    force_press_reads = 0;
    boot_row = -1;
    boot_col = 0;
    boot_matches = 0;
    bootstrap_mode = 0;
}

void atom_kbd_press_escape(void)
{
    immediate_escape = 1;
}


void atom_kbd_push_ascii(uint8_t c)
{
    atom_key_t k = atom_ascii_to_key(c);
    if (!k.valid) return;

    // Every mapped Atom key may be claimed by software that scans the
    // physical keyboard directly. ESC retains its dedicated immediate path.
    uint8_t is_direct = (c != 27);

    // V109: after a direct matrix scanner has claimed a physical key, terminal
    // auto-repeat copies of that same key are NOT queued.  They are evidence
    // that the host key is still being held, so just refresh its physical
    // contact and repeat-recognition window.  A different key remains an
    // ordinary FIFO character and therefore preserves normal typing order.
    if (is_direct && direct_key_owned &&
        (int32_t)(clockticks6502 - direct_repeat_window_until) < 0 &&
        k.keynum == direct_key_keynum &&
        k.shift == direct_key_shift && k.ctrl == direct_key_ctrl) {
        direct_key_active = 1;
        direct_key_until = clockticks6502 + DIRECT_REPEAT_HOLD_CYCLES;
        direct_repeat_window_until = clockticks6502 + DIRECT_REPEAT_WINDOW_CYCLES;
        return;
    }

    // Ownership expires silently once no repeat has been seen for the full
    // recognition window.  This does not affect an already active contact;
    // atom_kbd_read_port_b() releases that on direct_key_until.
    if (direct_key_owned &&
        (int32_t)(clockticks6502 - direct_repeat_window_until) >= 0) {
        direct_key_owned = 0;
    }

    if (fifo_full()) return;

    uint16_t packed = (uint16_t)(k.keynum & KEY_CODE_MASK);
    if (k.shift) packed |= KEY_SHIFT;
    if (k.ctrl)  packed |= KEY_CTRL;
    if (is_direct) packed |= KEY_DIRECT;

    kbd_fifo[kbd_head] = packed;
    kbd_head = (uint8_t)((kbd_head + 1) & (KBD_FIFO_SIZE - 1));
}

void atom_kbd_write_port_a(uint8_t v)
{
    current_row_raw = v;
    current_row = (uint8_t)(v & 0x0F);
    if (current_row > 9) current_row = 255;
}

// Return address currently on top of the emulated 6502 stack.
// JSR pushes the address of its final operand byte, so the FE71 callers are:
//   FE9F JSR FE71 -> $FEA1  (release scan)
//   FEA7 JSR FE71 -> $FEA9  (acquire new key)
//   FEAC JSR FE71 -> $FEAE  (verify same key)
static uint16_t atom_kbd_fe71_caller(void)
{
    uint8_t sp = *fake6502_sp;
    uint8_t lo = fakemem[0x0100u + ((uint8_t)(sp + 1u))];
    uint8_t hi = fakemem[0x0100u + ((uint8_t)(sp + 2u))];
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

uint8_t atom_kbd_read_port_b(void)
{
    // Bootstrap pulse has priority. It emulates a physical key only.
    if (boot_row >= 0) {
        uint8_t ret = 0xFF;
        if (current_row == (uint8_t)boot_row) {
            ret = (uint8_t)(ret & ~(1u << boot_col));
            if (boot_matches > 0) boot_matches--;
            if (boot_matches == 0) {
                boot_row = -1;
                release_gap_reads = 60;
            }
        }
        return ret;
    }

    if (force_press_reads > 0) {
        force_press_reads--;
        return 0x00;
    }

    if (release_gap_reads > 0) {
        release_gap_reads--;
        return 0xFF;
    }

    // Identify a genuine Atom ROM FE71 keyboard scan.  The return address
    // alone is not enough outside FE71 because unrelated machine code could
    // coincidentally leave the same bytes on the stack.
    uint16_t fe71_caller = atom_kbd_fe71_caller();
    uint8_t in_fe71 = (*fake6502_pc >= 0xFE71u && *fake6502_pc <= 0xFE91u);
    uint8_t osrdch_scan = in_fe71 &&
        (fe71_caller == 0xFEA1u || fe71_caller == 0xFEA9u || fe71_caller == 0xFEAEu);

    // Any FE71 invocation not made by Kernel OSRDCH is an external/discrete
    // FE71 request. This is a different Atom interface from raw B001 polling.
    uint8_t external_fe71_scan = in_fe71 && !osrdch_scan;


    // V101c: release the synthetic physical contact when host repeats stop.
    // Ownership is retained longer than the asserted contact so that the
    // terminal's delayed first auto-repeat can be recognised without making
    // a quick tap look like a long physical press.
    if (direct_key_active &&
        (int32_t)(clockticks6502 - direct_key_until) >= 0) {
        direct_key_active = 0;
    }
    if (direct_key_owned &&
        (int32_t)(clockticks6502 - direct_repeat_window_until) >= 0) {
        direct_key_owned = 0;
    }

    // Latch a NEW queued key only while OSRDCH is explicitly scanning for
    // a new press (FEA7 -> FE71). Never introduce the next key into the
    // earlier FE9F release scan.
    if (kbd_row < 0 && !queued_key_pending &&
        !fifo_empty() && fe71_caller == 0xFEA9u && osrdch_scan) {
        uint16_t packed = kbd_fifo[kbd_tail];

        uint8_t keynum = (uint8_t)(packed & KEY_CODE_MASK);
        keynum_to_matrix(keynum, (uint8_t *)&kbd_row, &kbd_col);
        kbd_shift = (packed & KEY_SHIFT) ? 1 : 0;
        kbd_ctrl  = (packed & KEY_CTRL)  ? 1 : 0;
        queued_key_pending = 1;
    }

    uint8_t ret = 0xFF;

    // Outside genuine ROM OSRDCH, a direct matrix scanner may claim the
    // OLDEST FIFO entry if it is a mapped Atom key and has selected that key's row.
    // Claiming removes the character from the FIFO immediately: it has become
    // physical-key state and can never spill into BASIC later.
    if (!osrdch_scan && !external_fe71_scan && !queued_key_pending && !fifo_empty()) {
        uint16_t packed = kbd_fifo[kbd_tail];
        if (packed & KEY_DIRECT) {
            uint8_t keynum = (uint8_t)(packed & KEY_CODE_MASK);
            uint8_t row, col;
            keynum_to_matrix(keynum, &row, &col);

            if (current_row == row) {
                kbd_tail = (uint8_t)((kbd_tail + 1) & (KBD_FIFO_SIZE - 1));
                direct_key_owned = 1;
                direct_key_active = 1;
                direct_key_keynum = keynum;
                direct_key_row = (int8_t)row;
                direct_key_col = col;
                direct_key_shift = (packed & KEY_SHIFT) ? 1u : 0u;
                direct_key_ctrl  = (packed & KEY_CTRL)  ? 1u : 0u;
                direct_key_until = clockticks6502 + DIRECT_FIRST_PRESS_CYCLES;
                direct_repeat_window_until = clockticks6502 + DIRECT_REPEAT_WINDOW_CYCLES;
            }
        }
    }

    // A direct-scanning program sees the physical contact on every scan while
    // it is active.  Reading it does NOT consume it; only expiry releases it.
    if (direct_key_active && !osrdch_scan && !external_fe71_scan &&
        current_row == (uint8_t)direct_key_row) {
        ret &= (uint8_t)~(1u << direct_key_col);
    }

    // External FE71: expose exactly the one latched FIFO key as a physical
    // matrix contact for this ROM scan. FEB1 consumes it after successful
    // acquisition, giving one discrete action per host character.
    if (external_fe71_pending && external_fe71_scan &&
        current_row == (uint8_t)external_fe71_row) {
        ret &= (uint8_t)~(1u << external_fe71_col);
    }

    // ESC is a dedicated physical matrix contact at row 0 / column 5.
    // Atom BASIC polls B001 bit 5 directly at C504 with row 0 selected,
    // so this must be visible even when OSRDCH is not running.
    if (immediate_escape && current_row == 0) {
        ret &= (uint8_t)~0x20u;
    }

    // SHIFT and CTRL are direct Port-B inputs, independent of the matrix row
    // and independent of whether the ordinary matrix key has already been
    // released at FEB1. The Atom ROM tests these bits later during character
    // decoding (FDD8/FDE2), so they must remain visible until FDE9.
    if (kbd_shift ||
        (direct_key_active && !osrdch_scan && !external_fe71_scan && direct_key_shift) ||
        (external_fe71_pending && external_fe71_scan && external_fe71_shift))
        ret &= (uint8_t)~0x80u;
    if (kbd_ctrl || forced_ctrl ||
        (direct_key_active && !osrdch_scan && !external_fe71_scan && direct_key_ctrl) ||
        (external_fe71_pending && external_fe71_scan && external_fe71_ctrl))
        ret &= (uint8_t)~0x40u;

    // V165: proven V141 physical keyboard merge. Inputs are active-low.
    ret &= atom_hw_kbd_get_port_b(current_row);

    if (kbd_row >= 0) {
        if (current_row == (uint8_t)kbd_row) {
            ret &= (uint8_t)~(1u << kbd_col);

            /*
             * FE71 scans six column masks (A=$20,$10,$08,$04,$02,$01),
             * and visits the selected row once for EACH column.  A physical
             * key must therefore remain down on all those row reads.  Count
             * a ROM key detection only when the current BIT mask in A is
             * actually testing this key's column.  V042 incorrectly counted
             * every visit to the row, which released columns 0..4 before the
             * ROM reached them; column 5 (e.g. T) happened to work because it
             * is scanned first.
             */
            // V046: do not release a queued key based on B001 read counts.
            // Keep it physically asserted through as many FE71 retries as
            // the ROM needs. atom_kbd_cpu_pc(FEB1) commits/releases it only
            // after the ROM has successfully acquired and verified the key.
        }
    }
    return ret;
}

void atom_kbd_cpu_pc(uint16_t pc)
{
    // V190: latch an external-FE71 input as soon as FE71 is entered.
    // Serial retains the proven V110b FIFO behaviour.  If no serial key is
    // waiting, a recent physical key-down edge may be used so that a short
    // real key tap is not lost between widely spaced external FE71 calls.
    if (pc == 0xFE71u && !external_fe71_pending) {
        uint16_t caller = atom_kbd_fe71_caller();
        uint8_t is_osrdch_caller =
            (caller == 0xFEA1u || caller == 0xFEA9u || caller == 0xFEAEu);

        if (!is_osrdch_caller) {
            if (!fifo_empty()) {
                uint16_t packed = kbd_fifo[kbd_tail];
                uint8_t keynum = (uint8_t)(packed & KEY_CODE_MASK);
                keynum_to_matrix(keynum, (uint8_t *)&external_fe71_row, &external_fe71_col);
                external_fe71_shift = (packed & KEY_SHIFT) ? 1u : 0u;
                external_fe71_ctrl  = (packed & KEY_CTRL)  ? 1u : 0u;
                external_fe71_source = 1;
                external_fe71_pending = 1;
            } else {
                uint8_t row, col, shift, ctrl;
                if (atom_hw_kbd_take_fe71_event(&row, &col, &shift, &ctrl)) {
                    external_fe71_row = (int8_t)row;
                    external_fe71_col = col;
                    external_fe71_shift = shift;
                    external_fe71_ctrl = ctrl;
                    external_fe71_source = 2;
                    external_fe71_pending = 1;
                }
            }
        }
    }

    // V110a: FE71 itself returns at $FE93. External callers such as Frogger
    // never pass through the OSRDCH completion point at $FEB1.
    //
    // The pending FIFO-head key was exposed throughout FE71's complete matrix
    // scan, so consume exactly that one host character when FE71 reaches its
    // RTS. This makes each external JSR $FE71 one discrete keypress request.
    if (pc == 0xFE93u && external_fe71_pending) {
        if (external_fe71_source == 1 && !fifo_empty())
            kbd_tail = (uint8_t)((kbd_tail + 1) & (KBD_FIFO_SIZE - 1));

        external_fe71_pending = 0;
        external_fe71_row = -1;
        external_fe71_shift = 0;
        external_fe71_ctrl = 0;
    external_fe71_source = 0;

        // External FE71 is discrete, so do not carry V101c held-key ownership
        // into the next FE71 request.
        direct_key_owned = 0;
        direct_key_active = 0;
        direct_key_row = -1;
        direct_key_until = 0;
        direct_repeat_window_until = 0;
    }

    // OSRDCH has successfully acquired and verified the physical ESC key.
    // The matrix contact can now be released; key number $3B is already in Y.
    if (pc == 0xFEB1u && immediate_escape) {
        immediate_escape = 0;
    }

    // BASIC's direct running-program ESC poll at C504 branches here when
    // row-0/column-5 is low.  Release the host ESC pulse once observed.
    if (pc == 0xC547u && immediate_escape) {
        immediate_escape = 0;
    }

    if (pc == 0xFEB1u) {
        // A normal ROM keyboard acquisition must not leave an old physical
        // edge waiting to be replayed later through an external FE71 call.
        atom_hw_kbd_clear_fe71_event();
    }

    if (pc == 0xFEB1u && queued_key_pending && kbd_row >= 0) {
        // Successful Atom ROM key acquisition + verification.  Now, and only
        // now, consume the host FIFO entry and release the physical key.
        kbd_tail = (uint8_t)((kbd_tail + 1) & (KBD_FIFO_SIZE - 1));
        queued_key_pending = 0;
        kbd_row = -1;
        // V048: release the matrix key here, but KEEP SHIFT/CTRL asserted.
        // The Atom ROM has accepted the key at FEB1, but it does not decode
        // modifier state until later (BIT $B001 at FDD8/FDE2).  Clearing the
        // modifiers here made shifted punctuation decode as the underlying
        // number key, e.g. " -> 2 and ' -> 7.
        hold_matches = 0;
        // No synthetic release-gap is needed: the next OSRDCH call performs
        // its real FE9F release scan before V046 may latch at FEA7.
        release_gap_reads = 0;
    }

    // FDE9 is reached after the ROM has finished all SHIFT/CTRL tests for
    // the accepted key.  Modifiers can now be released safely.
    if (pc == 0xFDE9u) {
        kbd_shift = 0;
        kbd_ctrl = 0;
    }
}

void atom_kbd_force_ctrl(uint8_t on)
{
    forced_ctrl = on ? 1u : 0u;
}

void atom_kbd_release(void)
{
    queued_key_pending = 0;
    immediate_escape = 0;
    direct_key_owned = 0;
    direct_key_active = 0;
    direct_key_row = -1;
    direct_key_shift = 0;
    direct_key_ctrl = 0;
    direct_key_until = 0;
    direct_repeat_window_until = 0;
    external_fe71_pending = 0;
    external_fe71_row = -1;
    external_fe71_shift = 0;
    external_fe71_ctrl = 0;
    external_fe71_source = 0;
    kbd_row = -1;
    kbd_shift = 0;
    kbd_ctrl = 0;
    hold_matches = 0;
    release_gap_reads = 60;
}

void atom_kbd_flush(void)
{
    kbd_head = 0;
    immediate_escape = 0;
    direct_key_owned = 0;
    direct_key_active = 0;
    direct_key_row = -1;
    direct_key_shift = 0;
    direct_key_ctrl = 0;
    direct_key_until = 0;
    direct_repeat_window_until = 0;
    external_fe71_pending = 0;
    external_fe71_row = -1;
    external_fe71_shift = 0;
    external_fe71_ctrl = 0;
    external_fe71_source = 0;
    kbd_tail = 0;
    queued_key_pending = 0;
    kbd_row = -1;
    kbd_shift = 0;
    kbd_ctrl = 0;
    hold_matches = 0;
    release_gap_reads = 0;
    force_press_reads = 0;
    boot_row = -1;
    boot_col = 0;
    boot_matches = 0;
    bootstrap_mode = 0;
}

void atom_kbd_force_anykey(uint16_t reads) { force_press_reads = reads; }
void atom_kbd_cancel_force(void) { force_press_reads = 0; }

void atom_kbd_bootstrap_key(uint8_t row, uint8_t col, uint8_t matches)
{
    boot_row = (int8_t)row;
    boot_col = col;
    boot_matches = matches;
}

void atom_kbd_set_bootstrap_mode(uint8_t enabled)
{
    bootstrap_mode = enabled ? 1 : 0;
}

void atom_kbd_inject_boot_return(uint8_t matches)
{
    // Real Atom RETURN key: key number $0D => row 6, column bit 1.
    (void)matches;
    boot_row = 6;
    boot_col = 1;
    boot_matches = 2;
}
