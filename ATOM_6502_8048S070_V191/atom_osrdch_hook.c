// atom_osrdch_hook.c – Patch Atom OSRDCH vector ($020A/$020B) to read from serial FIFO

#include <stdint.h>

#include "atom_osrdch_hook.h"

// Provided by fakemem.c
extern uint8_t fakemem[1 << 16];

// We expose these addresses to the 6502 code; fakemem.c traps them.
// $BFF0: SER_AVAIL (0/1)
// $BFF1: SER_DATA (pop)
#define SER_AVAIL_ADDR 0xBFF0
#define SER_DATA_ADDR  0xBFF1

// Where to place the stub in RAM.
// IMPORTANT: do NOT use $0400 – Atom/BASIC uses low-RAM workspace heavily and
// will overwrite it (which then makes typed input "echo" only on the host
// Serial console, while the Atom stops reacting).
//
// DO NOT park the stub at $8200: the Atom reset code sets the default text
// space to $8200, so BASIC and the command line will trample it.
//
// Instead, place the stub somewhere in mid RAM that the Atom OS and BASIC
// won't use for workspace in a minimal configuration.
//
// We avoid:
//  - $0000-$03FF: ZP + OS workspace + vectors
//  - $7000-$70FF: emulator callables block
//  - $8000-$81FF: this project's VRAM window
//  - $B000..: I/O areas
//
// $7E00 is a good “boring” page for this project.
#define HOOK_ADDR      0x7E00

void atom_install_osrdch_serial_hook(void)
{
    // 6502 stub at HOOK_ADDR ($7E00):
    //
    // OSRDCH_SER:
    //   PHP
    //   STX $E4
    //   STY $E5
    // wait:
    //   LDA $BFF0
    //   BEQ wait
    //   LDA $BFF1
    //   LDY $E5
    //   LDX $E4
    //   PLP
    //   RTS
    //
    // This returns the ASCII byte in A, like the real OSRDCH.

    static const uint8_t stub[] = {
        0x08,             // PHP
        0x86, 0xE4,       // STX $E4
        0x84, 0xE5,       // STY $E5
        0xAD, (uint8_t)(SER_AVAIL_ADDR & 0xFF), (uint8_t)(SER_AVAIL_ADDR >> 8), // LDA $BFF0
        0xF0, 0xFB,       // BEQ -5 (back to LDA)
        0xAD, (uint8_t)(SER_DATA_ADDR & 0xFF),  (uint8_t)(SER_DATA_ADDR >> 8),  // LDA $BFF1
        0xA4, 0xE5,       // LDY $E5
        0xA6, 0xE4,       // LDX $E4
        0x28,             // PLP
        0x60              // RTS
    };

    // Copy stub into RAM
    for (uint16_t i = 0; i < (uint16_t)sizeof(stub); ++i) {
        fakemem[HOOK_ADDR + i] = stub[i];
    }

    // Patch OSRDCH vector ($020A/$020B) to our stub
    fakemem[0x020A] = (uint8_t)(HOOK_ADDR & 0xFF);
    fakemem[0x020B] = (uint8_t)(HOOK_ADDR >> 8);
}
