#ifndef ATOM_HW_KBD_H
#define ATOM_HW_KBD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int     atom_hw_kbd_init(void);
void    atom_hw_kbd_scan(void);
uint8_t atom_hw_kbd_get_port_b(uint8_t row);
uint8_t atom_hw_kbd_rept_pressed(void);
uint8_t atom_hw_kbd_break_pressed(void);
// V190: one-shot recent physical key-down event for external FE71 callers.
// Returns 1 and consumes the event if it is still fresh.
uint8_t atom_hw_kbd_take_fe71_event(uint8_t *row, uint8_t *col, uint8_t *shift, uint8_t *ctrl);
void    atom_hw_kbd_clear_fe71_event(void);
#ifdef __cplusplus
}
#endif
#endif
