// Copyright (C) 2026 K95M65. Licensed under the GNU GPL v2 (see LICENSE and NOTICE).
/* Minimal SDL shim so Chocolate Doom's OPL code runs on the Lapis mixer (no real SDL). */
#ifndef LAPIS_SDL_SHIM_H
#define LAPIS_SDL_SHIM_H
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
typedef uint8_t Uint8; typedef uint16_t Uint16; typedef uint32_t Uint32;
typedef pthread_mutex_t SDL_mutex; typedef pthread_cond_t SDL_cond;
static inline SDL_mutex *SDL_CreateMutex(void) { SDL_mutex *m = malloc(sizeof *m); pthread_mutex_init(m, NULL); return m; }
static inline int SDL_LockMutex(SDL_mutex *m) { return pthread_mutex_lock(m); }
static inline int SDL_UnlockMutex(SDL_mutex *m) { return pthread_mutex_unlock(m); }
static inline void SDL_DestroyMutex(SDL_mutex *m) { pthread_mutex_destroy(m); free(m); }
static inline SDL_cond *SDL_CreateCond(void) { SDL_cond *c = malloc(sizeof *c); pthread_cond_init(c, NULL); return c; }
static inline int SDL_CondSignal(SDL_cond *c) { return pthread_cond_signal(c); }
static inline int SDL_CondWait(SDL_cond *c, SDL_mutex *m) { return pthread_cond_wait(c, m); }
static inline void SDL_DestroyCond(SDL_cond *c) { pthread_cond_destroy(c); free(c); }
#define SDL_INIT_AUDIO 0x10
static inline int SDL_Init(int f) { (void)f; return 0; }
static inline void SDL_QuitSubSystem(int f) { (void)f; }
static inline void SDL_PauseAudio(int p) { (void)p; }
#endif
