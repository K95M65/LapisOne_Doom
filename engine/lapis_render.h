// Copyright (C) 2026 K95M65. Licensed under the GNU GPL v2 (see LICENSE and NOTICE).
#ifndef LAPIS_RENDER_H
#define LAPIS_RENDER_H
#include <stdint.h>
enum { LR_BAYER = 0, LR_BLUE = 1, LR_ATKINSON = 2, LR_FLOYD = 3 };
struct lr_opts {
    int dither;          /* LR_* */
    int autoexp;         /* stretch each frame's 3D-view levels (smoothed) */
    double gamma;        /* <1 brightens mid-tones */
    double contrast;     /* around mid-grey */
    double sharpen;      /* unsharp amount, 0 = off */
    int out_w, out_h, out_x, out_y;   /* placement on the 400x240 panel */
};
void lr_defaults(struct lr_opts *o);
void lr_from_env(struct lr_opts *o);
/* src: XRGB8888 sw x sh. dst: 400x240 1bpp MSB-first, 50-byte stride, 1 = white. */
void lr_render(const uint32_t *src, int sw, int sh, uint8_t *dst, const struct lr_opts *o);
#endif
