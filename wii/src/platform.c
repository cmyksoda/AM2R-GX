#include "am2r.h"
#include "assets.h"
#include "bin_files.h"
#include "common.h"
#include "controls.h"
#include "gettime.h"
#include "gml_array.h"
#include "gx_renderer.h"
#include "input.h"
#include "instance.h"
#include "log.h"
#include "platformdefs.h"
#include "runner.h"
#include "runner_mouse.h"
#include "rvalue.h"
#include "safearea.h"
#include "hooks.h"
#include "utils.h"
#include "vm.h"
#include "wii_video.h"

#include <stdlib.h>
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

static bool screen_key(RValue *args, int32_t argCount, const char *key)
{
	return argCount >= 3 && args[0].type == RVALUE_STRING && args[1].type == RVALUE_STRING &&
	       strcmp(args[0].string, "Screen") == 0 && strcmp(args[1].string, key) == 0;
}

/* 1.5.5 asks its ini for [Screen] Widescreen (default on). The console's own 4:3 / 16:9 setting
 * picks it instead, once: after that the player's in-game toggle is what the ini holds and it wins,
 * until the console setting changes (or the card moves to another console) and we detect afresh. */
static RValue wii_ini_read_real(VMContext *ctx, RValue *args, int32_t argCount)
{
	double console = WiiVideo_isWide() ? 1.0 : 0.0;
	double filter, value;

	/* The screen is the window: Fullscreen off + Scale 2 is the pair whose window_set_size gives the wide compose
	 * its 426-wide frame. */
	if (screen_key(args, argCount, "Fullscreen"))
		return RValue_makeReal(0.0);
	if (screen_key(args, argCount, "Scale"))
		return RValue_makeReal(2.0);
	/* The VSync row is the 240p switch here, under its own key: a PC config's VSync=1 must not turn it on. */
	if (screen_key(args, argCount, "VSync"))
		return ini_screen(ctx, engineIniReadReal, "Wii240p", 0.0);
	/* Off, the game reads no controller at all: a config.ini from a PC could leave nothing but HOME. */
	if (argCount >= 3 && args[0].type == RVALUE_STRING && args[1].type == RVALUE_STRING &&
	    strcmp(args[0].string, "Control") == 0 && strcmp(args[1].string, "EnableXBJoypad") == 0)
		return RValue_makeReal(1.0);
	if (!screen_key(args, argCount, "Widescreen"))
		return engineIniReadReal(ctx, args, argCount);
	/* Our own key rides along: 1 = even 3:2 box filter, 0 = plain 2,1,2,1 columns. */
	filter = RValue_toReal(ini_screen(ctx, engineIniReadReal, "WiiWideFilter", -1.0));
	if (filter < 0 && engineIniWriteReal) {
		ini_screen(ctx, engineIniWriteReal, "WiiWideFilter", 1.0);
		filter = 1.0;
	}
	GxRenderer_setWideFilter(filter != 0.0);
	if (RValue_toReal(ini_screen(ctx, engineIniReadReal, "WiiAspect", -1.0)) == console) {
		value = RValue_toReal(engineIniReadReal(ctx, args, argCount));
		GxRenderer_setPillarbox(value == 0.0 && console != 0.0);
		return RValue_makeReal(value);
	}
	if (engineIniWriteReal) {
		ini_screen(ctx, engineIniWriteReal, "WiiAspect", console);
		ini_screen(ctx, engineIniWriteReal, "Widescreen", console);
	}
	GxRenderer_setPillarbox(false);
	logInfo("Wii: console aspect %s -> widescreen %s\n", console ? "16:9" : "4:3", console ? "on" : "off");
	return RValue_makeReal(console);
}

