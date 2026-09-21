#ifndef AM2R_WII_SWEEP_H
#define AM2R_WII_SWEEP_H

#include <stdbool.h>

struct Runner;

/* Dolphin-only room sweep: sd:/apps/am2r/game/sweep.txt lists "room x y camx camy" stops.
 * Absent on real SD cards, so hardware never enters this path. */
void Wii_sweepTick(struct Runner *runner);
bool Wii_sweepFinished(void);

#endif
