#pragma once
#ifndef INFGO_DISPLAY_H
#define INFGO_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void idisplay_init(void);         // called once from bitboard_app_main()
void idisplay_update(void);       // called periodically from Arduino loop()
void idsplay_blink_led(uint16_t delay_ms);  // optional “activity” hook

#ifdef __cplusplus
}
#endif

#endif // INFGO_DISPLAY_H