/* The in-game toggle writes Widescreen; a 4:3 game on a 16:9 console gets pillarboxed from then on. */
static RValue wii_ini_write_real(VMContext *ctx, RValue *args, int32_t argCount)
{
	if (screen_key(args, argCount, "Widescreen"))
		GxRenderer_setPillarbox(RValue_toReal(args[2]) == 0.0 && WiiVideo_isWide());
	if (screen_key(args, argCount, "VSync"))
		return ini_screen(ctx, engineIniWriteReal, "Wii240p", RValue_toReal(args[2]));
	return engineIniWriteReal(ctx, args, argCount);
}

static RValue wii_gamepad_set_vibration(MAYBE_UNUSED VMContext *ctx, RValue *args, int32_t argCount)
{
	if (argCount >= 3)
		WiiInput_rumble((float)RValue_toReal(args[1]), (float)RValue_toReal(args[2]));
	return RValue_makeReal(0.0);
}

/* AM2R's dialogs say "Save Data Corrupted", just before it deletes that save: a Wii has no dialog,
 * so the log keeps it. */
static RValue stub_show_message(VMContext *ctx, RValue *args, int32_t argCount)
{
	char *text = argCount > 0 ? RValue_toString(args[0], ctx->runner->dataWin) : NULL;

	logWarn("Wii: game message: %s\n", text ? text : "");
	free(text);
	return RValue_makeReal(0.0);
}

static int32_t getTextCode = -1;

/* The 8-Bit filter's tip ends with its PC key in every language: "…, F9 key on PC", "… (клавиша F9 на ПК)". */
static RValue without_pc_key(RValue tip)
{
	const char *t = tip.type == RVALUE_STRING ? tip.string : NULL;
	const char *f9 = t ? strstr(t, "F9") : NULL;
	const char *cut = NULL, *p;
	char *trimmed;

	for (p = t; f9 && p < f9; p++)
		if (strncmp(p, ", ", 2) == 0 || strncmp(p, " (", 2) == 0 || strncmp(p, "。", 3) == 0)
			cut = p;
	if (!cut)
		return tip;
	trimmed = safeMalloc((size_t)(cut - t) + 1);
	memcpy(trimmed, t, (size_t)(cut - t));
	trimmed[cut - t] = '\0';
	RValue_free(&tip);
	return RValue_makeOwnedString(trimmed);
}

/* get_text opens the language ini, reads one key and closes it, and closing re-serializes the whole file: 256 times as
 * the pause opens. The files never change while the game runs, so each answer is kept by language, section and key. */
static RValue cached_text(VMContext *ctx, RValue *args, int32_t argCount)
{
	static struct { char *key; char *value; } *cache;
	static int32_t langVar = -2, currentVar;
	RValue lang, text;
	char *file, *key;
	size_t n;
	ptrdiff_t at;

	if (langVar == -2) {
		sh_new_strdup(cache);
		langVar = Wii_varId(ctx->runner, "language", INSTANCE_GLOBAL);
		currentVar = Wii_varId(ctx->runner, "currentlanguage", INSTANCE_GLOBAL);
	}
	if (argCount != 2 || args[0].type != RVALUE_STRING || args[1].type != RVALUE_STRING || !args[0].string ||
	    !args[1].string || langVar < 0 || currentVar < 0 || !ctx->globalScopeInstance)
		return VM_callCodeIndex(ctx, getTextCode, args, argCount);
	lang = Instance_getSelfVar(ctx->globalScopeInstance, langVar);
	lang = GMLArray_getOnArrayRef(&lang, RValue_toInt32(Instance_getSelfVar(ctx->globalScopeInstance, currentVar)));
	file = RValue_toString(lang, ctx->runner->dataWin);
	n = strlen(file) + strlen(args[0].string) + strlen(args[1].string) + 3;
	key = safeMalloc(n);
	snprintf(key, n, "%s\x01%s\x01%s", file, args[0].string, args[1].string);
	free(file);
	at = shgeti(cache, key);
	if (at >= 0) {
		free(key);
		return RValue_makeOwnedString(safeStrdup(cache[at].value));
	}
	text = VM_callCodeIndex(ctx, getTextCode, args, argCount);
	if (text.type == RVALUE_STRING && text.string)
		shput(cache, key, safeStrdup(text.string));
	free(key);
	return text;
}

