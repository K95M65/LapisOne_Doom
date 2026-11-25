#define _GNU_SOURCE
// Lapis One sound backend for doomgeneric (GPLv2, like Chocolate Doom).
// SFX: own 16-channel mixer of the WAD's DMX sounds. Music: Chocolate Doom's OPL (Nuked OPL3)
// synth through a tiny SDL shim. Output: signed 16-bit stereo PCM piped into PipeWire's
// `pw-cat` (fallback: ALSA `aplay`). No shared libraries needed, so the binary stays static.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include "SDL_mixer.h"
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_misc.h"
#include "deh_str.h"

int lapis_snd_rate = 0;
void (*lapis_music_hook)(void *, Uint8 *, int) = NULL;
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

#define RATE 44100
#define NCH 16
#define BLOCK 256                       /* frames per mix block: 5.8 ms */

typedef struct { const uint8_t *data; uint32_t len, rate; } snd_t;
typedef struct { const snd_t *s; uint64_t pos, step; int lvol, rvol; volatile int on; } chan_t;
static chan_t chans[NCH];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t th; static volatile int running;
static FILE *out; static boolean use_prefix;
static int16_t musbuf[BLOCK * 2];

static FILE *open_output(void) {
    const char *cmd = getenv("LAPIS_DOOM_AUDIO_CMD");
    char rt[64];
    if (!getenv("XDG_RUNTIME_DIR")) { snprintf(rt, sizeof rt, "/run/user/%d", (int)getuid()); setenv("XDG_RUNTIME_DIR", rt, 1); }
    static const char *cands[] = {
        "exec pw-cat --playback --raw --format s16 --rate 44100 --channels 2 --latency 1024 --media-role Game "
        "-P '{ node.name=lapis-doom media.name=DOOM application.name=DOOM }' - 2>/dev/null",
        "exec aplay -q -t raw -f S16_LE -r 44100 -c 2 --buffer-time=60000 2>/dev/null", NULL };
    for (int i = 0; cmd ? i < 1 : cands[i] != NULL; i++) {
        const char *c = cmd ? cmd : cands[i];
        char bin[32]; sscanf(c + (strncmp(c, "exec ", 5) ? 0 : 5), "%31s", bin);
        char probe[96]; snprintf(probe, sizeof probe, "command -v %s >/dev/null 2>&1", bin);
        if (!cmd && system(probe) != 0) continue;
        FILE *f = popen(c, "w");
        if (f) { fcntl(fileno(f), F_SETPIPE_SZ, 8192); fprintf(stderr, "sound: output via %s\n", bin); return f; }
    }
    fprintf(stderr, "sound: no audio output (pw-cat/aplay not found)\n");
    return NULL;
}

