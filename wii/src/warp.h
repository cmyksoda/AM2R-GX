#ifndef AM2R_WII_WARP_H
#define AM2R_WII_WARP_H

struct Runner;
void Wii_tryWarp(struct Runner *runner);
void Wii_warpTo(struct Runner *runner, int room, int x, int y, int camx, int camy);

/* Pin AM2R's control-prompt profile to gamepad (the Wii is controller-only, but our
 * keyboard synthesis makes the game keep flipping back to keyboard glyphs). */
void Wii_forceGamepadProfile(struct Runner *runner);

#endif
