//-----------------------------------------------------------------------------
// fake6522.h - CYD / classic ESP32-safe version
//-----------------------------------------------------------------------------

#ifndef FAKE6522_H
#define FAKE6522_H

#include <stdint.h>
#include <stddef.h>
#include "driver/gpio.h"

// Safe dummy GPIO mappings for classic ESP32 (CYD).
// Only used for virtual bitboard LEDs in this build.
#define PORTA_1 GPIO_NUM_34
#define PORTA_2 GPIO_NUM_35
#define PORTA_3 GPIO_NUM_36
#define PORTA_4 GPIO_NUM_39
#define PORTA_5 GPIO_NUM_34
#define PORTA_6 GPIO_NUM_35
#define PORTA_7 GPIO_NUM_36
#define PORTA_8 GPIO_NUM_39

#define PORTB_1 GPIO_NUM_34
#define PORTB_2 GPIO_NUM_35
#define PORTB_3 GPIO_NUM_36
#define PORTB_4 GPIO_NUM_39
#define PORTB_5 GPIO_NUM_34
#define PORTB_6 GPIO_NUM_35
#define PORTB_7 GPIO_NUM_36
#define PORTB_8 GPIO_NUM_39

#define PORTC_1 GPIO_NUM_34
#define PORTC_2 GPIO_NUM_35
#define PORTC_3 GPIO_NUM_36
#define PORTC_4 GPIO_NUM_39
#define PORTC_5 GPIO_NUM_34
#define PORTC_6 GPIO_NUM_35
#define PORTC_7 GPIO_NUM_36
#define PORTC_8 GPIO_NUM_39

uint8_t fake6522_read(uint16_t addr);
void    fake6522_write(uint16_t addr, uint8_t byte);

#endif // FAKE6522_H
