// doomgeneric backend for the Pamir Lapis One 400x240 1-bit screen.
// Frames go over the lapis-hwserviced app-window protocol v1 (LPFW packets on a
// SOCK_SEQPACKET socket that the launcher obtained via D-Bus CreateWindow and
// passed to us as fd $LAPIS_FRAME_FD). Input: the Lapis's own A/B/fingerprint
// (protocol input packets) plus any evdev keyboard or gamepad (Xbox via xpad).
#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"

#undef KEY_TAB
#undef KEY_ENTER
#undef KEY_BACKSPACE
#undef KEY_MINUS
#undef KEY_F1
#undef KEY_F2
#undef KEY_F3
#undef KEY_F4
#undef KEY_F5
#undef KEY_F6
#undef KEY_F7
#undef KEY_F8
#undef KEY_F9
#undef KEY_F10
#undef KEY_F11
#undef KEY_F12
#undef KEY_EQUALS
#undef KEY_PAUSE
#undef KEY_HOME
#undef KEY_END
#undef KEY_PGUP
#undef KEY_PGDN
#undef KEY_INS
#undef KEY_DEL
#define D_TAB 9
#define D_ENTER 13
#define D_BACKSPACE 0x7f
#define D_MINUS 0x2d
#define D_EQUALS 0x3d
#define D_F(n) (0x80 + 0x3a + (n))

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include "usbpad.h"

#define W 400
#define H 240
#define STRIDE 50
#define PIX_BYTES (STRIDE * H)
#define HDR 56
#define TOOLBAR_Y 215

enum { K_FRAME = 1, K_VIS = 2, K_REQ = 3, K_PRESENTED = 4, K_DROPPED = 5,
       K_CLOSING = 6, K_BACK = 7, K_INPUT = 8 };
#define F_FULL 1
#define F_HANDLES_B 2
#define F_TAKES_INPUT 8

static int sock = -1;
static uint32_t win_id, win_gen, vis_gen;
static uint64_t vis_token, seq;
static bool visible, requested, have_frame;
static uint8_t packet[HDR + PIX_BYTES];
static struct timespec t0;
static int trace;

/* ---------------- key queue ---------------- */
#define KQ 256
static uint16_t kq[KQ];
static unsigned kq_w, kq_r;
static void push(int pressed, unsigned char key) {
    if (!key) return;
    kq[kq_w++ % KQ] = (pressed << 8) | key;
}
/* reference-counted logical keys so stick + dpad + device can share arrows */
static int refc[256];
static void hold(unsigned char key, bool down) {
    if (!key) return;
    if (down) { if (refc[key]++ == 0) push(1, key); }
    else if (refc[key] > 0) { if (--refc[key] == 0) push(0, key); }
}
/* timed taps for Lapis swipes */
static struct { unsigned char key; uint32_t until; } pulses[16];
static void pulse(unsigned char key, uint32_t ms) {
    uint32_t until = DG_GetTicksMs() + ms;
    for (int i = 0; i < 16; i++) if (pulses[i].key == key) {          /* repeat swipe: extend, keep holding */
        if (until > pulses[i].until) pulses[i].until = until; return; }
    for (int i = 0; i < 16; i++) if (!pulses[i].key) {
        pulses[i].key = key; pulses[i].until = DG_GetTicksMs() + ms; hold(key, true); return; }
}
static void run_pulses(void) {
    uint32_t now = DG_GetTicksMs();
    for (int i = 0; i < 16; i++) if (pulses[i].key && now >= pulses[i].until) {
        hold(pulses[i].key, false); pulses[i].key = 0; }
}

/* ---------------- LPFW protocol ---------------- */
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (8 * i); }
static uint16_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static void send_frame(void) {
    if (!visible || !requested || !have_frame) return;
    seq++;
    memcpy(packet, "LPFW", 4);
    put16(packet + 4, 1); put16(packet + 6, K_FRAME); put16(packet + 8, HDR);
    put16(packet + 10, F_FULL | F_HANDLES_B | F_TAKES_INPUT);
    put32(packet + 12, win_id); put32(packet + 16, win_gen); put32(packet + 20, vis_gen);
    put64(packet + 24, seq); put64(packet + 32, vis_token); put64(packet + 40, 0);
    put32(packet + 48, PIX_BYTES); put32(packet + 52, 0);
    ssize_t n = send(sock, packet, sizeof packet, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) { seq--; if (errno != EAGAIN && errno != EWOULDBLOCK) { perror("lapis send"); exit(0); } return; }
    requested = false;
    if (trace) fprintf(stderr, "TX frame seq=%llu vg=%u tok=%llx\n", (unsigned long long)seq, vis_gen, (unsigned long long)vis_token);
}

