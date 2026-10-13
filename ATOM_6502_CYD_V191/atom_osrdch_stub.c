#include <stdint.h>
#include "fakemem.h"

// Installs a tiny OSRDCH replacement stub at $7E00:
//
//   LDA $BFF0
//   BEQ $7E00
//   LDA $BFF1
//   RTS
//
void atom_install_osrdch_stub(void)
{
    // $7E00: AD F0 BF    LDA $BFF0
    fakemem[0x7E00] = 0xAD; fakemem[0x7E01] = 0xF0; fakemem[0x7E02] = 0xBF;

    // $7E03: F0 FB       BEQ $7E00  (relative -5)
    fakemem[0x7E03] = 0xF0; fakemem[0x7E04] = 0xFB;

    // $7E05: AD F1 BF    LDA $BFF1
    fakemem[0x7E05] = 0xAD; fakemem[0x7E06] = 0xF1; fakemem[0x7E07] = 0xBF;

    // $7E08: 60          RTS
    fakemem[0x7E08] = 0x60;
}
