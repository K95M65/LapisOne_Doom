// Copyright (C) 2026 K95M65. Licensed under the GNU GPL v2 (see LICENSE and NOTICE).
#ifndef USBPAD_H
#define USBPAD_H
#include <stdint.h>
enum { USBPAD_360 = 1, USBPAD_ONE = 2 };
enum {
    UP_A = 1 << 0, UP_B = 1 << 1, UP_X = 1 << 2, UP_Y = 1 << 3, UP_LB = 1 << 4, UP_RB = 1 << 5,
    UP_START = 1 << 6, UP_BACK = 1 << 7, UP_LS = 1 << 8, UP_RS = 1 << 9, UP_GUIDE = 1 << 10,
    UP_DUP = 1 << 11, UP_DDOWN = 1 << 12, UP_DLEFT = 1 << 13, UP_DRIGHT = 1 << 14,
};
struct usbpad_state { int connected; uint32_t buttons; int lx, ly, rx, ry, lt, rt; /* sticks -1000..1000 (y up = +), triggers 0..1000 */ };
int usbpad_parse_360(const uint8_t *r, int n, struct usbpad_state *s);
int usbpad_parse_gip(const uint8_t *r, int n, struct usbpad_state *s);
void usbpad_scan(void);
int usbpad_get(int idx, struct usbpad_state *out);
int usbpad_max(void);
#endif
