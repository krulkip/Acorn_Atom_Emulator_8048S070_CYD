//-----------------------------------------------------------------------------
// fakemem.h
// 15.06.2025 github.com/SMDHuman
//-----------------------------------------------------------------------------
#ifndef FAKEMEM_H
#define FAKEMEM_H

#include <stddef.h>
#include <stdint.h>



extern uint8_t fakemem[]; // Simulated memory for the 6502 CPU

extern uint16_t fakemem_access_address;
extern uint8_t fakemem_access_data;
extern uint8_t fakemem_access_mode;
extern uint8_t atom_last_b000_write;

// #A000-$AFFF banked utility-ROM window (ASBK-style switch card).
#define ATOM_AROM_BANK_COUNT 8

// Implemented in fakemem.c, so C++ translation units must use C linkage.
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t atom_arom_bank;
void atom_arom_clear_banks(void);
void atom_arom_mark_loaded(uint8_t bank, uint8_t loaded);
uint8_t atom_arom_is_loaded(uint8_t bank);
void atom_arom_select(uint8_t bank);
#ifdef __cplusplus
}
#endif

void fakemem_init(uint16_t reset_vector);
uint8_t read6502(uint16_t addr);
void write6502(uint16_t addr, uint8_t byte);
extern uint16_t last_read_addr;
extern uint8_t  last_read_value;

#endif
