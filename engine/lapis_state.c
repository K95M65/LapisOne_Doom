// Copyright (C) 2026 K95M65. Licensed under the GNU GPL v2 (see LICENSE and NOTICE).
/* Tiny bridge so the backend (which uses stdbool) can ask Doom about its state. */
#include "doomstat.h"
#include "i_system.h"
int lapis_in_game(void) { return gamestate == GS_LEVEL && !menuactive && !demoplayback; }
void lapis_quit(void) { I_Quit(); }
