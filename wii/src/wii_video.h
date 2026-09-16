#ifndef AM2R_WII_VIDEO_H
#define AM2R_WII_VIDEO_H

#include <gccore.h>
#include <stdbool.h>

#define WII_FB_W 640
#define WII_FB_H 480

bool WiiVideo_init(void);
void WiiVideo_present(void);
/* CopyDisp without clear/swap so a later CopyTex sees the EFB. */
void WiiVideo_touchEfb(void);
GXRModeObj *WiiVideo_rmode(void);
u32 WiiVideo_fbWidth(void);
u32 WiiVideo_efbHeight(void);
/* SYSCONF 16:9. Game still fills 640×480 (stretch). Pillarbox is WII_WIDE_PILLARBOX. */
bool WiiVideo_isWide(void);
/* Blind-debug fallback: bring up a text console and show a fatal message on the TV. */
void WiiVideo_panic(const char *title, const char *body);

#endif
