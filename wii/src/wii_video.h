#ifndef AM2R_WII_VIDEO_H
#define AM2R_WII_VIDEO_H

#include <gccore.h>
#include <stdbool.h>

#define WII_FB_W 640
#define WII_FB_H 480

bool WiiVideo_init(void);
void WiiVideo_present(void);
void WiiVideo_setActiveWidth(int w);
GXRModeObj *WiiVideo_rmode(void);
u32 WiiVideo_fbWidth(void);
u32 WiiVideo_efbHeight(void);
/* SYSCONF 16:9: the TV stretches the 640-wide frame, so the game composes its own wide view for it
 * (platform.c, gx_renderer.c); WII_WIDE_PILLARBOX keeps 4:3 instead. */
bool WiiVideo_isWide(void);
void WiiVideo_set240p(bool on); /* from the next frame on */
bool WiiVideo_is240p(void);
/* Text console on its own XFB: the panic screen and the patcher's progress display. */
bool WiiVideo_console(void);
void WiiVideo_panic(const char *title, const char *body, bool showLog);

#endif
