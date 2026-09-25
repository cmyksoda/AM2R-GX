#ifndef AM2R_WII_LOG_H
#define AM2R_WII_LOG_H

#include <stdbool.h>

bool WiiLog_open(const char *path);
void WiiLog_close(void);
void WiiLog_sync(void);

/* Mirror the log to a USB Gecko in slot B if one answers (Dolphin emulates it over TCP). */
void WiiLog_enableGecko(void);
/* Off while a text console is on screen, so only its own lines show there. */
void WiiLog_setEcho(bool on);

#endif
