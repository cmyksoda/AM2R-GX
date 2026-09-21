#ifndef AM2R_WII_WARP_H
#define AM2R_WII_WARP_H

#include <stdint.h>

#define WARP_SETTLE_FRAMES 40 /* let the landing room finish its first frames before leaving it */

struct Runner;

/* 1.1 and 1.5.5 number objects and rooms differently, so these come from the data by name. */
typedef struct {
	int32_t character, camera; /* oCharacter, oCamera; -1 if the data has no such object */
	int32_t landingRoom;       /* rm_a0h01 */
} Am2rIds;
const Am2rIds *Wii_am2rIds(struct Runner *runner);

/* Sets global.<name> if the game has such a variable; silently does nothing otherwise. */
void Wii_setGlobal(struct Runner *runner, const char *name, int value);
void Wii_tryWarp(struct Runner *runner);
void Wii_warpTo(struct Runner *runner, int room, int x, int y, int camx, int camy);

/* Pin AM2R's control-prompt profile to gamepad (the Wii is controller-only, but our
 * keyboard synthesis makes the game keep flipping back to keyboard glyphs). */
void Wii_forceGamepadProfile(struct Runner *runner);

#endif
