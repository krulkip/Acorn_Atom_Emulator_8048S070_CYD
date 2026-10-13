#ifndef ATOM_OSRDCH_HOOK_H
#define ATOM_OSRDCH_HOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Installs a small 6502 routine into RAM and repoints the OSRDCH vector
// ($020A/$020B) to it, so the Atom reads characters from the host UART FIFO.
//
// Safe to call after fakemem has been cleared/ROMs loaded.
void atom_install_osrdch_serial_hook(void);

#ifdef __cplusplus
}
#endif

#endif
