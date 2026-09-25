#include "controls.h"

#include "am2r.h"
#include "gml_array.h"
#include "input.h"
#include "instance.h"
#include "log.h"
#include "runner.h"
#include "rvalue.h"
#include "spatial_grid.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm.h"

#include <stdio.h>
#include <string.h>

/* AM2R's Control Options rows (oOptionsControl): 6 is Keyboard Settings, 7 and 9 are Android-only and already hidden. */
#define ROW_KEYBOARD 6
#define ROW_EXIT 10
#define XJOY_TITLE 50 /* oOptionsXJoypad's op[] slot for its title */
#define GP_FACE1 32769
#define GP_START 32778
#define NINTENDO_NAMES 2

enum { SCR_DEFAULTS, SCR_NAME, SCR_SPRITE, SCR_TEXT, SCR_COUNT };
static const char *const scriptNames[SCR_COUNT] = { "set_default_xjoypad", "get_xjoybtnname", "get_xjoybtnsprite", "get_text" };
static int32_t scriptCode[SCR_COUNT] = { -1, -1, -1, -1 };

/* key/key2: [Buttons] strings, so the words stay in the player's language; key2 joins as "key/key2". */
typedef struct {
	const char *text, *key, *key2;
} Name;

#define T(text) { text, NULL, NULL }
#define W(key, key2) { NULL, key, key2 }
/* By GameMaker constant from gp_face1: face1-4, LB, RB, LT, RT, select, start; see the maps in input.c. */
static const Name gcNames[10] = {
	T("A"), T("X"), T("B"), T("Y"), W("LeftShoulder", NULL), W("RightShoulder", NULL), T("Z"), T(""), T(""), W("Start", NULL),
};
static const Name nunchukNames[10] = {
	T("A"), T("C"), T("B"), T("-"), W("Up", "Down"), T("Z"), W("Left", "Right"), W("Down", NULL), T(""), T("+"),
};
static const Name remoteNames[10] = {
	T("2"), T("A"), T("1"), T("-"), T(""), T("B"), T(""), T(""), T(""), T("+"),
};
#undef T
#undef W
/* The game's Nintendo glyphs by letter; NULL keeps the game's own glyph. */
static const char *const gcGlyphs[4] = { "WiiUsXJoyBtnA", "WiiUsXJoyBtnX", "WiiUsXJoyBtnB", "WiiUsXJoyBtnY" };
static const char *const nunchukGlyphs[4] = { "WiiUsXJoyBtnA", NULL, "WiiUsXJoyBtnB", NULL };

/* Looked up by scope: canedit, for one, is also a global somewhere in the data, listed first. */
static int32_t var_id(Runner *runner, const char *name, int32_t scope)
{
	static struct {
		const char *name;
		int32_t scope, id;
	} cache[12];
	static unsigned used;
	unsigned k;

	for (k = 0; k < used; k++)
		if (cache[k].name == name && cache[k].scope == scope)
			return cache[k].id;
	if (used == sizeof cache / sizeof cache[0])
		return Wii_varId(runner, name, scope);
	cache[used].name = name;
	cache[used].scope = scope;
	cache[used].id = Wii_varId(runner, name, scope);
	return cache[used++].id;
}

static Instance *first_instance(Runner *runner, const char *object)
{
	int32_t idx = Wii_objectIndex(runner, object);

	if (idx < 0 || !runner->instancesByExactObject || arrlen(runner->instancesByExactObject[idx]) == 0)
		return NULL;
	return runner->instancesByExactObject[idx][0];
}

static RValue *array_slot(Runner *runner, Instance *inst, const char *name, int32_t i)
{
	int32_t id = var_id(runner, name, INSTANCE_SELF);
	RValue v = id >= 0 ? Instance_getSelfVar(inst, id) : (RValue){ .type = RVALUE_UNDEFINED };

	return v.type == RVALUE_ARRAY ? GMLArray_slot(v.array, i) : NULL;
}

