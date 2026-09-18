#include "common.h"
#include "gettime.h"
#include "input.h"
#include "log.h"
#include "platformdefs.h"
#include "runner.h"
#include "runner_mouse.h"
#include "rvalue.h"
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

static RValue stub_undef(MAYBE_UNUSED VMContext *ctx, MAYBE_UNUSED RValue *args, MAYBE_UNUSED int32_t argCount)
{
	return RValue_makeUndefined();
}

static void register_stubs(VMContext *ctx)
{
	static const char *zeros[] = {
		"gamepad_set_vibration",
		"joystick_zpos",
		"joystick_rpos",
		"joystick_upos",
		"joystick_vpos",
		"file_copy",
		"show_message_async",
		"get_integer",
		"show_message",
		"file_text_open_append",
		"date_current_datetime",
		"date_datetime_string",
		"display_reset",
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
		"background_add",
		"io_clear",
	};
	uint32_t i;

	for (i = 0; i < sizeof(zeros) / sizeof(zeros[0]); i++) {
		if (VM_findBuiltin(ctx, zeros[i]) == NULL)
			VM_registerBuiltin(ctx, zeros[i], stub_zero);
	}
	(void)stub_undef;

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
	WiiVideo_present();
}

void *platformGetProcAddress(const char *name)
{
	(void)name;
	return NULL;
}

bool platformHandleEvents(void)
{
	WiiInput_poll(g_runner);
	return WiiInput_shouldQuit();
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