static RValue wii_get_text(VMContext *ctx, RValue *args, int32_t argCount)
{
	if (argCount >= 2 && args[0].type == RVALUE_STRING && args[1].type == RVALUE_STRING && args[0].string &&
	    args[1].string && strcmp(args[0].string, "ExtrasMenu") == 0 && strcmp(args[1].string, "GBFilter_Tip") == 0)
		return without_pc_key(cached_text(ctx, args, argCount));
	/* The Display menu's VSync row is the 240p switch here (wii_ini_read_real keeps it under its own key). */
	if (argCount >= 2 && args[0].type == RVALUE_STRING && args[1].type == RVALUE_STRING && args[0].string &&
	    args[1].string && strcmp(args[0].string, "OptionsDisplay") == 0) {
		/* Fullscreen and Window Size are the two adjust pages here (controls.c, input.c). */
		if (strcmp(args[1].string, "Fullscreen") == 0)
			return RValue_makeOwnedString(safeStrdup("Resize/Move Screen"));
		if (strcmp(args[1].string, "Fullscreen_Tip") == 0)
			return RValue_makeOwnedString(safeStrdup("Fit the picture to your TV"));
		if (strcmp(args[1].string, "WindowSize") == 0 || strcmp(args[1].string, "DisplayScale") == 0)
			return RValue_makeOwnedString(safeStrdup("HUD Margins"));
		if (strcmp(args[1].string, "WindowSize_Tip") == 0 || strcmp(args[1].string, "DisplayScale_Tip") == 0)
			return RValue_makeOwnedString(safeStrdup("Keep the HUD inside your TV's edges"));
		if (strcmp(args[1].string, "VSync") == 0)
			return RValue_makeOwnedString(safeStrdup("240p"));
		if (strcmp(args[1].string, "VSync_Tip") == 0)
			return RValue_makeOwnedString(
				safeStrdup("Each of the game's 240 lines drawn once, for CRT TVs"));
	}
	return cached_text(ctx, args, argCount);
}

static void hook_get_text(VMContext *ctx)
{
	ptrdiff_t s = shgeti(ctx->codeIndexByName, "get_text");

	if (s < 0)
		return;
	getTextCode = ctx->codeIndexByName[s].value;
	repeat(ctx->funcCallCacheCount, fi) {
		const char *name = ctx->dataWin->func.functions[fi].name;

		if (name && strcmp(name, "get_text") == 0) {
			ctx->funcCallCache[fi].builtin = (void *)wii_get_text;
			ctx->funcCallCache[fi].scriptCodeIndex = -1;
		}
	}
}

/* display_reset(aa, vsync): the game calls it at boot and when its VSync row (our 240p switch) changes. */
static RValue wii_display_reset(MAYBE_UNUSED VMContext *ctx, RValue *args, int32_t argCount)
{
	if (argCount >= 2)
		WiiVideo_set240p(RValue_toReal(args[1]) != 0.0);
	GxRenderer_loseSurfaces();
	return RValue_makeReal(0.0);
}