/* Lapis device controls.
   In play: swipe up/down = walk forward/back (~0.6 s per swipe, repeated swipes chain into continuous
   walking), swipe left/right = turn ~45 deg, tap = use/open, tap-and-hold = auto-walk forward until the
   next swipe/tap. A = fire, fingerprint press = use. In menus: swipes = menu up/down/left/right, tap or
   A = select, fingerprint = yes. B = menu / back. Hold A + fingerprint for 2 s = quit to the dashboard. */
extern int lapis_in_game(void);
extern void lapis_quit(void);
static bool autowalk, a_held, fp_held; static uint32_t combo_since;
static int swipe_ms(const char *env, int def) { const char *e = getenv(env); return e ? atoi(e) : def; }
static void stop_autowalk(void) { if (autowalk) { autowalk = false; hold(KEY_UPARROW, false); } }
static void lapis_input(int code, bool canceled) {
    bool game = lapis_in_game();
    switch (code) {
    case 1: if (!canceled) { a_held = true; hold(KEY_FIRE, true); push(1, D_ENTER); } break;   /* A down */
    case 2: a_held = false; hold(KEY_FIRE, false); push(0, D_ENTER); break;                  /* A up */
    case 3: if (!canceled) { fp_held = true; hold(KEY_USE, true); push(1, 'y'); } break;     /* FP down */
    case 4: fp_held = false; hold(KEY_USE, false); push(0, 'y'); break;
    case 5: if (game) { stop_autowalk(); pulse(KEY_UPARROW, swipe_ms("LAPIS_DOOM_WALK_MS", 600)); } else pulse(KEY_UPARROW, 60); break;
    case 6: if (game) { stop_autowalk(); pulse(KEY_DOWNARROW, swipe_ms("LAPIS_DOOM_WALK_MS", 600)); } else pulse(KEY_DOWNARROW, 60); break;
    case 7: pulse(KEY_LEFTARROW, game ? swipe_ms("LAPIS_DOOM_TURN_MS", 290) : 60); break;
    case 8: pulse(KEY_RIGHTARROW, game ? swipe_ms("LAPIS_DOOM_TURN_MS", 290) : 60); break;
    case 9: if (game) { stop_autowalk(); pulse(KEY_USE, 120); } else pulse(D_ENTER, 60); break;   /* tap */
    case 10: if (game && !autowalk) { autowalk = true; hold(KEY_UPARROW, true); } else stop_autowalk(); break; /* tap-and-hold */
    default: break;
    }
}
static uint32_t pad_combo_since;
static void check_quit_combos(void) {
    uint32_t now = DG_GetTicksMs();
    if (a_held && fp_held) { if (!combo_since) combo_since = now ? now : 1; else if (now - combo_since > 2000) { fprintf(stderr, "lapis: quit (A+fingerprint)\n"); lapis_quit(); } }
    else combo_since = 0;
    static uint32_t test_quit = (uint32_t)-1;
    if (test_quit == (uint32_t)-1) { const char *e = getenv("LAPIS_DOOM_TEST_QUIT_MS"); test_quit = e ? (uint32_t)atoi(e) : 0; }
    if (test_quit && now > test_quit) { fprintf(stderr, "lapis: quit (test timer)\n"); lapis_quit(); }
}

