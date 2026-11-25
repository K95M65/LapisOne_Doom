// Copyright (C) 2026 K95M65. Licensed under the GNU GPL v2 (see LICENSE and NOTICE).
/* Bridges Chocolate Doom 3.0.1 OPL/MIDI sources to doomgeneric's older headers. */
#ifndef LAPIS_COMPAT_H
#define LAPIS_COMPAT_H
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "doomtype.h"
#ifndef PACKEDPREFIX
#define PACKEDPREFIX
#endif
#ifndef PACKED_STRUCT
#define PACKED_STRUCT(...) PACKEDPREFIX struct __VA_ARGS__ PACKEDATTR
#endif
#define SDL_SwapBE32(x) __builtin_bswap32(x)
#define SDL_SwapBE16(x) __builtin_bswap16(x)
#define SDL_SwapLE16(x) (x)
#define SDL_SwapLE32(x) (x)
static inline void *I_Realloc(void *p, size_t n) { void *q = realloc(p, n); if (n && !q) abort(); return q; }
typedef enum { opl_doom1_1_666, opl_doom2_1_666, opl_doom_1_9 } opl_driver_ver_t;
void I_SetOPLDriverVer(opl_driver_ver_t ver);
#endif
