// Userspace Xbox 360 / One / Series USB controller driver over Linux usbfs
// (/dev/bus/usb). For kernels without xpad (the Lapis One kernel has
// CONFIG_INPUT_JOYSTICK unset and no module support). No libusb needed:
// talks the usbfs ioctls directly, so the binary stays fully static.
#include "usbpad.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#define MAXPADS 4
struct pad {
    int used, fd, kind, iface, ep_in, ep_out, vid, pid;
    char path[64];
    uint8_t gip_seq;
    long last_init;
    pthread_t th;
    volatile int alive;
    struct usbpad_state st;
};
static struct pad pads[MAXPADS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int usbfs_ok = -1;     /* -1 unknown, 0 not openable, 1 ok */
static int warned;

/* ---------- report parsers (pure, unit-tested) ---------- */
static int16_t le16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

int usbpad_parse_360(const uint8_t *r, int n, struct usbpad_state *s) {
    if (n < 14 || r[0] != 0x00 || r[1] < 0x14) return 0;
    uint32_t b = 0;
    if (r[2] & 0x01) b |= UP_DUP;   if (r[2] & 0x02) b |= UP_DDOWN;
    if (r[2] & 0x04) b |= UP_DLEFT; if (r[2] & 0x08) b |= UP_DRIGHT;
    if (r[2] & 0x10) b |= UP_START; if (r[2] & 0x20) b |= UP_BACK;
    if (r[2] & 0x40) b |= UP_LS;    if (r[2] & 0x80) b |= UP_RS;
    if (r[3] & 0x01) b |= UP_LB;    if (r[3] & 0x02) b |= UP_RB;
    if (r[3] & 0x04) b |= UP_GUIDE;
    if (r[3] & 0x10) b |= UP_A;     if (r[3] & 0x20) b |= UP_B;
    if (r[3] & 0x40) b |= UP_X;     if (r[3] & 0x80) b |= UP_Y;
    s->buttons = b;
    s->lt = r[4] * 1000 / 255; s->rt = r[5] * 1000 / 255;
    s->lx = le16(r + 6) * 1000 / 32768; s->ly = le16(r + 8) * 1000 / 32768;
    s->rx = le16(r + 10) * 1000 / 32768; s->ry = le16(r + 12) * 1000 / 32768;
    return 1;
}

/* returns 1 on input report, 2 if guide packet (updates guide bit), 0 otherwise */
int usbpad_parse_gip(const uint8_t *r, int n, struct usbpad_state *s) {
    if (n >= 5 && r[0] == 0x07) {           /* guide button */
        if (r[4] & 1) s->buttons |= UP_GUIDE; else s->buttons &= ~UP_GUIDE;
        return 2;
    }
    if (n < 18 || r[0] != 0x20) return 0;
    uint32_t b = s->buttons & UP_GUIDE;
    if (r[4] & 0x04) b |= UP_START; if (r[4] & 0x08) b |= UP_BACK;
    if (r[4] & 0x10) b |= UP_A;     if (r[4] & 0x20) b |= UP_B;
    if (r[4] & 0x40) b |= UP_X;     if (r[4] & 0x80) b |= UP_Y;
    if (r[5] & 0x01) b |= UP_DUP;   if (r[5] & 0x02) b |= UP_DDOWN;
    if (r[5] & 0x04) b |= UP_DLEFT; if (r[5] & 0x08) b |= UP_DRIGHT;
    if (r[5] & 0x10) b |= UP_LB;    if (r[5] & 0x20) b |= UP_RB;
    if (r[5] & 0x40) b |= UP_LS;    if (r[5] & 0x80) b |= UP_RS;
    s->buttons = b;
    s->lt = (uint16_t)le16(r + 6) * 1000 / 1023; s->rt = (uint16_t)le16(r + 8) * 1000 / 1023;
    s->lx = le16(r + 10) * 1000 / 32768; s->ly = le16(r + 12) * 1000 / 32768;
    s->rx = le16(r + 14) * 1000 / 32768; s->ry = le16(r + 16) * 1000 / 32768;
    return 1;
}

/* ---------- usbfs plumbing ---------- */
static int xfer(struct pad *p, int ep, void *buf, int len, int timeout_ms) {
    struct usbdevfs_bulktransfer bt = { .ep = ep, .len = len, .timeout = timeout_ms, .data = buf };
    return ioctl(p->fd, USBDEVFS_BULK, &bt);
}

static void gip_send(struct pad *p, const uint8_t *pkt, int len) {
    uint8_t b[64]; memcpy(b, pkt, len);
    if (len > 2) { b[2] = p->gip_seq++; if (!p->gip_seq) p->gip_seq = 1; }
    if (xfer(p, p->ep_out, b, len, 200) < 0) fprintf(stderr, "usbpad: send 0x%02x failed: %s\n", b[0], strerror(errno));
}

static void gip_init(struct pad *p) {
    static const uint8_t power_on[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };
    static const uint8_t s_init[]   = { 0x05, 0x20, 0x00, 0x0f, 0x06 };
    static const uint8_t led_on[]   = { 0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14 };
    static const uint8_t no_rumble[] = { 0x09, 0x00, 0x00, 0x09, 0x00, 0x0f, 0, 0, 0, 0, 0xff, 0x00, 0xeb };
    static const uint8_t elite2_extra[] = { 0x4d, 0x10, 0x00, 0x02, 0x07, 0x00 };  /* xpad: Elite 2 extra input */
    if (getenv("LAPIS_DOOM_PADTRACE")) fprintf(stderr, "usbpad: GIP init %04x:%04x\n", p->vid, p->pid);
    gip_send(p, power_on, sizeof power_on);
    gip_send(p, s_init, sizeof s_init);
    if (p->vid == 0x045e && p->pid == 0x0b00) gip_send(p, elite2_extra, sizeof elite2_extra);
    gip_send(p, led_on, sizeof led_on);
    gip_send(p, no_rumble, sizeof no_rumble);
}

static void *reader(void *arg) {
    struct pad *p = arg;
    uint8_t r[64];
    struct usbpad_state st = {0};
    if (p->kind == USBPAD_ONE) gip_init(p);   /* also re-sent when the pad announces itself */
    while (p->alive) {
        int n = xfer(p, p->ep_in, r, sizeof r, 250);
        if (n < 0) {
            if (errno == ETIMEDOUT || errno == EAGAIN) continue;
            fprintf(stderr, "usbpad: read error on %s: %s\n", p->path, strerror(errno));
            break;                                    /* unplugged (ENODEV) or error */
        }
        int got = 0;
        if (getenv("LAPIS_DOOM_PADTRACE")) { fprintf(stderr, "usbpad rx %d:", n); for (int k = 0; k < n && k < 20; k++) fprintf(stderr, " %02x", r[k]); fprintf(stderr, "\n"); }
        if (p->kind == USBPAD_360) got = usbpad_parse_360(r, n, &st);
        else {
            if (n >= 4 && (r[1] & 0x10) && r[0] != 0x20) {   /* packet asks for an ack */
                uint8_t ack[13] = { 0x01, 0x20, r[2], 0x09, 0x00, r[0], 0x20, r[3], 0, 0, 0, 0, 0 };
                xfer(p, p->ep_out, ack, sizeof ack, 200);
            }
            if (n >= 1 && r[0] == 0x02) {                 /* announce -> (re)send power-on, at most every 400 ms */
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                long now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
                if (now - p->last_init > 400) { p->last_init = now; gip_init(p); }
            }
            got = usbpad_parse_gip(r, n, &st);
        }
        if (got) { pthread_mutex_lock(&mu); st.connected = 1; p->st = st; pthread_mutex_unlock(&mu); }
    }
    pthread_mutex_lock(&mu);
    fprintf(stderr, "usbpad: %s disconnected\n", p->path);
    ioctl(p->fd, USBDEVFS_RELEASEINTERFACE, &p->iface);
    close(p->fd); p->used = 0; p->alive = 0; memset(&p->st, 0, sizeof p->st);
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* Parse the descriptors usbfs returns on read(): device desc then config desc(s). */
static int probe(const char *path) {
    for (int i = 0; i < MAXPADS; i++) if (pads[i].used && !strcmp(pads[i].path, path)) return 0;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES && !warned) { warned = 1; fprintf(stderr, "usbpad: no permission to open %s (USB pads disabled until fixed)\n", path); }
        if (errno == EACCES && usbfs_ok < 0) usbfs_ok = 0;
        return 0;
    }
    usbfs_ok = 1;
    uint8_t d[4096]; int n = read(fd, d, sizeof d);
    if (n < 18 || d[1] != 1) { close(fd); return 0; }
    int vid = d[8] | d[9] << 8, pid = d[10] | d[11] << 8;
    int kind = 0, iface = -1, ep_in = 0, ep_out = 0, cur_if = -1, cur_kind = 0;
    for (int o = 18; o + 2 <= n && d[o] >= 2; o += d[o]) {
        if (d[o + 1] == 4 && o + 9 <= n) {            /* interface */
            cur_if = d[o + 2]; cur_kind = 0;
            if (d[o + 3] != 0) continue;              /* alt setting 0 only */
            if (d[o + 5] == 0xff && d[o + 6] == 0x5d && d[o + 7] == 0x01) cur_kind = USBPAD_360;
            if (d[o + 5] == 0xff && d[o + 6] == 0x47 && d[o + 7] == 0xd0) cur_kind = USBPAD_ONE;
            if (cur_kind && !kind) { kind = cur_kind; iface = cur_if; }
        } else if (d[o + 1] == 5 && o + 7 <= n && kind && cur_if == iface && cur_kind) {
            int ea = d[o + 2], type = d[o + 3] & 3;
            if (type == 3 || type == 2) { if (ea & 0x80) { if (!ep_in) ep_in = ea; } else if (!ep_out) ep_out = ea; }
        }
        if (kind && ep_in && ep_out) break;
    }
    if (!kind || !ep_in || (kind == USBPAD_ONE && !ep_out)) { close(fd); return 0; }
    struct usbdevfs_ioctl dis = { .ifno = iface, .ioctl_code = USBDEVFS_DISCONNECT, .data = NULL };
    ioctl(fd, USBDEVFS_IOCTL, &dis);                  /* detach any kernel driver; ignore errors */
    if (kind == USBPAD_ONE && !getenv("LAPIS_DOOM_NO_PADRESET")) {
        /* A GIP pad left powered-on by an earlier session can go silent (no reports, no
           heartbeats) and ignore power-on. A port reset makes it re-announce (0x02) cleanly. */
        if (ioctl(fd, USBDEVFS_RESET, 0) < 0) fprintf(stderr, "usbpad: reset %s failed: %s\n", path, strerror(errno));
        ioctl(fd, USBDEVFS_IOCTL, &dis);
    }
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
        fprintf(stderr, "usbpad: %s %04x:%04x claim failed: %s\n", path, vid, pid, strerror(errno));
        close(fd); return 0;
    }
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAXPADS; i++) if (!pads[i].used) {
        struct pad *p = &pads[i];
        memset(p, 0, sizeof *p);
        p->used = 1; p->fd = fd; p->vid = vid; p->pid = pid; p->gip_seq = 1; p->kind = kind; p->iface = iface; p->ep_in = ep_in; p->ep_out = ep_out; p->alive = 1;
        snprintf(p->path, sizeof p->path, "%s", path);
        fprintf(stderr, "usbpad: %s %04x:%04x %s controller (if %d, in 0x%02x out 0x%02x)\n",
                path, vid, pid, kind == USBPAD_360 ? "Xbox 360" : "Xbox One/Series", iface, ep_in, ep_out);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&p->th, &a, reader, p);
        pthread_mutex_unlock(&mu);
        return 1;
    }
    pthread_mutex_unlock(&mu);
    close(fd); return 0;
}

