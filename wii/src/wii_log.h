#ifndef AM2R_WII_LOG_H
#define AM2R_WII_LOG_H

#include <stdbool.h>

bool WiiLog_open(const char *path);
void WiiLog_close(void);

#endif
