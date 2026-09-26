#ifndef AM2R_WII_SAFEAREA_H
#define AM2R_WII_SAFEAREA_H

#include <stdint.h>

/* Game rows a CRT hides per edge: the 240p Test Suite's green lines sit at 22 and 457 of 480. */
#define WII_SAFE_ROWS 11

struct Runner;

int32_t WiiSafe_guiSurface(struct Runner *runner);

#endif
