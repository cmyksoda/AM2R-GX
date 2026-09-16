#include "wii_video.h"

#include <malloc.h>
#include <ogc/conf.h>
#include <stdio.h>
#include <string.h>

#define FIFO_SIZE (1024 * 1024)

static GXRModeObj *rmode;
static void *xfb[2];
static u32 fb;
static void *fifo;
static bool inited;
static bool first_present = true;
static bool wide;

static GXRModeObj *pick_mode(void)
{
	GXRModeObj *m = VIDEO_GetPreferredMode(NULL);

	if (m->xfbHeight > 480) {
		if (CONF_GetProgressiveScan() && VIDEO_HaveComponentCable())
			m = &TVEurgb60Hz480Prog;
		else
			m = &TVEurgb60Hz480IntDf;
	}
	return m;
}

bool WiiVideo_init(void)
{
	f32 yscale;
	u32 xfbHeight;
	GXColor clear = {0, 0, 0, 0};

	if (inited)
		return true;

	VIDEO_Init();
	rmode = pick_mode();
	/* 4:3 TV: fill. 16:9 TV: same fill = horizontal stretch. Pillarbox later. */
	wide = (CONF_GetAspectRatio() == CONF_ASPECT_16_9);

	fb = 0;
	xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	if (!xfb[0] || !xfb[1])
		return false;

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb[fb]);
	VIDEO_SetBlack(true);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	fifo = memalign(32, FIFO_SIZE);
	if (!fifo)
		return false;
	memset(fifo, 0, FIFO_SIZE);
	GX_Init(fifo, FIFO_SIZE);
	GX_SetCopyClear(clear, 0x00ffffff);
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
	xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_FALSE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering,
			((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
	if (rmode->aa)
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	else
		/* RGB8 EFB has no alpha; gui_surface / oCharacter.surf need draw_clear_alpha. */
		GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetDispCopyGamma(GX_GM_1_0);
	GX_SetDither(GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);

	inited = true;
	return true;
}

void WiiVideo_touchEfb(void)
{
	GX_DrawDone();
	GX_CopyDisp(xfb[fb], GX_FALSE);
	GX_Flush();
}

void WiiVideo_present(void)
{
	GX_DrawDone();
	GX_CopyDisp(xfb[fb], GX_TRUE);
	GX_Flush();
	VIDEO_SetNextFramebuffer(xfb[fb]);
	if (first_present) {
		VIDEO_SetBlack(false);
		first_present = false;
	}
	VIDEO_Flush();
	VIDEO_WaitVSync();
	fb ^= 1;
}

GXRModeObj *WiiVideo_rmode(void)
{
	return rmode;
}

u32 WiiVideo_fbWidth(void)
{
	return rmode ? rmode->fbWidth : WII_FB_W;
}

u32 WiiVideo_efbHeight(void)
{
	return rmode ? rmode->efbHeight : WII_FB_H;
}

bool WiiVideo_isWide(void)
{
	return wide;
}

/* Self-contained: takes a fresh XFB so it works even if GX/video never came up. */
void WiiVideo_panic(const char *title, const char *body)
{
	static void *pxfb;
	GXRModeObj *m;

	VIDEO_Init();
	m = pick_mode();
	pxfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(m));
	if (!pxfb)
		return;

	console_init(pxfb, 20, 20, m->fbWidth, m->xfbHeight, m->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(m);
	VIDEO_SetNextFramebuffer(pxfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (m->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	printf("\x1b[2J\n\n  AM2R GX -- %s\n\n", title ? title : "fatal error");
	if (body && *body)
		printf("  %s\n\n", body);
	printf("  Log: sd:/apps/am2r/profile.txt\n");
	printf("  Hold HOME to return to the loader.\n");
}
