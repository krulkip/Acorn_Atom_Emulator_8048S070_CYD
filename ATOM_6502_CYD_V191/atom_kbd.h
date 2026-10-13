#ifndef ATOM_KBD_H
#define ATOM_KBD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Core API
void    atom_kbd_init(void);
void    atom_kbd_push_ascii(uint8_t c);
void    atom_kbd_press_escape(void);
void    atom_kbd_write_port_a(uint8_t v);
uint8_t atom_kbd_read_port_b(void);

// Control helpers
void atom_kbd_cpu_pc(uint16_t pc);
void atom_kbd_release(void);
void atom_kbd_flush(void);

// Optional "force any key" (legacy bootstrap)
void atom_kbd_force_anykey(uint16_t reads);
void atom_kbd_force_ctrl(uint8_t on);
void atom_kbd_cancel_force(void);

// New precise bootstrap (single key pulse)
void atom_kbd_bootstrap_key(uint8_t row, uint8_t col, uint8_t matches);
void atom_kbd_set_bootstrap_mode(uint8_t enabled);
void atom_kbd_inject_boot_return(uint8_t matches);

// Debug exports



// Optional debug hooks (implemented in .ino if needed)
void atom_kbd_dbg_event(uint8_t event, uint8_t packed, uint8_t row_raw);
void atom_kbd_dbg_b001(uint8_t row_raw, uint8_t ret, uint8_t state);

#ifdef __cplusplus
}
#endif

#endif