static void pump_socket(void) {
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = recv(sock, buf, sizeof buf, MSG_DONTWAIT);
        if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) return; perror("lapis recv"); exit(0); }
        if (n == 0) { fprintf(stderr, "lapis: socket closed\n"); exit(0); }
        if (n < HDR || memcmp(buf, "LPFW", 4)) continue;
        uint16_t kind = get16(buf + 6), flags = get16(buf + 10);
        if (trace) fprintf(stderr, "RX kind=%u flags=%u vg=%u seq=%llu tok=%llx len=%zd\n", kind, flags, get32(buf + 20), (unsigned long long)get64(buf + 24), (unsigned long long)get64(buf + 32), n);
        switch (kind) {
        case K_VIS:
            visible = flags & 1;
            vis_gen = get32(buf + 20);
            vis_token = visible ? get64(buf + 32) : 0;
            if (visible) seq = 0;
            requested = false;
            if (!visible) { for (int k = 0; k < 256; k++) while (refc[k]) hold(k, false); }
            break;
        case K_REQ: if (visible) { requested = true; send_frame(); } break;
        case K_CLOSING: fprintf(stderr, "lapis: closing (%u)\n", flags); exit(0);
        case K_BACK: push(1, KEY_ESCAPE); push(0, KEY_ESCAPE); break;
        case K_INPUT: lapis_input(flags & 0xff, flags & 0x100); break;
        default: break;
        }
    }
}

/* ---------------- evdev: keyboards + gamepads ---------------- */
#define MAXDEV 16
struct dev {
    int fd; char path[64]; bool pad;
    struct input_absinfo abs[ABS_CNT]; bool hasabs[ABS_CNT];
    int axis_state[ABS_CNT];   /* -1, 0, +1 */
};
static struct dev devs[MAXDEV];
static int ndev;
static uint32_t last_scan;

static bool test_bit(const unsigned long *a, int b) { return a[b / (8 * sizeof(long))] >> (b % (8 * sizeof(long))) & 1; }

static void scan_devices(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && ndev < MAXDEV) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[64]; snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        bool known = false;
        for (int i = 0; i < ndev; i++) if (!strcmp(devs[i].path, path)) known = true;
        if (known) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long keys[KEY_CNT / (8 * sizeof(long)) + 1] = {0}, abs[ABS_CNT / (8 * sizeof(long)) + 1] = {0};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        bool pad = test_bit(keys, BTN_SOUTH) || test_bit(keys, BTN_GAMEPAD);
        bool kbd = test_bit(keys, KEY_A) && test_bit(keys, KEY_SPACE);
        if (!pad && !kbd) { close(fd); continue; }
        struct dev *dv = &devs[ndev++];
        memset(dv, 0, sizeof *dv);
        dv->fd = fd; dv->pad = pad; snprintf(dv->path, sizeof dv->path, "%s", path);
        for (int a = 0; a < ABS_CNT; a++) if (test_bit(abs, a)) {
            dv->hasabs[a] = ioctl(fd, EVIOCGABS(a), &dv->abs[a]) == 0;
        }
        char name[128] = "?"; ioctl(fd, EVIOCGNAME(sizeof name), name);
        fprintf(stderr, "lapis: input %s: %s (%s)\n", path, name, pad ? "gamepad" : "keyboard");
    }
    closedir(d);
}

static void drop_dev(int i) {
    struct dev *dv = &devs[i];
    for (int a = 0; a < ABS_CNT; a++) dv->axis_state[a] = 0;
    close(dv->fd);
    fprintf(stderr, "lapis: input %s removed\n", dv->path);
    devs[i] = devs[--ndev];
    for (int k = 0; k < 256; k++) while (refc[k]) hold(k, false);
}

static unsigned char kbd_key(unsigned code) {
    static const char row1[] = "1234567890", row2[] = "qwertyuiop[]", row3[] = "asdfghjkl;'", row4[] = "\\zxcvbnm,./";
    if (code >= KEY_1 && code <= KEY_0) return row1[code - KEY_1];
    if (code >= KEY_Q && code <= KEY_RIGHTBRACE) return row2[code - KEY_Q];
    if (code >= KEY_A && code <= KEY_APOSTROPHE) return row3[code - KEY_A];
    if (code >= KEY_BACKSLASH && code <= KEY_SLASH) return row4[code - KEY_BACKSLASH];
    if (code >= KEY_F1 && code <= KEY_F10) return D_F(code - KEY_F1 + 1);
    switch (code) {
    case KEY_ENTER: case KEY_KPENTER: return D_ENTER;
    case KEY_ESC: return KEY_ESCAPE;
    case KEY_LEFT: return KEY_LEFTARROW;
    case KEY_RIGHT: return KEY_RIGHTARROW;
    case KEY_UP: return KEY_UPARROW;
    case KEY_DOWN: return KEY_DOWNARROW;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: return KEY_FIRE;
    case KEY_SPACE: return KEY_USE;
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return KEY_RSHIFT;
    case KEY_LEFTALT: case KEY_RIGHTALT: return KEY_LALT;
    case KEY_TAB: return D_TAB;
    case KEY_BACKSPACE: return D_BACKSPACE;
    case KEY_MINUS: return D_MINUS;
    case KEY_EQUAL: return D_EQUALS;
    case KEY_F11: return 0x80 + 0x57;
    case KEY_F12: return 0x80 + 0x58;
    default: return 0;
    }
}