static Instance *op_row(Runner *runner, Instance *menu, int32_t i)
{
	RValue *v = array_slot(runner, menu, "op", i);
	int key;

	if (!v)
		return NULL;
	key = RValue_toInt32(*v);
	return hmget(runner->instancesById, key);
}

static double var_num(Runner *runner, Instance *inst, const char *name, int32_t scope, double fallback)
{
	int32_t id = inst ? var_id(runner, name, scope) : -1;
	RValue v = id >= 0 ? Instance_getSelfVar(inst, id) : (RValue){ .type = RVALUE_UNDEFINED };

	return v.type == RVALUE_UNDEFINED ? fallback : RValue_toReal(v);
}

static RValue call_script(VMContext *ctx, int which, RValue *args, int32_t argCount)
{
	if (scriptCode[which] < 0)
		return RValue_makeUndefined();
	return VM_callCodeIndex(ctx, scriptCode[which], args, argCount);
}

/* get_text(section, key): the arguments sit in the order the game pushes them. */
static char *button_word(VMContext *ctx, const char *key)
{
	RValue args[2] = { RValue_makeString("Buttons"), RValue_makeString(key) };
	RValue r = call_script(ctx, SCR_TEXT, args, 2);
	char *s = safeStrdup(r.type == RVALUE_STRING && r.string ? r.string : key);

	RValue_free(&r);
	return s;
}

static char *name_text(VMContext *ctx, const Name *n)
{
	char *a, *b, *both;
	size_t len;

	if (n->text)
		return safeStrdup(n->text);
	a = button_word(ctx, n->key);
	if (!n->key2)
		return a;
	b = button_word(ctx, n->key2);
	len = strlen(a) + strlen(b) + 2;
	both = safeMalloc(len);
	snprintf(both, len, "%s/%s", a, b);
	free(a);
	free(b);
	return both;
}

static bool nintendo_names(Runner *runner)
{
	return var_num(runner, first_instance(runner, "oControl"), "mod_buttonsconfig", INSTANCE_SELF, 0) == NINTENDO_NAMES;
}

/* The remap screen's Defaults row: AM2R's own defaults first (vibration, deadzone, ...), then our layout on top.
 * Boot also calls this script, before the options are read; there the stock set must stay for wii_pad_defaults. */
static RValue wii_set_default_xjoypad(VMContext *ctx, RValue *args, int32_t argCount)
{
	RValue r = call_script(ctx, SCR_DEFAULTS, args, argCount);

	if (first_instance(ctx->runner, "oOptionsXJoypad")) {
		WiiInput_applyLayout(ctx);
		logInfo("controls: Defaults restores the Wii layout\n");
	}
	return r;
}

static RValue wii_get_xjoybtnname(VMContext *ctx, RValue *args, int32_t argCount)
{
	WiiPad pad = WiiInput_pad();
	const Name *names = pad == WII_PAD_GAMECUBE ? gcNames : pad == WII_PAD_NUNCHUK ? nunchukNames :
			    pad == WII_PAD_REMOTE ? remoteNames : NULL;
	int32_t i = argCount > 0 ? RValue_toInt32(args[0]) - GP_FACE1 : -1;
	Name n;

	if (i < 0 || i >= 10 || !nintendo_names(ctx->runner))
		return call_script(ctx, SCR_NAME, args, argCount);
	if (!names) /* the Classic Controller: AM2R's Nintendo names, but its Start button is printed "+" */
		return i == GP_START - GP_FACE1 ? RValue_makeOwnedString(safeStrdup("+")) : call_script(ctx, SCR_NAME, args, argCount);
	n = names[i];
	/* two-button aim: the Nunchuk's d-pad Up alone is the aim-up button */
	if (pad == WII_PAD_NUNCHUK && n.key2 && var_num(ctx->runner, ctx->globalScopeInstance, "opaimstyle", INSTANCE_GLOBAL, 0) == 1)
		n.key2 = NULL;
	return RValue_makeOwnedString(name_text(ctx, &n));
}