static void *mixer(void *arg) {
    (void)arg;
    int32_t acc[BLOCK * 2]; int16_t pcm[BLOCK * 2];
    while (running) {
        memset(acc, 0, sizeof acc);
        if (lapis_music_hook) {
            memset(musbuf, 0, sizeof musbuf);
            lapis_music_hook(NULL, (Uint8 *)musbuf, sizeof musbuf);
            for (int i = 0; i < BLOCK * 2; i++) acc[i] = musbuf[i] * 5 / 2;   /* OPL output is quiet next to SFX */
        }
        pthread_mutex_lock(&mu);
        for (int c = 0; c < NCH; c++) {
            chan_t *ch = &chans[c]; if (!ch->on) continue;
            for (int i = 0; i < BLOCK; i++) {
                uint32_t idx = ch->pos >> 16;
                if (idx >= ch->s->len) { ch->on = 0; break; }
                int v = ((int)ch->s->data[idx] - 128) << 8;
                acc[2 * i] += v * ch->lvol >> 8; acc[2 * i + 1] += v * ch->rvol >> 8;
                ch->pos += ch->step;
            }
        }
        pthread_mutex_unlock(&mu);
        for (int i = 0; i < BLOCK * 2; i++) { int v = acc[i]; pcm[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
        if (fwrite(pcm, sizeof pcm, 1, out) != 1) { fprintf(stderr, "sound: output closed\n"); break; }
        fflush(out);
    }
    return NULL;
}

static boolean S_Init(boolean prefix) {
    use_prefix = prefix;
    if (getenv("LAPIS_DOOM_NOSOUND")) return false;
    signal(SIGPIPE, SIG_IGN);
    out = open_output(); if (!out) return false;
    lapis_snd_rate = RATE; running = 1;
    pthread_create(&th, NULL, mixer, NULL);
    return true;
}
static void S_Shutdown(void) { if (!running) return; running = 0; pthread_join(th, NULL); pclose(out); out = NULL; lapis_snd_rate = 0; }

static int S_GetSfxLumpNum(sfxinfo_t *sfx) {
    char name[9];
    if (sfx->link) sfx = sfx->link;
    if (use_prefix) M_snprintf(name, sizeof name, "ds%s", DEH_String(sfx->name));
    else M_StringCopy(name, DEH_String(sfx->name), sizeof name);
    return W_CheckNumForName(name);
}

static const snd_t *load(sfxinfo_t *sfx) {
    if (sfx->driver_data) return sfx->driver_data;
    int lump = sfx->lumpnum; if (lump < 0) return NULL;
    const uint8_t *d = W_CacheLumpNum(lump, PU_STATIC); int n = W_LumpLength(lump);
    if (n < 8 || d[0] != 3 || d[1] != 0) return NULL;
    uint32_t rate = d[2] | d[3] << 8, len = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24;
    if (len > (uint32_t)n - 8 || len <= 32) return NULL;
    snd_t *s = malloc(sizeof *s); s->data = d + 8 + 16; s->len = len - 32; s->rate = rate ? rate : 11025;
    sfx->driver_data = s; return s;
}

static void setvol(chan_t *c, int vol, int sep) {   /* vol 0..127, sep 0..254 (128 centre) */
    int l = (254 - sep) * vol / 127, r = sep * vol / 127;
    c->lvol = l * 2; c->rvol = r * 2;               /* ~0..254 => up to ~1.0 gain */
}
static void S_UpdateParams(int ch, int vol, int sep) { if (ch < 0 || ch >= NCH) return; pthread_mutex_lock(&mu); setvol(&chans[ch], vol, sep); pthread_mutex_unlock(&mu); }
static int S_Start(sfxinfo_t *sfx, int ch, int vol, int sep) {
    if (ch < 0 || ch >= NCH) return -1;
    const snd_t *s = load(sfx); if (!s) return -1;
    pthread_mutex_lock(&mu);
    chan_t *c = &chans[ch]; c->s = s; c->pos = 0; c->step = ((uint64_t)s->rate << 16) / RATE; setvol(c, vol, sep); c->on = 1;
    pthread_mutex_unlock(&mu);
    return ch;
}
static void S_Stop(int ch) { if (ch >= 0 && ch < NCH) chans[ch].on = 0; }
static boolean S_IsPlaying(int ch) { return ch >= 0 && ch < NCH && chans[ch].on; }
static void S_Update(void) {}
static void S_Cache(sfxinfo_t *s, int n) { for (int i = 0; i < n; i++) { s[i].lumpnum = S_GetSfxLumpNum(&s[i]); load(&s[i]); } }

static snddevice_t devs[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };
sound_module_t DG_sound_module = { devs, sizeof devs / sizeof *devs, S_Init, S_Shutdown, S_GetSfxLumpNum, S_Update,
                                   S_UpdateParams, S_Start, S_Stop, S_IsPlaying, S_Cache };

extern music_module_t music_opl_module;
music_module_t DG_music_module;
__attribute__((constructor)) static void pick_music(void) { DG_music_module = music_opl_module; }
