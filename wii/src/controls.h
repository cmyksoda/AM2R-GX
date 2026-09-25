#ifndef AM2R_WII_CONTROLS_H
#define AM2R_WII_CONTROLS_H

struct Runner;
struct VMContext;

/* Options > Controls fitted to Wii controllers, without touching the game's data. */
void WiiControls_hookScripts(struct VMContext *ctx); /* after the call cache is built */
void WiiControls_frame(struct Runner *runner);       /* after the step, before the draw */

#endif
