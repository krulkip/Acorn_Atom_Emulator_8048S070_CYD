// fakemem_atom.h
#pragma once
#include <Arduino.h>


#ifndef FAKEMEM_ATOM_H
#define FAKEMEM_ATOM_H

#ifdef __cplusplus
extern "C" {
#endif

void fakemem_init_atom();
void atom_video_update();
void atom_scan_for_banner();
uint8_t atom_reload_asbk_s();
void atom_dump_vram_top();
void atom_dump_vram_ascii_top();

#ifdef __cplusplus
}
#endif

#endif
