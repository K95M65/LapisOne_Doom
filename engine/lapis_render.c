// Doom 320x200 colour -> Lapis 400x240 1-bit (Sharp memory LCD, same panel class as Playdate).
#include "lapis_render.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "bluenoise64.h"

#define PW 400
#define PH 240
#define STRIDE 50

static const uint8_t bayer8[8][8] = {
    { 0,32, 8,40, 2,34,10,42},{48,16,56,24,50,18,58,26},{12,44, 4,36,14,46, 6,38},{60,28,52,20,62,30,54,22},
    { 3,35,11,43, 1,33, 9,41},{51,19,59,27,49,17,57,25},{15,47, 7,39,13,45, 5,37},{63,31,55,23,61,29,53,21}};

void lr_defaults(struct lr_opts *o) {
    o->dither = LR_ATKINSON; o->autoexp = 1; o->gamma = 0.75; o->contrast = 1.25; o->sharpen = 0.6;
    o->out_h = 215; o->out_w = 344; o->out_x = (PW - 344) / 2; o->out_y = 0;
}

void lr_from_env(struct lr_opts *o) {
    const char *e;
    if ((e = getenv("LAPIS_DOOM_DITHER"))) {
        if (!strcmp(e, "bayer")) o->dither = LR_BAYER;
        else if (!strcmp(e, "atkinson")) o->dither = LR_ATKINSON;
        else if (!strcmp(e, "floyd")) o->dither = LR_FLOYD;
        else if (!strcmp(e, "blue")) o->dither = LR_BLUE;
    }
    if ((e = getenv("LAPIS_DOOM_AUTOEXP"))) o->autoexp = atoi(e);
    if ((e = getenv("LAPIS_DOOM_GAMMA"))) o->gamma = atof(e);
    if ((e = getenv("LAPIS_DOOM_CONTRAST"))) o->contrast = atof(e);
    if ((e = getenv("LAPIS_DOOM_SHARPEN"))) o->sharpen = atof(e);
    if ((e = getenv("LAPIS_DOOM_SCALE"))) {
        if (!strcmp(e, "fill")) o->out_w = PW;
        else if (!strcmp(e, "43")) o->out_w = o->out_h * 4 / 3;
        else o->out_w = o->out_h * 320 / 200;
        o->out_x = (PW - o->out_w) / 2;
    }
}

static float buf[PH][PW + 2];
static float lo_s = 0, hi_s = 255;

void lr_render(const uint32_t *src, int sw, int sh, uint8_t *dst, const struct lr_opts *o) {
    int W = o->out_w, H = o->out_h;
    /* 1. luminance + bilinear-ish (box) resample */
    for (int y = 0; y < H; y++) {
        float fy = (y + 0.5f) * sh / H - 0.5f; int y0 = fy < 0 ? 0 : (int)fy; float ty = fy - y0; int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        for (int x = 0; x < W; x++) {
            float fx = (x + 0.5f) * sw / W - 0.5f; int x0 = fx < 0 ? 0 : (int)fx; float tx = fx - x0; int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            uint32_t p[4] = { src[y0 * sw + x0], src[y0 * sw + x1], src[y1 * sw + x0], src[y1 * sw + x1] };
            float l[4];
            for (int k = 0; k < 4; k++) l[k] = ((p[k] >> 16 & 255) * 0.299f + (p[k] >> 8 & 255) * 0.587f + (p[k] & 255) * 0.114f);
            buf[y][x] = (l[0] * (1 - tx) + l[1] * tx) * (1 - ty) + (l[2] * (1 - tx) + l[3] * tx) * ty;
        }
    }
    /* 2. auto exposure from the 3D view (rows above the status bar), smoothed over time */
    int view_h = H * 168 / 200;
    float lo = 0, hi = 255;
    if (o->autoexp) {
        int hist[256] = {0}, n = 0;
        for (int y = 0; y < view_h; y += 2) for (int x = 0; x < W; x += 2) { int v = (int)buf[y][x]; hist[v < 0 ? 0 : v > 255 ? 255 : v]++; n++; }
        int acc = 0, plo = -1, phi = 255;
        for (int i = 0; i < 256; i++) { acc += hist[i]; if (plo < 0 && acc >= n * 2 / 100) plo = i; if (acc >= n * 985 / 1000) { phi = i; break; } }
        if (plo < 0) plo = 0;
        if (phi - plo < 80) { int mid = (phi + plo) / 2; plo = mid - 40; phi = mid + 40; }
        if (plo < 0) plo = 0; if (phi > 255) phi = 255;
        lo_s += (plo - lo_s) * 0.12f; hi_s += (phi - hi_s) * 0.12f;
        lo = lo_s; hi = hi_s;
    }
    /* 3. levels + gamma + contrast LUT, applied per pixel (status bar uses fixed levels) */
    for (int y = 0; y < H; y++) {
        float l0 = y < view_h ? lo : 0, h0 = y < view_h ? hi : 255;
        for (int x = 0; x < W; x++) {
            float v = (buf[y][x] - l0) / (h0 - l0);
            if (v < 0) v = 0; if (v > 1) v = 1;
            v = powf(v, (float)o->gamma);
            v = (v - 0.5f) * (float)o->contrast + 0.5f;
            buf[y][x] = v * 255.0f;
        }
    }
    /* 4. unsharp mask (cross kernel) for crisper edges on the 1-bit panel */
    if (o->sharpen > 0) {
        static float tmp[PH][PW + 2];
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            float c = buf[y][x];
            float n = (buf[y > 0 ? y - 1 : y][x] + buf[y + 1 < H ? y + 1 : y][x] + buf[y][x > 0 ? x - 1 : x] + buf[y][x + 1 < W ? x + 1 : x]) * 0.25f;
            tmp[y][x] = c + (float)o->sharpen * (c - n);
        }
        for (int y = 0; y < H; y++) memcpy(buf[y], tmp[y], W * sizeof(float));
    }
    /* 5. dither to 1 bit */
    memset(dst, 0, STRIDE * PH);
    for (int y = 0; y < H; y++) {
        int py = o->out_y + y;
        uint8_t *row = dst + py * STRIDE;
        int serp = (o->dither >= LR_ATKINSON) && (y & 1);
        for (int i = 0; i < W; i++) {
            int x = serp ? W - 1 - i : i;
            int px = o->out_x + x;
            float v = buf[y][x];
            int on;
            if (o->dither == LR_BAYER) on = v > (bayer8[py & 7][px & 7] * 4 + 2);
            else if (o->dither == LR_BLUE) on = v > bluenoise64[(py & 63) * 64 + (px & 63)] + 0.5f;
            else {
                on = v >= 128; float err = v - (on ? 255 : 0);
                int dx = serp ? -1 : 1;
                #define ADD(xx, yy, w) do { int _x = (xx), _y = (yy); if (_x >= 0 && _x < W && _y < H) buf[_y][_x] += err * (w); } while (0)
                if (o->dither == LR_ATKINSON) {
                    err *= 0.125f;
                    ADD(x + dx, y, 1); ADD(x + 2 * dx, y, 1); ADD(x - dx, y + 1, 1); ADD(x, y + 1, 1); ADD(x + dx, y + 1, 1); ADD(x, y + 2, 1);
                } else {
                    ADD(x + dx, y, 7 / 16.f); ADD(x - dx, y + 1, 3 / 16.f); ADD(x, y + 1, 5 / 16.f); ADD(x + dx, y + 1, 1 / 16.f);
                }
                #undef ADD
            }
            if (on) row[px >> 3] |= 0x80 >> (px & 7);
        }
    }
}
