#ifndef AM2R_WII_PATCHER_H
#define AM2R_WII_PATCHER_H

#include <stdbool.h>

/* First boot with the player's AM2R_11.zip beside the app and no game/data.win: unpack the
 * 1.1 data, apply the community 1.5.5 delta, repack the textures for the GPU, copy the loose
 * files. True when game/data.win exists afterwards (or already did). */
bool WiiPatcher_run(void);

#endif
