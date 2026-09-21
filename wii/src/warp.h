#ifndef AM2R_WII_WARP_H
#define AM2R_WII_WARP_H

/* AM2R 1.1 data.win indices. 1.5.5 numbers them differently: look them up by name there. */
#define AM2R_OBJ_CHARACTER 266
#define AM2R_OBJ_CAMERA 426
#define AM2R_ROOM_LANDING 21
#define WARP_SETTLE_FRAMES 40 /* let the landing room finish its first frames before leaving it */

struct Runner;
void Wii_tryWarp(struct Runner *runner);
void Wii_warpTo(struct Runner *runner, int room, int x, int y, int camx, int camy);

/* Pin AM2R's control-prompt profile to gamepad (the Wii is controller-only, but our
 * keyboard synthesis makes the game keep flipping back to keyboard glyphs). */
void Wii_forceGamepadProfile(struct Runner *runner);

#endif