/* Xbox (xpad / standard Linux gamepad) button map */
static bool ev_start, ev_select;
static void pad_button(unsigned code, bool down) {
    switch (code) {
    case BTN_SOUTH: hold(KEY_USE, down); hold(D_ENTER, down); hold('y', down); break;   /* A: use / menu select / yes */
    case BTN_EAST:  hold(D_BACKSPACE, down); hold('n', down); break;                     /* B: menu back / no */
    case BTN_WEST:  hold(KEY_FIRE, down); break;                                         /* X: fire */
    case BTN_NORTH: hold(']', down); break;                                              /* Y: next weapon */
    case BTN_TL:    hold('[', down); break;                                              /* LB: prev weapon */
    case BTN_TR:    hold(']', down); break;                                              /* RB: next weapon */
    case BTN_TL2:   hold(KEY_RSHIFT, down); break;                                       /* LT (digital): run */
    case BTN_TR2:   hold(KEY_FIRE, down); break;                                         /* RT (digital): fire */
    case BTN_START: ev_start = down; hold(KEY_ESCAPE, down); break;                                       /* Menu/Start: menu */
    case BTN_SELECT: ev_select = down; hold(D_TAB, down); break;                                           /* View/Back: automap */
    case BTN_THUMBL: hold(KEY_RSHIFT, down); break;                                      /* L3: run */
    case BTN_THUMBR: hold(KEY_USE, down); break;                                         /* R3: use */
    case BTN_DPAD_UP: hold(KEY_UPARROW, down); break;
    case BTN_DPAD_DOWN: hold(KEY_DOWNARROW, down); break;
    case BTN_DPAD_LEFT: hold(KEY_LEFTARROW, down); break;
    case BTN_DPAD_RIGHT: hold(KEY_RIGHTARROW, down); break;
    default: break;
    }
}

static void set_axis(struct dev *dv, int a, int st, unsigned char neg, unsigned char pos) {
    int old = dv->axis_state[a];
    if (old == st) return;
    if (old < 0) hold(neg, false); else if (old > 0) hold(pos, false);
    if (st < 0) hold(neg, true); else if (st > 0) hold(pos, true);
    dv->axis_state[a] = st;
}

static void pad_axis(struct dev *dv, int a, int v) {
    if (!dv->hasabs[a]) return;
    struct input_absinfo *ai = &dv->abs[a];
    double range = (double)ai->maximum - ai->minimum; if (range <= 0) return;
    double n = (v - ai->minimum) / range;            /* 0..1 */
    double c = n * 2.0 - 1.0;                         /* -1..1 for sticks */
    switch (a) {
    case ABS_X:  set_axis(dv, a, c < -0.45 ? -1 : c > 0.45 ? 1 : 0, ',', '.'); break;                 /* left stick X: strafe */
    case ABS_Y:  set_axis(dv, a, c < -0.35 ? -1 : c > 0.35 ? 1 : 0, KEY_UPARROW, KEY_DOWNARROW); break; /* left stick Y: move */
    case ABS_RX: set_axis(dv, a, c < -0.35 ? -1 : c > 0.35 ? 1 : 0, KEY_LEFTARROW, KEY_RIGHTARROW); break; /* right stick X: turn */
    case ABS_HAT0X: set_axis(dv, a, v < 0 ? -1 : v > 0 ? 1 : 0, KEY_LEFTARROW, KEY_RIGHTARROW); break;  /* d-pad */
    case ABS_HAT0Y: set_axis(dv, a, v < 0 ? -1 : v > 0 ? 1 : 0, KEY_UPARROW, KEY_DOWNARROW); break;
    case ABS_RZ: case ABS_GAS:   set_axis(dv, a, n > 0.4 ? 1 : 0, 0, KEY_FIRE); break;               /* RT: fire */
    case ABS_Z:  case ABS_BRAKE: set_axis(dv, a, n > 0.4 ? 1 : 0, 0, KEY_RSHIFT); break;             /* LT: run */
    default: break;
    }
}


