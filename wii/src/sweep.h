#ifndef AM2R_WII_SWEEP_H
#define AM2R_WII_SWEEP_H

#include <stdbool.h>

struct Runner;

/* Dolphin-only room sweep: sweep.txt holds "dwell N", "tour 1" and "room x y camx camy [dropX dropY dropEvery]" stops.
 * Absent on real SD cards, so hardware never enters this path. */
void Wii_sweepTick(struct Runner *runner);
bool Wii_sweepFinished(void);
/* Call right after presenting: holds a frame listed in shots.txt on screen for the host to grab. */
void Wii_shotHold(void);
/* Call before the display copy: dumps a shot frame's EFB when the EFBDUMP sentinel exists. */
void Wii_efbDump(void);
/* JUKEBOX marker: once in gameplay, every music track for a few seconds each, then every other sound. */
void Wii_jukeboxTick(struct Runner *runner);
/* MENU marker: reach the Extras menu by menu state, not frame numbers, and shoot it. */
void Wii_menuTick(struct Runner *runner);
/* A hook's own shot, announced like a tape shot so the host grabs (and EFB-dumps) it. */
void Wii_requestShot(int frame, const char *what, bool last);

#endif
