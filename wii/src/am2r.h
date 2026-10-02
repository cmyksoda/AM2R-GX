#ifndef AM2R_WII_AM2R_H
#define AM2R_WII_AM2R_H

#include <stdint.h>

struct Runner;

/* Object index by name, -1 if the data has none. */
int32_t Wii_objectIndex(struct Runner *runner, const char *name);
int32_t Wii_spriteIndex(struct Runner *runner, const char *name);
int32_t Wii_roomIndex(struct Runner *runner, const char *name);
/* A variable's id in one scope (-1 self, -5 global): the VM's name map keeps whichever scope the data lists first. */
int32_t Wii_varId(struct Runner *runner, const char *name, int32_t scope);

/* Pin AM2R's control-prompt profile to gamepad: the Wii is controller-only. */
void Wii_forceGamepadProfile(struct Runner *runner);

/* Clear the widescreen view each frame, except on the item screen: it shows the frozen game from that surface. */
void Wii_setViewClears(struct Runner *runner);

struct VMContext;
/* Native stand-ins for AM2R scripts whose GML is slow on a Wii and whose effect is simple (reset_map, draw_cool_text,
 * draw_map_surf). */
void Wii_hookAm2rScripts(struct VMContext *ctx);

struct DataWin;
/* Put the Wii port's credits ahead of AM2R's in the ending roll. */
void Wii_addCredits(struct DataWin *dw);

/* Tell the renderer which pages the next seconds will draw (title, file select, intro, play, credits), once per change. */
void Wii_wishPages(struct Runner *runner);

#endif
