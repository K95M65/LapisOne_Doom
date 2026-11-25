// Copyright (C) 2026 K95M65. Licensed under the GNU GPL v2 (see LICENSE and NOTICE).
// lapis-usbpad-test: list/poll Xbox USB pads via usbfs (diagnostic for doom-padcheck)
#include "usbpad.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    for (int t = 0; t < secs * 10; t++) {
        if (t % 20 == 0) usbpad_scan();
        for (int i = 0; i < usbpad_max(); i++) {
            struct usbpad_state s;
            if (usbpad_get(i, &s) && s.connected && t % 5 == 0)
                printf("pad%d buttons=0x%04x L(%d,%d) R(%d,%d) LT=%d RT=%d\n", i, s.buttons, s.lx, s.ly, s.rx, s.ry, s.lt, s.rt);
        }
        usleep(100000);
    }
    return 0;
}
