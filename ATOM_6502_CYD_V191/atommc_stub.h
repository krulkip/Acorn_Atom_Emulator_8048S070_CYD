#ifndef ATOMMC_STUB_H
#define ATOMMC_STUB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void atommmc_stub_reset(void);
uint8_t atommmc_stub_read(uint16_t addr);
void atommmc_stub_write(uint16_t addr, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
