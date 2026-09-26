#ifndef AM2R_WII_CONTROLS_H
#define AM2R_WII_CONTROLS_H

#include <stdbool.h>

struct Runner;
struct VMContext;

/* The options screens fitted to Wii controllers and a TV, without touching the game's data. */
void WiiControls_hookScripts(struct VMContext *ctx); /* after the call cache is built */
void WiiControls_frame(struct Runner *runner);       /* after the step, before the draw */
bool WiiControls_menuBack(struct Runner *runner);    /* puts an options screen's cursor on its Back row */

#endif
