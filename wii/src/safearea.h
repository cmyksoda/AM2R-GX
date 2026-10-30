#ifndef AM2R_WII_SAFEAREA_H
#define AM2R_WII_SAFEAREA_H

#include <stdbool.h>
#include <stdint.h>

struct Runner;
struct VMContext;

int WiiSafe_rows(void);
int32_t WiiSafe_guiSurface(struct Runner *runner);
int32_t WiiSafe_screenSurface(struct Runner *runner);
bool WiiSafe_ingame(struct Runner *runner);
bool WiiSafe_paused(struct Runner *runner);
int32_t WiiSafe_hudSplit(struct Runner *runner, int32_t width);
float WiiSafe_lift(struct Runner *runner);
float WiiSafe_shift(struct Runner *runner);
void WiiSafe_frame(struct Runner *runner);
void WiiSafe_hookScripts(struct VMContext *ctx);

#endif
