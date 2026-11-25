#include "usbpad.h"
#include <stdio.h>
#include <assert.h>
int main(void) {
    struct usbpad_state s = {0};
    unsigned char r360[20] = {0x00,0x14, 0x11 /*dup+start*/, 0x90 /*A+Y*/, 255, 0, 0x00,0x80 /*lx=-32768*/, 0xff,0x7f /*ly=32767*/, 0,0, 0,0};
    assert(usbpad_parse_360(r360, 20, &s) == 1);
    assert(s.buttons == (UP_DUP|UP_START|UP_A|UP_Y)); assert(s.lt == 1000 && s.rt == 0);
    assert(s.lx == -1000 && s.ly == 999);
    struct usbpad_state g = {0};
    unsigned char gip[18] = {0x20,0x00,0x05,0x0e, 0x14 /*start? 0x04 + A 0x10*/, 0x18 /*dright + LB*/, 0xff,0x03 /*LT 1023*/, 0,0, 0,0, 0x01,0x80 /*ly -32767*/, 0xff,0x7f, 0,0};
    assert(usbpad_parse_gip(gip, 18, &g) == 1);
    assert(g.buttons == (UP_START|UP_A|UP_DRIGHT|UP_LB)); assert(g.lt == 1000); assert(g.ly == -999); assert(g.rx == 999);
    unsigned char guide[6] = {0x07,0x30,0x01,0x02,0x01,0x5b};
    assert(usbpad_parse_gip(guide, 6, &g) == 2 && (g.buttons & UP_GUIDE));
    assert(usbpad_parse_gip(gip, 18, &g) == 1 && (g.buttons & UP_GUIDE));   /* guide sticky across input reports */
    unsigned char bad[4] = {0x03,0x20,0,0}; assert(usbpad_parse_gip(bad, 4, &g) == 0);
    puts("parser tests OK"); return 0;
}