/* ---------------- userspace USB Xbox pads (usbpad.c) ---------------- */
static struct usbpad_state upad_prev[8];
static void upad_edge(uint32_t prev, uint32_t cur, uint32_t bit, const unsigned char *keys) {
    if ((prev ^ cur) & bit) for (int i = 0; keys[i]; i++) hold(keys[i], cur & bit);
}
static int upad_dir(int v, int thr) { return v < -thr ? -1 : v > thr ? 1 : 0; }
static void upad_axis(int old, int now, unsigned char neg, unsigned char pos) {
    if (old == now) return;
    if (old < 0) hold(neg, false); else if (old > 0) hold(pos, false);
    if (now < 0) hold(neg, true); else if (now > 0) hold(pos, true);
}
static void apply_usbpad(struct usbpad_state *o, const struct usbpad_state *n) {
    static const unsigned char kA[] = {KEY_USE, D_ENTER, 'y', 0}, kB[] = {D_BACKSPACE, 'n', 0}, kX[] = {KEY_FIRE, 0},
        kY[] = {']', 0}, kLB[] = {'[', 0}, kRB[] = {']', 0}, kSt[] = {KEY_ESCAPE, 0}, kBk[] = {D_TAB, 0},
        kLS[] = {KEY_RSHIFT, 0}, kRS[] = {KEY_USE, 0}, kU[] = {KEY_UPARROW, 0}, kD[] = {KEY_DOWNARROW, 0},
        kL[] = {KEY_LEFTARROW, 0}, kR[] = {KEY_RIGHTARROW, 0};
    uint32_t a = o->buttons, b = n->buttons;
    upad_edge(a, b, UP_A, kA); upad_edge(a, b, UP_B, kB); upad_edge(a, b, UP_X, kX); upad_edge(a, b, UP_Y, kY);
    upad_edge(a, b, UP_LB, kLB); upad_edge(a, b, UP_RB, kRB); upad_edge(a, b, UP_START, kSt); upad_edge(a, b, UP_BACK, kBk);
    upad_edge(a, b, UP_LS, kLS); upad_edge(a, b, UP_RS, kRS); upad_edge(a, b, UP_DUP, kU); upad_edge(a, b, UP_DDOWN, kD);
    upad_edge(a, b, UP_DLEFT, kL); upad_edge(a, b, UP_DRIGHT, kR);
    upad_axis(upad_dir(o->lx, 450), upad_dir(n->lx, 450), ',', '.');                       /* left stick X: strafe */
    upad_axis(-upad_dir(o->ly, 350), -upad_dir(n->ly, 350), KEY_UPARROW, KEY_DOWNARROW);  /* left stick Y (up=+): move */
    upad_axis(upad_dir(o->rx, 350), upad_dir(n->rx, 350), KEY_LEFTARROW, KEY_RIGHTARROW); /* right stick X: turn */
    upad_axis(o->rt > 400, n->rt > 400, 0, KEY_FIRE);                                     /* RT: fire */
    upad_axis(o->lt > 400, n->lt > 400, 0, KEY_RSHIFT);                                   /* LT: run */
    *o = *n;
}
static void pump_usbpads(void) {
    bool combo = ev_start && ev_select;                       /* View + Start held 2 s = quit */
    for (int i = 0; i < usbpad_max() && i < 8; i++) {
        struct usbpad_state cur; usbpad_get(i, &cur);
        apply_usbpad(&upad_prev[i], &cur);
        if ((cur.buttons & (UP_START | UP_BACK)) == (UP_START | UP_BACK)) combo = true;
    }
    uint32_t now = DG_GetTicksMs();
    if (!combo) pad_combo_since = 0;
    else if (!pad_combo_since) pad_combo_since = now ? now : 1;
    else if (now - pad_combo_since > 2000) { fprintf(stderr, "lapis: quit (View+Start)\n"); lapis_quit(); }
}

