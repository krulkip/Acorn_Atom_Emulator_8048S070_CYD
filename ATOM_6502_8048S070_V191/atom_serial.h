#ifndef ATOM_SERIAL_H
#define ATOM_SERIAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Simple UART->Atom FIFO used by the OSRDCH hook.
void    atom_serial_init(void);
void    atom_serial_push(uint8_t c);
uint8_t atom_serial_avail(void);   // 0/1
uint8_t atom_serial_pop(void);     // returns 0 if empty

// V123: Atom output -> host Serial mirror FIFO.
void    atom_debug_out_init(void);
void    atom_debug_out_push(uint8_t c);
uint8_t atom_debug_out_avail(void);
uint8_t atom_debug_out_pop(void);

#ifdef __cplusplus
}
#endif

#endif