static void register_stubs(VMContext *ctx)
{
	static const char *zeros[] = {
		"joystick_zpos",
		"joystick_rpos",
		"joystick_upos",
		"joystick_vpos",
		"get_integer",
		"file_text_open_append",
		"date_current_datetime",
		"date_datetime_string",
		"show_debug_overlay",
		"screen_save",
		"surface_save",
		"display_get_dpi_x",
		"display_get_dpi_y",
		"device_mouse_raw_x",
		"device_mouse_raw_y",
		"io_clear",
		"window_set_position",
		"window_get_x",
		"window_get_y",
		"texture_set_interpolation_ext",
		/* 1.5.5's secret-ending hornoad is the only physics object; it sits still. */
		"physics_apply_local_impulse",
		"physics_apply_torque",
	};
	uint32_t i;

	for (i = 0; i < sizeof(zeros) / sizeof(zeros[0]); i++) {
		if (VM_findBuiltin(ctx, zeros[i]) == NULL)
			VM_registerBuiltin(ctx, zeros[i], stub_zero);
	}
	if (VM_findBuiltin(ctx, "show_message") == NULL)
		VM_registerBuiltin(ctx, "show_message", stub_show_message);
	if (VM_findBuiltin(ctx, "show_message_async") == NULL)
		VM_registerBuiltin(ctx, "show_message_async", stub_show_message);
	if (VM_findBuiltin(ctx, "gamepad_set_vibration") == NULL)
		VM_registerBuiltin(ctx, "gamepad_set_vibration", wii_gamepad_set_vibration);
	if (VM_findBuiltin(ctx, "display_reset") == NULL)
		VM_registerBuiltin(ctx, "display_reset", wii_display_reset);
	if (VM_findBuiltin(ctx, "sprite_replace") == NULL)
		VM_registerBuiltin(ctx, "sprite_replace", WiiAssets_builtin("sprite_replace"));
	if (VM_findBuiltin(ctx, "background_replace") == NULL)
		VM_registerBuiltin(ctx, "background_replace", WiiAssets_builtin("background_replace"));
	if (VM_findBuiltin(ctx, "surface_getpixel_ext") == NULL)
		VM_registerBuiltin(ctx, "surface_getpixel_ext", WiiAssets_builtin("surface_getpixel_ext"));
	if (VM_findBuiltin(ctx, "font_replace") == NULL)
		VM_registerBuiltin(ctx, "font_replace", WiiAssets_builtin("font_replace"));
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
			/* A GML script shadowing a builtin must win (AM2R's string_split returns a substring,
			 * not an array); Butterscotch's cache prefers the builtin. */
			sidx = shgeti(ctx->codeIndexByName, (char *)name);
			if (sidx >= 0) {
				ctx->funcCallCache[fi].builtin = NULL;
				ctx->funcCallCache[fi].scriptCodeIndex = ctx->codeIndexByName[sidx].value;
				continue;
			}
			if (engineIniReadReal && ctx->funcCallCache[fi].builtin == (void *)engineIniReadReal)
				ctx->funcCallCache[fi].builtin = (void *)wii_ini_read_real;
			if (engineIniWriteReal && ctx->funcCallCache[fi].builtin == (void *)engineIniWriteReal)
				ctx->funcCallCache[fi].builtin = (void *)wii_ini_write_real;
			if (strcmp(name, "window_get_fullscreen") == 0 || strcmp(name, "window_set_fullscreen") == 0)
				ctx->funcCallCache[fi].builtin = (void *)stub_zero;
			if (strcmp(name, "sprite_add") == 0 || strcmp(name, "background_add") == 0)
				ctx->funcCallCache[fi].builtin = (void *)WiiAssets_builtin(name);
			if (ctx->funcCallCache[fi].builtin != NULL)
				continue;
			b = VM_findBuiltin(ctx, name);
			if (b)
				ctx->funcCallCache[fi].builtin = (void *)b;
		}
		WiiControls_hookScripts(ctx);
		WiiSafe_hookScripts(ctx);
		Wii_hookAm2rScripts(ctx);
		Wii_addCredits(ctx->dataWin);
		hook_get_text(ctx);
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
	WiiBinFiles_install(runner->fileSystem);
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
	Wii_hooksBeforeCopy();
	WiiVideo_present();
	Wii_hooksAfterPresent();
}

void *platformGetProcAddress(const char *name)
{
	(void)name;
	return NULL;
}

bool platformHandleEvents(void)
{
	WiiInput_poll(g_runner);
	return Wii_hooksEvents(g_runner) || WiiInput_shouldQuit();
}

void platformSleepUntil(uint64_t time)
{
	static uint64_t woke; /* the engine asks for woke + one step */
	int64_t remaining = (int64_t)time - (int64_t)nowNanos();

	if (!woke || time <= woke || !WiiVideo_paceStep(time - woke)) {
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
	woke = nowNanos();
}