static void pump_evdev(void) {
    uint32_t now = DG_GetTicksMs();
    if (now - last_scan > 2000 || last_scan == 0) { last_scan = now ? now : 1; scan_devices(); if (!getenv("LAPIS_DOOM_NO_USBPAD")) usbpad_scan(); }
    for (int i = 0; i < ndev; i++) {
        struct input_event ev[64];
        for (;;) {
            ssize_t n = read(devs[i].fd, ev, sizeof ev);
            if (n < 0) { if (errno == EAGAIN) break; drop_dev(i); i--; goto next; }
            if (n == 0) break;
            for (size_t k = 0; k < n / sizeof *ev; k++) {
                struct input_event *e = &ev[k];
                if (e->type == EV_KEY && e->value != 2) {
                    if (devs[i].pad) pad_button(e->code, e->value);
                    else { unsigned char dk = kbd_key(e->code); if (dk) hold(dk, e->value); }
                } else if (e->type == EV_ABS && devs[i].pad) pad_axis(&devs[i], e->code, e->value);
            }
        }
    next:;
    }
}

/* ---------------- rendering: see lapis_render.c ---------------- */
#include "lapis_render.h"
extern int lapis_extralight, lapis_fullbright;
static struct lr_opts ropts;
static long frame_no, dump_at = -1;
static const char *dump_path;

void DG_DrawFrame(void) {
    lr_render((const uint32_t *)DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY, packet + HDR, &ropts);
    if (dump_path && ++frame_no == dump_at) {
        FILE *f = fopen(dump_path, "wb");
        if (f) { fwrite(DG_ScreenBuffer, 4, DOOMGENERIC_RESX * DOOMGENERIC_RESY, f); fwrite(packet + HDR, 1, PIX_BYTES, f); fclose(f); }
    }
    have_frame = true;
    pump_socket();
    send_frame();
}

void DG_Init(void) {
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const char *fd = getenv("LAPIS_FRAME_FD");
    if (!fd) { fprintf(stderr, "lapis-doom: LAPIS_FRAME_FD not set; start via the lapis-doom launcher\n"); exit(1); }
    sock = atoi(fd);
    trace = getenv("LAPIS_DOOM_TRACE") != NULL;
    const char *s = getenv("LAPIS_WINDOW_ID"); win_id = s ? strtoul(s, 0, 10) : 0;
    s = getenv("LAPIS_WINDOW_GEN"); win_gen = s ? strtoul(s, 0, 10) : 0;
    if (!win_id || !win_gen) { fprintf(stderr, "lapis-doom: window id/generation missing\n"); exit(1); }
    lr_defaults(&ropts); lr_from_env(&ropts);
    const char *lt = getenv("LAPIS_DOOM_LIGHT");      /* full | 0..4 extra light levels (default 2) */
    if (lt && !strcmp(lt, "full")) lapis_fullbright = 1; else lapis_extralight = lt ? atoi(lt) : 2;
    dump_path = getenv("LAPIS_DOOM_DUMP"); if (getenv("LAPIS_DOOM_DUMP_FRAME")) dump_at = atol(getenv("LAPIS_DOOM_DUMP_FRAME"));
    signal(SIGPIPE, SIG_IGN);
    scan_devices();
    fprintf(stderr, "lapis-doom: window %u/%u, drawing %dx%d at x=%d, dither %d, light %s\n", win_id, win_gen, ropts.out_w, ropts.out_h, ropts.out_x, ropts.dither, lapis_fullbright ? "full" : "boost");
}

void DG_SleepMs(uint32_t ms) {
    struct pollfd p = { .fd = sock, .events = POLLIN };
    poll(&p, 1, ms);
    pump_socket();
}

uint32_t DG_GetTicksMs(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    pump_socket();
    pump_evdev();
    pump_usbpads();
    run_pulses();
    check_quit_combos();
    if (kq_r == kq_w) return 0;
    uint16_t k = kq[kq_r++ % KQ];
    *pressed = k >> 8; *key = k & 0xff;
    return 1;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
    return 0;
}
