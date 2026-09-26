#ifndef AM2R_WII_INPUT_H
#define AM2R_WII_INPUT_H

#include <stdbool.h>

struct Runner;
struct VMContext;

/* The controller the player last used; it picks the button names the menus show. */
typedef enum { WII_PAD_CLASSIC, WII_PAD_GAMECUBE, WII_PAD_NUNCHUK } WiiPad;

void WiiInput_init(void);
void WiiInput_poll(struct Runner *runner);
bool WiiInput_shouldQuit(void);
WiiPad WiiInput_pad(void);
bool WiiInput_hasRumble(void);
void WiiInput_rumble(float left, float right); /* gamepad_set_vibration's levels, 0..1 */
void WiiInput_stopRumble(void);
void WiiInput_applyLayout(struct VMContext *vm);
void WiiInput_flipMenus(struct VMContext *vm); /* swaps the menu bindings to/from their Nunchuk slots */

#endif
