#ifndef AM2R_WII_PATHS_H
#define AM2R_WII_PATHS_H

#include <stdbool.h>

/* Where the game lives: the folder the Homebrew Channel started boot.dol from, on the SD card or a USB
 * drive; sd:/apps/am2r until WiiPaths_init says otherwise. Everything else is built from it. */
extern char wiiAppDir[64];
extern char wiiGameDir[80];
extern char wiiDataWin[96];
extern char wiiDrive[16];

#define WII_APP_DIR wiiAppDir
#define WII_GAME_DIR wiiGameDir
#define WII_DATA_WIN wiiDataWin
#define WII_DRIVE wiiDrive

/* Mounts the drive boot.dol was started from and settles the folder; false when it is not there. */
bool WiiPaths_init(int argc, char **argv);
/* "the SD card" or "the USB drive", for messages. */
const char *WiiPaths_media(void);

#endif
