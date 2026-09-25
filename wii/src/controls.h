#ifndef AM2R_WII_CONTROLS_H
#define AM2R_WII_CONTROLS_H

struct Runner;

/* Options > Controls fitted to Wii controllers, without touching the game's data. */
void WiiControls_frame(struct Runner *runner); /* after the step, before the draw */

#endif
