#ifndef AM2R_WII_HOOKS_H
#define AM2R_WII_HOOKS_H

#include <stdbool.h>

struct Runner;
struct expansion_t;

/* Test hooks: weak no-ops in hooks.c. A local test folder (not part of the repository) supplies real ones,
 * so a clean checkout builds the game exactly as it ships. */
void Wii_hooksBoot(int argc, char **argv);   /* the log is open, the network not yet */
const char *Wii_hooksTape(void);              /* an input tape to replay instead of the controllers, or NULL */
void Wii_hooksBeforeLoop(void);
void Wii_hooksExit(void);                     /* the log is closed */
void Wii_hooksFrame(struct Runner *runner);   /* each frame, before the room is drawn */
bool Wii_hooksEvents(struct Runner *runner);  /* each frame, after the controllers are read; true quits */
void Wii_hooksBeforeCopy(void);               /* the frame is drawn, not yet copied out */
void Wii_hooksAfterPresent(void);
/* what the pads read this frame; sticks = GameCube main X, main Y, C-stick Y */
void Wii_hooksPad(struct Runner *runner, unsigned int *wheld, unsigned short *gcheld, struct expansion_t *exp,
		  signed char *sticks);

#endif
