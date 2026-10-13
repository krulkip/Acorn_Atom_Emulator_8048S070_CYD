// atom_serial.c  – UART FIFO for Atom OSRDCH injection

#include <stdint.h>
#include "atom_serial.h"

#define SER_FIFO_SIZE 128  // MUST be power-of-two for mask wrap

static volatile uint8_t fifo[SER_FIFO_SIZE];
static volatile uint8_t head = 0;
static volatile uint8_t tail = 0;

static inline uint8_t next_i(uint8_t i) { return (uint8_t)((i + 1) & (SER_FIFO_SIZE - 1)); }

void atom_serial_init(void)
{
    head = 0;
    tail = 0;
}

void atom_serial_push(uint8_t c)
{
    uint8_t n = next_i(head);
    if (n == tail) {
        // full -> drop
        return;
    }
    fifo[head] = c;
    head = n;
}

uint8_t atom_serial_avail(void)
{
    return (head != tail) ? 1 : 0;
}

uint8_t atom_serial_pop(void)
{
    if (head == tail) return 0;
    uint8_t c = fifo[tail];
    tail = next_i(tail);
    return c;
}


// V123: separate non-blocking Atom output FIFO.
#define DBG_OUT_FIFO_SIZE 256
static volatile uint8_t dbg_out_fifo[DBG_OUT_FIFO_SIZE];
static volatile uint16_t dbg_out_head = 0;
static volatile uint16_t dbg_out_tail = 0;

static inline uint16_t dbg_out_next(uint16_t i)
{
    return (uint16_t)((i + 1u) & (DBG_OUT_FIFO_SIZE - 1u));
}

void atom_debug_out_init(void)
{
    dbg_out_head = 0;
    dbg_out_tail = 0;
}

void atom_debug_out_push(uint8_t c)
{
    uint16_t n = dbg_out_next(dbg_out_head);
    if (n == dbg_out_tail) return;
    dbg_out_fifo[dbg_out_head] = c;
    dbg_out_head = n;
}

uint8_t atom_debug_out_avail(void)
{
    return dbg_out_head != dbg_out_tail;
}

uint8_t atom_debug_out_pop(void)
{
    if (dbg_out_head == dbg_out_tail) return 0;
    uint8_t c = dbg_out_fifo[dbg_out_tail];
    dbg_out_tail = dbg_out_next(dbg_out_tail);
    return c;
}
