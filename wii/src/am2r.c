#include "am2r.h"
#include "gx_renderer.h"

#include "gml_array.h"
#include "instance.h"
#include "int_rvalue_hashmap.h"
#include "log.h"
#include "runner.h"
#include "rvalue.h"
#include "stb_ds.h"
#include "text_utils.h"
#include "utils.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int32_t Wii_objectIndex(Runner *runner, const char *name)
{
	DataWin *dw = runner ? runner->dataWin : NULL;
	uint32_t i;

	for (i = 0; dw && i < dw->objt.count; i++)
		if (strcmp(dw->objt.objects[i].name, name) == 0)
			return (int32_t)i;
	return -1;
}

int32_t Wii_spriteIndex(Runner *runner, const char *name)
{
	DataWin *dw = runner ? runner->dataWin : NULL;
	uint32_t i;

	for (i = 0; dw && i < dw->sprt.count; i++)
		if (dw->sprt.sprites[i].name && strcmp(dw->sprt.sprites[i].name, name) == 0)
			return (int32_t)i;
	return -1;
}

int32_t Wii_roomIndex(Runner *runner, const char *name)
{
	DataWin *dw = runner ? runner->dataWin : NULL;
	uint32_t i;

	for (i = 0; dw && i < dw->room.count; i++)
		if (dw->room.rooms[i].name && strcmp(dw->room.rooms[i].name, name) == 0)
			return (int32_t)i;
	return -1;
}

int32_t Wii_varId(Runner *runner, const char *name, int32_t scope)
{
	DataWin *dw = runner ? runner->dataWin : NULL;
	uint32_t i;

	for (i = 0; dw && i < dw->vari.variableCount; i++) {
		Variable *v = &dw->vari.variables[i];

		if (v->instanceType == scope && v->varID >= 0 && v->name && strcmp(v->name, name) == 0)
			return v->varID;
	}
	return -1;
}

/* global.controltype picks the prompt glyph set: 0 keyboard, 1 numbered pad, 2 named A/B pad.
 * check_keys resets it every step from the last input seen. Pin it to 2 after the step so prompts
 * stay the named-button set on a controller-only console. Gameplay unaffected. */
void Wii_forceGamepadProfile(Runner *runner)
{
	static int32_t varId = -2;
	VMContext *vm;

	if (!runner || !runner->vmContext)
		return;
	vm = runner->vmContext;
	if (varId == -2) {
		ptrdiff_t slot = shgeti(vm->varNameMap, "controltype");
		varId = slot >= 0 ? vm->varNameMap[slot].value : -1;
	}
	if (varId >= 0 && vm->globalScopeInstance)
		Instance_setSelfVar(vm->globalScopeInstance, varId, RValue_makeInt32(2));
}

void Wii_setViewClears(Runner *runner)
{
	static int32_t itemRoom = -2;

	if (!runner)
		return;
	if (itemRoom == -2)
		itemRoom = Wii_roomIndex(runner, "itemroom");
	runner->clearViewSurfaces = runner->currentRoomIndex != itemRoom;
}

static int32_t sprite_named(Runner *runner, const char *name)
{
	ptrdiff_t slot = shgeti(runner->assetsByName, name);
	int32_t i = slot >= 0 ? runner->assetsByName[slot].value : -1;

	if (i < 0 || (uint32_t)i >= runner->dataWin->sprt.count || !runner->dataWin->sprt.sprites[i].name ||
	    strcmp(runner->dataWin->sprt.sprites[i].name, name) != 0)
		return -1;
	return i;
}

static void add_sprite(int32_t **tpags, DataWin *dw, int32_t sprite, uint32_t frames)
{
	const Sprite *sp;
	uint32_t f;

	if (sprite < 0 || (uint32_t)sprite >= dw->sprt.count)
		return;
	sp = &dw->sprt.sprites[sprite];
	for (f = 0; sp->tpagIndices && f < sp->textureCount && f < frames; f++)
		arrput(*tpags, sp->tpagIndices[f]);
}

static void add_background(int32_t **tpags, DataWin *dw, const char *prefix)
{
	uint32_t i;

	for (i = 0; i < dw->bgnd.count; i++)
		if (dw->bgnd.backgrounds[i].name && strncmp(dw->bgnd.backgrounds[i].name, prefix, strlen(prefix)) == 0)
			arrput(*tpags, dw->bgnd.backgrounds[i].tpagIndex);
}

/* Samus's animations for one suit: the sprites with Varia and Gravity twins (sX, sVX, sGX); Fusion mode uses
 * sX_fusion, then _varia or _gravity after it. */
