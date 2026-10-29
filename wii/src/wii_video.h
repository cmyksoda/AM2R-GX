#ifndef AM2R_WII_VIDEO_H
#define AM2R_WII_VIDEO_H

#include <gccore.h>
#include <stdbool.h>

#define WII_FB_W 640
#define WII_FB_H 480

bool WiiVideo_init(void);
void WiiVideo_present(void);
uint64_t WiiVideo_gpuWaitNs(void); /* how long the last frame waited for the GPU */
/* At 60 Hz a present leaves the GPU drawing and copying the frame while the next step runs. Anything that reads or frees
 * what the GPU may still be using, or waits for it with GX_DrawDone, settles that frame first. */
void WiiVideo_settle(void);
void WiiVideo_drawDone(void); /* GX_DrawDone, after settling */
bool WiiVideo_paceStep(u64 stepNs); /* false: not a 50 Hz console, sleep as usual */
void WiiVideo_setActiveWidth(int w);
GXRModeObj *WiiVideo_rmode(void);
u32 WiiVideo_fbWidth(void);
u32 WiiVideo_efbHeight(void);
/* SYSCONF 16:9: the TV stretches the 640-wide frame, so the game composes its own wide view for it
 * (platform.c, gx_renderer.c). */
bool WiiVideo_isWide(void);
void WiiVideo_set240p(bool on); /* from the next frame on */
/* Text console on its own XFB: the panic screen and the patcher's progress display. */
bool WiiVideo_console(void);
void WiiVideo_panic(const char *title, const char *body, bool showLog);

#endif
