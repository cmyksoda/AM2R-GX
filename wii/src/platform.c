#include "assets.h"
#include "common.h"
#include "gettime.h"
#include "gx_renderer.h"
#include "input.h"
#include "log.h"
#include "platformdefs.h"
#include "runner.h"
#include "runner_mouse.h"
#include "rvalue.h"
#include "sweep.h"
#include "utils.h"
#include "vm.h"
#include "wii_video.h"

#include <string.h>
#include <time.h>

static Runner *g_runner;
static int32_t g_width = WII_FB_W;
static int32_t g_height = WII_FB_H;
static bool g_initialized;

static RValue stub_zero(MAYBE_UNUSED VMContext *ctx, MAYBE_UNUSED RValue *args, MAYBE_UNUSED int32_t argCount)
{
	return RValue_makeReal(0.0);
}

/* 1.5.5 places its HUD surface relative to this; ours always sits at the window origin. */
static RValue stub_application_get_position(VMContext *ctx, MAYBE_UNUSED RValue *args, MAYBE_UNUSED int32_t argCount)
{
	GMLArray *pos = GMLArray_create(ctx->dataWin, 4);

	GMLArray_set(pos, 0, RValue_makeReal(0.0));
	GMLArray_set(pos, 1, RValue_makeReal(0.0));
	GMLArray_set(pos, 2, RValue_makeReal((GMLReal)g_width));
	GMLArray_set(pos, 3, RValue_makeReal((GMLReal)g_height));
	return RValue_makeArray(pos);
}

static BuiltinFunc engineIniReadReal, engineIniWriteReal;

static RValue ini_screen(VMContext *ctx, BuiltinFunc fn, const char *key, double value)
{
	RValue args[3] = { RValue_makeString("Screen"), RValue_makeString(key), RValue_makeReal(value) };

	return fn(ctx, args, 3);
}

/* 1.5.5 asks its ini for [Screen] Widescreen (default on). The console's own 4:3 / 16:9 setting
 * picks it instead, once: after that the player's in-game toggle is what the ini holds and it wins,
 * until the console setting changes (or the card moves to another console) and we detect afresh. */
static RValue wii_ini_read_real(VMContext *ctx, RValue *args, int32_t argCount)
{
	double console = WiiVideo_isWide() ? 1.0 : 0.0;
	double filter;

	if (argCount < 3 || args[0].type != RVALUE_STRING || args[1].type != RVALUE_STRING ||
	    strcmp(args[0].string, "Screen") != 0 || strcmp(args[1].string, "Widescreen") != 0)
		return engineIniReadReal(ctx, args, argCount);
	/* Our own key rides along: 1 = even 3:2 box filter, 0 = plain 2,1,2,1 columns. */
	filter = RValue_toReal(ini_screen(ctx, engineIniReadReal, "WiiWideFilter", -1.0));
	if (filter < 0 && engineIniWriteReal) {
		ini_screen(ctx, engineIniWriteReal, "WiiWideFilter", 1.0);
		filter = 1.0;
	}
	GxRenderer_setWideFilter(filter != 0.0);
	if (RValue_toReal(ini_screen(ctx, engineIniReadReal, "WiiAspect", -1.0)) == console)
		return engineIniReadReal(ctx, args, argCount);
	if (engineIniWriteReal) {
		ini_screen(ctx, engineIniWriteReal, "WiiAspect", console);
		ini_screen(ctx, engineIniWriteReal, "Widescreen", console);
	}
	logInfo("Wii: console aspect %s -> widescreen %s\n", console ? "16:9" : "4:3", console ? "on" : "off");
	return RValue_makeReal(console);
}

static RValue wii_display_reset(MAYBE_UNUSED VMContext *ctx, MAYBE_UNUSED RValue *args, MAYBE_UNUSED int32_t argCount)
{
	GxRenderer_loseSurfaces();
	return RValue_makeReal(0.0);
}

