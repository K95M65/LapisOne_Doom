#include "lapis_render.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {   /* render_tool in.raw out.pbm  (opts from env, repeat N frames for autoexp settle) */
    static uint32_t src[320 * 200]; static uint8_t dst[12000];
    FILE *f = fopen(argv[1], "rb"); if (!f || fread(src, 4, 320 * 200, f) != 320 * 200) return 1; fclose(f);
    struct lr_opts o; lr_defaults(&o); lr_from_env(&o);
    for (int i = 0; i < 40; i++) lr_render(src, 320, 200, dst, &o);
    f = fopen(argv[2], "wb"); fprintf(f, "P4\n400 240\n");
    for (int i = 0; i < 12000; i++) dst[i] ^= 0xff;   /* PBM: 1 = black */
    fwrite(dst, 1, 12000, f); fclose(f); return 0;
}
