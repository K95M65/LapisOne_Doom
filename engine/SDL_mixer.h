#ifndef LAPIS_SDL_MIXER_SHIM_H
#define LAPIS_SDL_MIXER_SHIM_H
#include "SDL.h"
#define AUDIO_S16SYS 0x8010
extern int lapis_snd_rate;    /* set by i_lapissound.c */
extern void (*lapis_music_hook)(void *, Uint8 *, int);
static inline int Mix_QuerySpec(int *f, Uint16 *fmt, int *ch) { *f = lapis_snd_rate; *fmt = AUDIO_S16SYS; *ch = 2; return lapis_snd_rate > 0; }
static inline void Mix_HookMusic(void (*fn)(void *, Uint8 *, int), void *u) { (void)u; lapis_music_hook = fn; }
static inline int Mix_OpenAudio(int f, Uint16 fmt, int ch, int c) { (void)f; (void)fmt; (void)ch; (void)c; return -1; }
static inline const char *Mix_GetError(void) { return "no SDL on Lapis"; }
static inline void Mix_CloseAudio(void) {}
#endif