static void register_stubs(VMContext *ctx)
{
	static const char *zeros[] = {
		"gamepad_set_vibration",
		"joystick_zpos",
		"joystick_rpos",
		"joystick_upos",
		"joystick_vpos",
		"show_message_async",
		"get_integer",
		"show_message",
		"file_text_open_append",
		"date_current_datetime",
		"date_datetime_string",
		"get_integer_async",
		"show_debug_overlay",
		"screen_save",
		"surface_save",
		"display_get_dpi_x",
		"display_get_dpi_y",
		"device_mouse_raw_x",
		"device_mouse_raw_y",
		"virtual_key_add",
		"virtual_key_show",
		"io_clear",
		"texture_set_interpolation_ext",
		/* 1.5.5's secret-ending hornoad is the only physics object; it sits still. */
		"physics_apply_local_impulse",
		"physics_apply_torque",
		/* TrueType replacements for the non-English fonts; the embedded fonts stay. */
		"font_replace",
	};
	uint32_t i;

	for (i = 0; i < sizeof(zeros) / sizeof(zeros[0]); i++) {
		if (VM_findBuiltin(ctx, zeros[i]) == NULL)
			VM_registerBuiltin(ctx, zeros[i], stub_zero);
	}
	if (VM_findBuiltin(ctx, "display_reset") == NULL)
		VM_registerBuiltin(ctx, "display_reset", wii_display_reset);
	if (VM_findBuiltin(ctx, "sprite_replace") == NULL)
		VM_registerBuiltin(ctx, "sprite_replace", WiiAssets_builtin("sprite_replace"));
	if (VM_findBuiltin(ctx, "background_replace") == NULL)
		VM_registerBuiltin(ctx, "background_replace", WiiAssets_builtin("background_replace"));
	if (VM_findBuiltin(ctx, "surface_getpixel") == NULL)
		VM_registerBuiltin(ctx, "surface_getpixel", WiiAssets_builtin("surface_getpixel"));
	if (VM_findBuiltin(ctx, "surface_getpixel_ext") == NULL)
		VM_registerBuiltin(ctx, "surface_getpixel_ext", WiiAssets_builtin("surface_getpixel_ext"));
	if (VM_findBuiltin(ctx, "application_get_position") == NULL)
		VM_registerBuiltin(ctx, "application_get_position", stub_application_get_position);
	/* A name can only be registered once; the call cache below is what routes calls to the wrapper. */
	engineIniReadReal = VM_findBuiltin(ctx, "ini_read_real");
	engineIniWriteReal = VM_findBuiltin(ctx, "ini_write_real");

	if (ctx->funcCallCache) {
		repeat(ctx->funcCallCacheCount, fi) {
			const char *name = ctx->dataWin->func.functions[fi].name;
			ptrdiff_t sidx;
			BuiltinFunc b;

			if (name == NULL)
				continue;
			/* A GML script shadowing a builtin (AM2R's string_split is a GM:S 1.4 compat
			 * script returning a substring, not the builtin's array) must win, or its
			 * result draws as "<array:0x...>". Butterscotch's cache prefers the builtin. */
			sidx = shgeti(ctx->codeIndexByName, (char *)name);
			if (sidx >= 0) {
				ctx->funcCallCache[fi].builtin = NULL;
				ctx->funcCallCache[fi].scriptCodeIndex = ctx->codeIndexByName[sidx].value;
				continue;
			}
			if (engineIniReadReal && ctx->funcCallCache[fi].builtin == (void *)engineIniReadReal)
				ctx->funcCallCache[fi].builtin = (void *)wii_ini_read_real;
			if (strcmp(name, "sprite_add") == 0 || strcmp(name, "background_add") == 0)
				ctx->funcCallCache[fi].builtin = (void *)WiiAssets_builtin(name);
			if (ctx->funcCallCache[fi].builtin != NULL)
				continue;
			b = VM_findBuiltin(ctx, name);
			if (b)
				ctx->funcCallCache[fi].builtin = (void *)b;
		}
	}
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless)
{
	(void)title;
	(void)headless;
	if (reqW > 0)
		g_width = reqW;
	if (reqH > 0)
		g_height = reqH;
	if (!WiiVideo_init()) {
		logError("WiiVideo_init failed\n");
		return false;
	}
	g_initialized = true;
	logInfo("Wii platform: %dx%d GX (fb %ux%u)\n", g_width, g_height,
		WiiVideo_fbWidth(), WiiVideo_efbHeight());
	return true;
}

void platformExit(void)
{
	g_initialized = false;
}

void platformInitFunctions(Runner *runner)
{
	g_runner = runner;
	runner->setCursor = NULL;
	runner->currentCursor = GML_CR_DEFAULT;
	if (runner->vmContext)
		register_stubs(runner->vmContext);
}

bool platformGetWindowSize(int32_t *outW, int32_t *outH)
{
	if (!outW || !outH || !g_initialized)
		return false;
	*outW = g_width;
	*outH = g_height;
	return true;
}

bool platformGetScaledWindowSize(int32_t *outW, int32_t *outH)
{
	return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height)
{
	if (width > 0)
		g_width = width;
	if (height > 0)
		g_height = height;
}

void platformSetWindowTitle(const char *title)
{
	(void)title;
}

void platformGetMousePos(double *xPos, double *yPos)
{
	if (xPos)
		*xPos = 0.0;
	if (yPos)
		*yPos = 0.0;
}

void platformSwapBuffers(void)
{
	GxRenderer_present();
	Wii_efbDump();
	WiiVideo_present();
	Wii_shotHold();
}

void *platformGetProcAddress(const char *name)
{
	(void)name;
	return NULL;
}

bool platformHandleEvents(void)
{
	WiiInput_poll(g_runner);
	return WiiInput_shouldQuit() || Wii_sweepFinished();
}

void platformSleepUntil(uint64_t time)
{
	int64_t remaining = (int64_t)time - (int64_t)nowNanos();

	if (remaining > 2000000) {
		struct timespec ts;

		remaining -= 1000000;
		ts.tv_sec = remaining / 1000000000;
		ts.tv_nsec = remaining % 1000000000;
		nanosleep(&ts, NULL);
	}
	while (nowNanos() < time)
		YIELD();
}
