#ifndef AM2R_WII_INPUT_H
#define AM2R_WII_INPUT_H

#include <stdbool.h>

struct Runner;
struct VMContext;

/* The controller the player last used; it picks the button names the menus show. */
typedef enum { WII_PAD_CLASSIC, WII_PAD_GAMECUBE, WII_PAD_NUNCHUK, WII_PAD_REMOTE } WiiPad;

void WiiInput_init(void);
void WiiInput_poll(struct Runner *runner);
bool WiiInput_shouldQuit(void);
WiiPad WiiInput_pad(void);
void WiiInput_applyLayout(struct VMContext *vm);

#endif
