#ifndef AM2R_WII_AM2R_H
#define AM2R_WII_AM2R_H

#include <stdint.h>

struct Runner;

/* Object index by name, -1 if the data has none: 1.1 and 1.5.5 number objects differently. */
int32_t Wii_objectIndex(struct Runner *runner, const char *name);

/* Pin AM2R's control-prompt profile to gamepad: the Wii is controller-only. */
void Wii_forceGamepadProfile(struct Runner *runner);

#endif
