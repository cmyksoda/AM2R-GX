#ifndef AM2R_WII_SWEEP_H
#define AM2R_WII_SWEEP_H

#include <stdbool.h>

struct Runner;

/* Dolphin-only room sweep: sweep.txt lists "room x y camx camy [dropX dropY dropEvery]" stops.
 * Absent on real SD cards, so hardware never enters this path. */
void Wii_sweepTick(struct Runner *runner);
bool Wii_sweepFinished(void);
/* Call right after presenting: holds a frame listed in shots.txt on screen for the host to grab. */
void Wii_shotHold(void);

#endif
