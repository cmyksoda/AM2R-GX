#ifndef AM2R_WII_INPUT_H
#define AM2R_WII_INPUT_H

#include <stdbool.h>

struct Runner;

void WiiInput_init(void);
void WiiInput_poll(struct Runner *runner);
bool WiiInput_shouldQuit(void);

#endif