void usbpad_scan(void) {
    DIR *b = opendir("/dev/bus/usb");
    if (!b) { if (usbfs_ok < 0) { usbfs_ok = 0; fprintf(stderr, "usbpad: /dev/bus/usb not available; USB pads disabled\n"); } return; }
    struct dirent *e;
    while ((e = readdir(b))) {
        if (e->d_name[0] == '.') continue;
        char bus[300]; snprintf(bus, sizeof bus, "/dev/bus/usb/%s", e->d_name);
        DIR *dd = opendir(bus); if (!dd) continue;
        struct dirent *f;
        while ((f = readdir(dd))) {
            if (f->d_name[0] == '.') continue;
            char path[600]; snprintf(path, sizeof path, "%s/%s", bus, f->d_name);
            if (strlen(path) < 64) probe(path);
        }
        closedir(dd);
    }
    closedir(b);
}

int usbpad_get(int idx, struct usbpad_state *out) {
    if (idx < 0 || idx >= MAXPADS) return 0;
    pthread_mutex_lock(&mu);
    int ok = pads[idx].used;
    if (ok) *out = pads[idx].st; else memset(out, 0, sizeof *out);
    pthread_mutex_unlock(&mu);
    return ok;
}
int usbpad_max(void) { return MAXPADS; }