static void add_suit(int32_t **tpags, Runner *runner, int32_t suit, bool fusion)
{
	DataWin *dw = runner->dataWin;
	uint32_t i;
	char name[96];

	for (i = 0; i < dw->sprt.count; i++) {
		const char *n = dw->sprt.sprites[i].name;
		int32_t base, gravity, pick, f;

		if (!n || strncmp(n, "sV", 2) != 0 || !n[2])
			continue;
		snprintf(name, sizeof name, "s%s", n + 2);
		base = sprite_named(runner, name);
		snprintf(name, sizeof name, "sG%s", n + 2);
		gravity = sprite_named(runner, name);
		if (base < 0 || gravity < 0)
			continue;
		pick = suit == 1 ? (int32_t)i : suit == 2 ? gravity : base;
		if (fusion) {
			snprintf(name, sizeof name, "s%s_fusion%s", n + 2, suit == 1 ? "_varia" : suit == 2 ? "_gravity" : "");
			f = sprite_named(runner, name);
			if (f >= 0)
				pick = f;
		}
		add_sprite(tpags, dw, pick, UINT32_MAX);
	}
}

/* What the next seconds will draw, for the loader to fetch while idle (a page pulled mid-play stalls a Wii ~140 ms):
 * the title's first frame, the file select, the intro's art, and in play Samus's suit, the HUD and the pause screen. */
void Wii_wishPages(Runner *runner)
{
	enum { NONE = -1, TITLE_START = 100, FILE_SELECT = 101, INTRO = 200 };
	static int32_t ingame = -2, suitVar, fusionVar, controllerRoom, titleRoom, introRoom, last = NONE;
	DataWin *dw;
	Instance *g;
	int32_t key, suit, *tpags = NULL;
	uint32_t i;

	if (!runner || !runner->vmContext || !(g = runner->vmContext->globalScopeInstance))
		return;
	dw = runner->dataWin;
	if (ingame == -2) {
		ingame = Wii_varId(runner, "ingame", INSTANCE_GLOBAL);
		suitVar = Wii_varId(runner, "currentsuit", INSTANCE_GLOBAL);
		fusionVar = Wii_varId(runner, "mod_fusion", INSTANCE_GLOBAL);
		controllerRoom = Wii_roomIndex(runner, "rm_controller");
		titleRoom = Wii_roomIndex(runner, "titleroom");
		introRoom = Wii_roomIndex(runner, "gameintroroom");
	}
	if (runner->currentRoomIndex == controllerRoom) {
		key = TITLE_START;
	} else if (runner->currentRoomIndex == titleRoom) {
		key = FILE_SELECT;
	} else if (runner->currentRoomIndex == introRoom ||
		   (ingame >= 0 && RValue_toReal(Instance_getSelfVar(g, ingame)) == 1)) {
		suit = suitVar >= 0 ? (int32_t)RValue_toReal(Instance_getSelfVar(g, suitVar)) : 0;
		key = (suit >= 0 && suit <= 2 ? suit : 0) * 2 +
		      (fusionVar >= 0 && RValue_toReal(Instance_getSelfVar(g, fusionVar)) == 1) +
		      (runner->currentRoomIndex == introRoom ? INTRO : 0);
	} else {
		return;
	}
	if (key == last)
		return;
	last = key;
	if (key == TITLE_START) {
		add_sprite(&tpags, dw, sprite_named(runner, "sTitleAnimated"), 1);
		add_background(&tpags, dw, "bgAM2RTitle");
		add_background(&tpags, dw, "bgURL");
		add_background(&tpags, dw, "bgVersion");
		add_sprite(&tpags, dw, sprite_named(runner, "sVersionFont"), UINT32_MAX);
	} else if (key == FILE_SELECT) {
		add_background(&tpags, dw, "bgSaveSlot");
	} else {
		if (key >= INTRO) {
			add_background(&tpags, dw, "bgIntro");
			add_background(&tpags, dw, "bgLoading");
		}
		for (i = 0; i < dw->sprt.count; i++)
			if (dw->sprt.sprites[i].name && (strncmp(dw->sprt.sprites[i].name, "sMap", 4) == 0 ||
							 strncmp(dw->sprt.sprites[i].name, "sGUIFont", 8) == 0))
				add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
		add_suit(&tpags, runner, key % INTRO / 2, key & 1);
		for (i = 0; i < dw->sprt.count; i++)
			if (dw->sprt.sprites[i].name && strncmp(dw->sprt.sprites[i].name, "sSubScr", 7) == 0)
				add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
		add_background(&tpags, dw, "bg_SubScr");
		add_background(&tpags, dw, "bgMapScreen");
	}
	GxRenderer_wish(dw, tpags, (uint32_t)arrlen(tpags));
	logInfo("wish: %s, %d texture items\n", key == TITLE_START ? "title start" : key == FILE_SELECT ? "file select" :
		key >= INTRO ? "intro" : key & 1 ? "play, fusion" : "play", (int)arrlen(tpags));
	arrfree(tpags);
}

static int32_t coolTextCode = -1;

/* draw_cool_text(x, y, text, outline, top, bottom, alpha) draws its text nine times one pixel apart, copying and laying
 * out the string each time; the credits roll does that with all its rows every frame. */
