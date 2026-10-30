#include "wii_video.h"

#include "wii_paths.h"

#include <malloc.h>
#include <ogc/conf.h>
#include <stdio.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>

#include "log.h"

#define FIFO_SIZE (1024 * 1024)
#define FIELD_NS_50 20000000ull

static GXRModeObj *rmode;
static GXRModeObj modes[2]; /* the console's own mode, and its 240p twin */
static bool low, lowWanted;
static void *xfb[3];
static bool fifty;
static volatile int shownFb, queuedFb = -1;
/* The XFB a 60 Hz present copied into, until the GPU reaches the end of that frame, the retrace count it was presented
 * after, and whether the CPU missed a retrace on it. */
static volatile int copyingFb = -1;
static volatile u32 copyingAt;
static volatile bool copyingCpuLate;
static volatile u32 retraces;
/* Once the GPU finishes a frame after its retrace, frames done in time are held one retrace more, so a heavy stretch
 * costs one repeat instead of one per swing. The next frame the CPU itself is late with ends the lag at no cost. */
static volatile bool lagging;
static volatile int heldFb = -1;
static volatile u32 heldUntil;
/* Retraces that found no new frame to show, and frames replaced before any retrace showed them. */
static volatile u32 repeats, drops;
static u64 settleTicks;
static void *fifo;
static bool inited;
static bool first_present = true;
static bool wide;

/* A PAL console follows its own 50/60 Hz setting (EuRGB60), as licensed games do: a TV that cannot take PAL60
 * belongs to a console set to 50 Hz. 50 Hz is the 480-line EFB scaled onto 576 interlaced lines. */
static GXRModeObj *pick_mode(void)
{
	GXRModeObj *m = VIDEO_GetPreferredMode(NULL);

	if (m->xfbHeight > 480 && CONF_GetEuRGB60() > 0) {
		if (CONF_GetProgressiveScan() > 0 && VIDEO_HaveComponentCable())
			m = &TVEurgb60Hz480Prog;
		else
			m = &TVEurgb60Hz480IntDf;
	} else if (m->xfbHeight > 480) {
		m = &TVPal576IntDfScale;
	}
	return m;
}

/* 240p shows each of the game's 240 lines once (double-strike). PAL 50 Hz has only a 264-line double-strike mode,
 * so that one is cut to 240 lines and centered. */
static void make_240(const GXRModeObj *base, GXRModeObj *out)
{
	switch (base->viTVMode >> 2) {
	case VI_PAL:
		*out = TVPal264Ds;
		out->efbHeight = out->xfbHeight = 240;
		out->viHeight = 480;
		out->viYOrigin = (VI_MAX_HEIGHT_PAL / 2 - 480 / 2) / 2;
		break;
	case VI_MPAL:
		*out = TVMpal240Ds;
		break;
	case VI_EURGB60:
		*out = TVEurgb60Hz240Ds;
		break;
	default:
		*out = TVNtsc240Ds;
		break;
	}
	out->viXOrigin = base->viXOrigin;
	out->viWidth = base->viWidth;
}

