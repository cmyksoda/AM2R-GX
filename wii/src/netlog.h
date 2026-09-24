#ifndef AM2R_WII_NETLOG_H
#define AM2R_WII_NETLOG_H

/* LOGTO=ip:port (a wiiload argument, or a game/LOGTO file): at exit the DOL hands profile.txt and any
 * grabs/efb-*.ppm to a listener on the host, so a measurement run needs no FTP server on the console. */
void NetLog_setTarget(const char *ipport);
void NetLog_send(void);

#endif