static RValue wii_get_xjoybtnsprite(VMContext *ctx, RValue *args, int32_t argCount)
{
	WiiPad pad = WiiInput_pad();
	const char *const *glyphs = pad == WII_PAD_GAMECUBE ? gcGlyphs : pad == WII_PAD_NUNCHUK ? nunchukGlyphs : NULL;
	int32_t i = argCount > 0 ? RValue_toInt32(args[0]) - GP_FACE1 : -1;
	int32_t sprite;

	if (glyphs && i >= 0 && i < 4 && glyphs[i] && nintendo_names(ctx->runner) &&
	    (sprite = Wii_spriteIndex(ctx->runner, glyphs[i])) >= 0)
		return RValue_makeInt32(sprite);
	return call_script(ctx, SCR_SPRITE, args, argCount);
}

void WiiControls_hookScripts(VMContext *ctx)
{
	static const BuiltinFunc standIns[SCR_COUNT] = { wii_set_default_xjoypad, wii_get_xjoybtnname, wii_get_xjoybtnsprite, NULL };
	int k;

	for (k = 0; k < SCR_COUNT; k++) {
		ptrdiff_t s = shgeti(ctx->codeIndexByName, (char *)scriptNames[k]);

		scriptCode[k] = s >= 0 ? ctx->codeIndexByName[s].value : -1;
	}
	repeat(ctx->funcCallCacheCount, fi) {
		const char *name = ctx->dataWin->func.functions[fi].name;

		for (k = 0; k < SCR_COUNT && name; k++)
			if (standIns[k] && scriptCode[k] >= 0 && strcmp(name, scriptNames[k]) == 0) {
				ctx->funcCallCache[fi].builtin = (void *)standIns[k];
				ctx->funcCallCache[fi].scriptCodeIndex = -1;
			}
	}
}

/* Hidden the way the game hides its Android rows (invisible, never selected); the rows below move up a line. */
static void hide_keyboard_row(Runner *runner, Instance *menu)
{
	RValue *edit = array_slot(runner, menu, "canedit", ROW_KEYBOARD);
	float sep = (float)var_num(runner, menu, "sep", INSTANCE_SELF, 16);
	Instance *row;
	int32_t i;

	if (!edit)
		return;
	RValue_free(edit);
	*edit = RValue_makeInt32(0);
	if ((row = op_row(runner, menu, ROW_KEYBOARD)))
		row->visible = false;
	for (i = ROW_KEYBOARD + 1; i <= ROW_EXIT; i++)
		if ((row = op_row(runner, menu, i))) {
			row->y -= sep;
			if (runner->spatialGrid)
				SpatialGrid_markInstanceAsDirty(runner->spatialGrid, row);
		}
	logInfo("controls: Keyboard Settings hidden\n");
}

/* "Joypad Configuration": the title the game already has, in every language, for its other pad screen. */
static void retitle_joypad(Runner *runner, Instance *menu)
{
	VMContext *vm = runner->vmContext;
	Instance *title = op_row(runner, menu, XJOY_TITLE);
	int32_t text = var_id(runner, "text", INSTANCE_SELF);
	RValue args[2] = { RValue_makeString("OptionsControlJoypad"), RValue_makeString("Title_Joypad") };
	RValue r;

	if (!title || text < 0)
		return;
	r = call_script(vm, SCR_TEXT, args, 2);
	if (r.type == RVALUE_STRING) {
		Instance_setSelfVar(title, text, r);
		logInfo("controls: joypad screen titled \"%s\"\n", r.string);
	}
	RValue_free(&r);
}

void WiiControls_frame(Runner *runner)
{
	static uint32_t controlMenu, joypadMenu;
	Instance *menu;

	if (!runner || !runner->vmContext)
		return;
	if ((menu = first_instance(runner, "oOptionsControl")) && menu->instanceId != controlMenu) {
		controlMenu = menu->instanceId;
		hide_keyboard_row(runner, menu);
	}
	if ((menu = first_instance(runner, "oOptionsXJoypad")) && menu->instanceId != joypadMenu) {
		joypadMenu = menu->instanceId;
		retitle_joypad(runner, menu);
	}
}