static void setup_copy(void)
{
	f32 yscale;
	u32 xfbHeight;

	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
	xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_FALSE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering,
			((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
}

static void queue_fb(int r)
{
	if (queuedFb >= 0)
		drops++;
	queuedFb = r;
	VIDEO_SetNextFramebuffer(xfb[r]);
	if (first_present) {
		VIDEO_SetBlack(false);
		first_present = false;
	}
	VIDEO_Flush();
}

/* At 50 Hz the game still steps 60 times a second: the VI shows whichever finished frame is newest at each
 * retrace, so one frame in six is never shown. A third XFB keeps the copy off the buffer being scanned out. At 60 Hz
 * the third one takes the next frame's copy while the last one waits for its retrace. */
static void on_retrace(u32 count)
{
	retraces = count;
	if (queuedFb >= 0) {
		shownFb = queuedFb;
		queuedFb = -1;
	} else if (!fifty && !first_present) {
		repeats++;
	}
	if (heldFb >= 0 && count >= heldUntil) {
		queue_fb(heldFb);
		heldFb = -1;
	}
}

/* The GPU finished a 60 Hz frame and its copy: it is shown at the next retrace, or held one more while lagging. */
static void on_draw_done(void)
{
	bool late;

	if (copyingFb < 0)
		return;
	late = retraces != copyingAt;
	if (late)
		lagging = true;
	else if (copyingCpuLate)
		lagging = false;
	if (lagging && !late) {
		heldFb = copyingFb;
		heldUntil = copyingAt + 1;
	} else {
		queue_fb(copyingFb);
	}
	copyingFb = -1;
}

void WiiVideo_settle(void)
{
	u64 t;

	if (copyingFb < 0)
		return;
	t = gettime();
	GX_WaitDrawDone();
	settleTicks += gettime() - t;
}

void WiiVideo_drawDone(void)
{
	WiiVideo_settle();
	GX_DrawDone();
}

bool WiiVideo_init(void)
{
	GXColor clear = {0, 0, 0, 0};

	if (inited)
		return true;

	VIDEO_Init();
	modes[0] = *pick_mode();
	make_240(&modes[0], &modes[1]);
	rmode = &modes[0];
	wide = (CONF_GetAspectRatio() == CONF_ASPECT_16_9);

	fifty = rmode->viTVMode >> 2 == VI_PAL;
	xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[2] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	if (!xfb[0] || !xfb[1] || !xfb[2])
		return false;
	VIDEO_SetPostRetraceCallback(on_retrace);

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb[0]);
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
	GX_SetDrawDoneCallback(on_draw_done);
	GX_SetCopyClear(clear, 0x00ffffff);
	setup_copy();
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

/* Whole-frame wall time, present to present: the GML profiler only sees script time. misses: retraces since the last
 * present that showed no new frame, and frames never shown. */
static void frame_stats(u64 t0, u64 t1, u64 t2, u32 misses)
{
	static u64 last, frame, gpu, sync;
	static u32 n, late, seenRepeats, seenDrops;

	if (last) {
		u32 us = ticks_to_microsecs(t2 - last);

		frame += us;
		gpu += ticks_to_microsecs(t1 - t0);
		sync += ticks_to_microsecs(t2 - t1);
		if (us > (fifty ? 20800 : 17500) || misses)
			late++;
		if (++n == 200) {
			logInfo("FRAME avg %.2f ms (gpu wait %.2f, vsync wait %.2f), %u/200 late "
				"(%u repeated, %u dropped)\n", frame / 200000.0, gpu / 200000.0, sync / 200000.0, late,
				repeats - seenRepeats, drops - seenDrops);
			seenRepeats = repeats;
			seenDrops = drops;
			frame = gpu = sync = 0;
			n = late = 0;
		}
	}
	last = t2;
}

static void present_fifty(void)
{
	u32 level;
	int r;

	for (r = 0; r == shownFb || r == queuedFb; r++)
		;
	GX_CopyDisp(xfb[r], GX_TRUE);
	GX_DrawDone();
	_CPU_ISR_Disable(level);
	queuedFb = r;
	VIDEO_SetNextFramebuffer(xfb[r]);
	VIDEO_Flush();
	_CPU_ISR_Restore(level);
	if (first_present) {
		VIDEO_SetBlack(false);
		VIDEO_Flush();
		first_present = false;
	}
}

/* At 50 Hz steps start on retraces too, as at 60 Hz, instead of on a timer that drifts: each retrace earns 1.2 of them,
 * so every fifth one runs two back to back (the first never shown) and the TV sees a steady rhythm. */
bool WiiVideo_paceStep(u64 stepNs)
{
	static bool primed;
	static u32 seen;
	static u64 paid; /* game time the retraces have paid for, in whole ns: a float credit drifted a field late */
	u64 fps;
	u32 rc;

	if (!fifty || !stepNs)
		return false;
	/* The engine's step, rounded to whole steps a second: its wake-up time adds a little to each. */
	fps = (1000000000ull + stepNs / 2) / stepNs;
	if (!fps)
		return false;
	stepNs = 1000000000ull / fps;
	for (;;) {
		rc = retraces;
		if (primed)
			paid += (u64)(rc - seen) * FIELD_NS_50;
		primed = true;
		seen = rc;
		if (paid >= stepNs)
			break;
		VIDEO_WaitVSync();
	}
	paid -= stepNs;
	/* One step owed at most: a frame too heavy for its field slows the game, as at 60 Hz, rather than bursting. */
	if (paid > stepNs)
		paid = stepNs;
	return true;
}

/* Between frames: the next one is drawn at the new height. */
static void switch_mode(void)
{
	WiiVideo_settle();
	low = lowWanted;
	rmode = &modes[low];
	VIDEO_SetBlack(true);
	VIDEO_Configure(rmode);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	setup_copy();
	first_present = true;
	logInfo("video: %s, %ux%u\n", low ? "240p" : "full height", rmode->fbWidth, rmode->efbHeight);
}

void WiiVideo_set240p(bool on)
{
	lowWanted = on;
}

static u64 gpuWait;

uint64_t WiiVideo_gpuWaitNs(void)
{
	return ticks_to_nanosecs(gpuWait);
}

/* 60 Hz: the frame's copy goes into an XFB neither shown nor waiting for its retrace, and the GPU finishes it while the
 * next step runs (on_draw_done queues it); the CPU only waits for the vsync. */
static void present_sixty(void)
{
	static u32 seenRepeats, seenDrops, lastPresentAt;
	u64 t1;
	u32 level, misses;
	int r;

	WiiVideo_settle();
	gpuWait = settleTicks;
	settleTicks = 0;
	t1 = gettime();
	_CPU_ISR_Disable(level);
	for (r = 0; r < 2 && (r == shownFb || r == queuedFb || r == heldFb); r++)
		;
	_CPU_ISR_Restore(level);
	GX_CopyDisp(xfb[r], GX_TRUE);
	_CPU_ISR_Disable(level);
	copyingCpuLate = retraces - lastPresentAt > 1;
	lastPresentAt = copyingAt = retraces;
	copyingFb = r;
	_CPU_ISR_Restore(level);
	GX_SetDrawDone();
	VIDEO_WaitVSync();
	misses = (repeats - seenRepeats) + (drops - seenDrops);
	seenRepeats = repeats;
	seenDrops = drops;
	frame_stats(t1 - gpuWait, t1, gettime(), misses);
	if (lowWanted != low)
		switch_mode();
}

void WiiVideo_present(void)
{
	u64 t0, t1;

	if (!fifty) {
		present_sixty();
		return;
	}
	t0 = gettime();
	GX_DrawDone();
	t1 = gettime();
	gpuWait = t1 - t0;
	present_fifty();
	frame_stats(t0, t1, gettime(), 0);
	if (lowWanted != low)
		switch_mode();
}

/* VI scales the 640-wide XFB onto this many of the line's 720 pixels. Nintendo's 640 leaves
 * borders on the Wii U's HDMI path; 704 or 720 fill it (overscan.txt's third number). */
void WiiVideo_setActiveWidth(int w)
{
	int k;

	if (!rmode || w < 640 || w > 720 || w == rmode->viWidth)
		return;
	WiiVideo_settle();
	for (k = 0; k < 2; k++) {
		modes[k].viWidth = (u16)(w & ~1);
		modes[k].viXOrigin = (u16)((720 - modes[k].viWidth) / 2);
	}
	VIDEO_Configure(rmode);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	logInfo("VI active width %u (origin %u)\n", rmode->viWidth, rmode->viXOrigin);
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

/* Own XFB, so it works even if GX never came up; the game's next frame takes the display back. */
bool WiiVideo_console(void)
{
	static void *pxfb;
	GXRModeObj *m;
	u32 level;

	if (inited)
		WiiVideo_settle();
	VIDEO_Init();
	/* VIDEO_Init drops the retrace callback the 60 Hz present counts on after the patcher; no queued or held frame may
	 * take the screen from the console. */
	_CPU_ISR_Disable(level);
	queuedFb = heldFb = -1;
	_CPU_ISR_Restore(level);
	VIDEO_SetPostRetraceCallback(on_retrace);
	m = pick_mode();
	if (!pxfb)
		pxfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(m));
	if (!pxfb)
		return false;
	console_init(pxfb, 20, 20, m->fbWidth, m->xfbHeight, m->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(m);
	VIDEO_SetNextFramebuffer(pxfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (m->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	printf("\x1b[2J");
	return true;
}

/* Word-wrapped inside the margins: the console itself breaks lines mid-word at the screen's edge. */
static void print_wrapped(const char *text, int width)
{
	while (*text) {
		int n = (int)strlen(text), cut = n;

		if (n > width) {
			for (cut = width; cut > 0 && text[cut] != ' '; cut--)
				;
			if (cut == 0)
				cut = width;
		}
		printf("  %.*s\n", cut, text);
		text += cut;
		while (*text == ' ')
			text++;
	}
}

void WiiVideo_panic(const char *title, const char *body, bool showLog)
{
	if (!WiiVideo_console())
		return;
	printf("\n\n  AM2R GX -- %s\n\n", title ? title : "fatal error");
	if (body && *body) {
		print_wrapped(body, 66);
		printf("\n");
	}
	if (showLog)
		printf("  Details: %s/profile.txt\n", WII_APP_DIR);
	printf("  Hold HOME to return to the Homebrew Channel.\n");
}