static RValue wii_draw_cool_text(VMContext *ctx, RValue *args, int32_t argCount)
{
	Renderer *r = ctx->runner->renderer;
	PreprocessedText text;
	char *str;

	if (argCount != 7 || !r)
		return VM_callCodeIndex(ctx, coolTextCode, args, argCount);
	r->drawAlpha = (float)RValue_toReal(args[6]);
	r->drawColor = RValue_toColour(args[3]);
	str = RValue_toString(args[2], ctx->runner->dataWin);
	text = TextUtils_preprocessGmlTextIfNeeded(ctx->runner, str);
	GxRenderer_drawOutlinedText(r, text.text, RValue_toReal(args[0]), RValue_toReal(args[1]), (int32_t)r->drawColor,
				    (int32_t)RValue_toColour(args[4]), (int32_t)RValue_toColour(args[5]), r->drawAlpha);
	PreprocessedText_free(text);
	free(str);
	r->drawAlpha = 1.0f;
	return RValue_makeUndefined();
}

static int32_t resetMapCode = -1, mapVar, dmapVar, logOptionVar, iVar, jVar;

/* reset_map builds a "Resetting i,j" line for each of its 6,400 cells, which writelog drops unless AM2R's log option is
 * on: ~120 ms of every game load. Off, the cells are set here as the script would; on, the script itself runs. */
static RValue wii_reset_map(VMContext *ctx, RValue *args, int32_t argCount)
{
	Instance *g = ctx->globalScopeInstance;
	RValue zero = RValue_makeReal(0), cell = RValue_makeString("0");
	int32_t i, j;

	if (logOptionVar < 0 || RValue_toReal(Instance_getSelfVar(g, logOptionVar)) == 1 || !ctx->currentInstance)
		return VM_callCodeIndex(ctx, resetMapCode, args, argCount);
	for (i = 0; i < 80; i++)
		for (j = 0; j < 80; j++) {
			VM_arraySetWithCoW(ctx, IntRValueHashMap_getOrInsertUndefined(&g->selfVars, mapVar),
					   i * GML_LEGACY_ARRAY_STRIDE + j, cell);
			VM_arraySetWithCoW(ctx, IntRValueHashMap_getOrInsertUndefined(&g->selfVars, dmapVar),
					   i * GML_LEGACY_ARRAY_STRIDE + j, zero);
		}
	/* its loop counters are the caller's own variables */
	Instance_setSelfVar(ctx->currentInstance, iVar, RValue_makeReal(80));
	Instance_setSelfVar(ctx->currentInstance, jVar, RValue_makeReal(80));
	return RValue_makeUndefined();
}

/* Ours, then AM2R's, in its markup (* heading, / centred, a=b paired, ; next row). The roll scrolls at a speed set by
 * its row count to last as long as its music, so these rows make it a little faster, not longer. */
static const char credits[] = "*AM2R GX;;;;;;;;;;;;"
	"*Wii Port;/cmyksoda;;"
	"*Beta Testers;bonkmaykr=Erzaui;NiuuS=Splatterpunk;;"
	"*Controller Screen Artist;/Asophia Mills;;"
	"*Made Possible By;/AM2R Community Developers;/The Butterscotch Team;/devkitPro;/DoctorM64;/The Dolphin Team;;"
	"*Special Thanks;/My sweet girlfriend,;/for always supporting me <3;;/syl, for being the sweetest syl ever;;;;;;;;;;;;"
	"*Another ";

void Wii_addCredits(DataWin *dw)
{
	repeat(dw->strg.count, i)
		if (strcmp(dw->strg.strings[i], "*Another ") == 0)
			dw->strg.strings[i] = credits;
}

void Wii_hookAm2rScripts(VMContext *ctx)
{
	ptrdiff_t s = shgeti(ctx->codeIndexByName, "reset_map"), t = shgeti(ctx->codeIndexByName, "draw_cool_text");
	bool resetMap;

	resetMapCode = s >= 0 ? ctx->codeIndexByName[s].value : -1;
	coolTextCode = t >= 0 ? ctx->codeIndexByName[t].value : -1;
	mapVar = Wii_varId(ctx->runner, "map", INSTANCE_GLOBAL);
	dmapVar = Wii_varId(ctx->runner, "dmap", INSTANCE_GLOBAL);
	logOptionVar = Wii_varId(ctx->runner, "opwritelog", INSTANCE_GLOBAL);
	iVar = Wii_varId(ctx->runner, "i", INSTANCE_SELF);
	jVar = Wii_varId(ctx->runner, "j", INSTANCE_SELF);
	resetMap = resetMapCode >= 0 && mapVar >= 0 && dmapVar >= 0 && iVar >= 0 && jVar >= 0;
	repeat(ctx->funcCallCacheCount, fi) {
		const char *name = ctx->dataWin->func.functions[fi].name;
		void *standIn = NULL;

		if (name && resetMap && strcmp(name, "reset_map") == 0)
			standIn = (void *)wii_reset_map;
		else if (name && coolTextCode >= 0 && strcmp(name, "draw_cool_text") == 0)
			standIn = (void *)wii_draw_cool_text;
		if (standIn) {
			ctx->funcCallCache[fi].builtin = standIn;
			ctx->funcCallCache[fi].scriptCodeIndex = -1;
		}
	}
}
