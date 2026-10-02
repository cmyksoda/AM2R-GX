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
#include "vm_builtins.h"

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

/* check_keys resets global.controltype (0 keyboard, 1 numbered pad, 2 named pad) every step; pinned to 2 after it,
 * as the Wii has only controllers. */
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

/* The metroid kinds whose objects (the metroid, its parts, the triggers that wake it) bring sprites on pages of their
 * own: the Alpha's turn alone sits on an Area 2 page that loaded mid-fight. */
static const char *const metroidKinds[] = { "MAlpha", "MGamma", "MZeta", "MOmega", "Queen" };
#define METROID_KINDS 5

/* A bit per kind with an object in the room; the objects are looked up by name once. */
static int32_t metroids_here(Runner *runner)
{
	static int32_t *objs[METROID_KINDS];
	static bool listed;
	DataWin *dw = runner->dataWin;
	int32_t mask = 0, k;
	uint32_t i;
	ptrdiff_t n;

	if (!listed) {
		listed = true;
		for (i = 0; i < dw->objt.count; i++)
			for (k = 0; k < METROID_KINDS; k++)
				if (dw->objt.objects[i].name && dw->objt.objects[i].name[0] == 'o' &&
				    strncmp(dw->objt.objects[i].name + 1, metroidKinds[k], strlen(metroidKinds[k])) == 0)
					arrput(objs[k], (int32_t)i);
	}
	if (!runner->instancesByExactObject)
		return 0;
	for (k = 0; k < METROID_KINDS; k++)
		for (n = 0; n < arrlen(objs[k]) && !(mask & (1 << k)); n++)
			if (arrlen(runner->instancesByExactObject[objs[k][n]]) > 0)
				mask |= 1 << k;
	return mask;
}

/* What the next seconds will draw, for the loader to fetch while idle (a page pulled mid-play stalls a Wii ~140 ms):
 * the title's first frame, the file select, the intro's art, the credits' ship and score screen, and in play Samus's
 * suit, the HUD, the pause screen and the metroid the room holds. */
void Wii_wishPages(Runner *runner)
{
	enum { NONE = -1, TITLE_START = 100, FILE_SELECT = 101, CREDITS = 102, INTRO = 200, METROID = 1000 };
	static int32_t ingame = -2, suitVar, fusionVar, controllerRoom, titleRoom, introRoom, subscreenRoom, creditsRoom;
	static int32_t last = NONE;
	DataWin *dw;
	Instance *g;
	int32_t key, suit, k, *tpags = NULL;
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
		subscreenRoom = Wii_roomIndex(runner, "rm_subscreen");
		creditsRoom = Wii_roomIndex(runner, "rm_credits");
	}
	if (runner->currentRoomIndex == controllerRoom) {
		key = TITLE_START;
	} else if (runner->currentRoomIndex == titleRoom) {
		key = FILE_SELECT;
	} else if (runner->currentRoomIndex == creditsRoom) {
		key = CREDITS;
	} else if (runner->currentRoomIndex == introRoom ||
		   (ingame >= 0 && RValue_toReal(Instance_getSelfVar(g, ingame)) == 1)) {
		suit = suitVar >= 0 ? (int32_t)RValue_toReal(Instance_getSelfVar(g, suitVar)) : 0;
		key = (suit >= 0 && suit <= 2 ? suit : 0) * 2 +
		      (fusionVar >= 0 && RValue_toReal(Instance_getSelfVar(g, fusionVar)) == 1) +
		      (runner->currentRoomIndex == introRoom ? INTRO : 0) +
		      /* the pause screen's room has none of the room's objects, and play resumes in that room */
		      (runner->currentRoomIndex == subscreenRoom && last >= 0 ? last / METROID : metroids_here(runner)) * METROID;
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
		for (i = 0; i < dw->sprt.count; i++)
			if (dw->sprt.sprites[i].name && strncmp(dw->sprt.sprites[i].name, "sSaveSlot", 9) == 0)
				add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
		add_background(&tpags, dw, "bgGUIMetCount");
	} else if (key == CREDITS) {
		/* the ship that crosses the roll, then the score screen's art and lettering */
		add_sprite(&tpags, dw, sprite_named(runner, "sShip"), UINT32_MAX);
		add_background(&tpags, dw, "bgScoreScreen");
		for (i = 0; i < dw->sprt.count; i++)
			if (dw->sprt.sprites[i].name && strncmp(dw->sprt.sprites[i].name, "sGUIFont", 8) == 0)
				add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
	} else {
		if (key % METROID >= INTRO) {
			add_background(&tpags, dw, "bgIntro");
			add_background(&tpags, dw, "bgLoading");
		}
		for (i = 0; i < dw->sprt.count; i++)
			if (dw->sprt.sprites[i].name && (strncmp(dw->sprt.sprites[i].name, "sMap", 4) == 0 ||
							 strncmp(dw->sprt.sprites[i].name, "sGUIFont", 8) == 0))
				add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
		add_suit(&tpags, runner, key % METROID % INTRO / 2, key & 1);
		for (i = 0; i < dw->sprt.count; i++)
			if (dw->sprt.sprites[i].name && strncmp(dw->sprt.sprites[i].name, "sSubScr", 7) == 0)
				add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
		add_background(&tpags, dw, "bg_SubScr");
		add_background(&tpags, dw, "bgMapScreen");
		for (k = 0; k < METROID_KINDS; k++)
			for (i = 0; (key / METROID) & (1 << k) && i < dw->sprt.count; i++)
				if (dw->sprt.sprites[i].name && dw->sprt.sprites[i].name[0] == 's' &&
				    strncmp(dw->sprt.sprites[i].name + 1, metroidKinds[k], strlen(metroidKinds[k])) == 0)
					add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
	}
	GxRenderer_wish(dw, tpags, (uint32_t)arrlen(tpags));
	logInfo("wish: %s, metroids %x, %d texture items\n", key == TITLE_START ? "title start" : key == FILE_SELECT ?
		"file select" : key == CREDITS ? "credits" : key % METROID >= INTRO ? "intro" : key & 1 ? "play, fusion" :
		"play", (int)(key / METROID), (int)arrlen(tpags));
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

static int32_t mapSurfCode = -1, mapBlockCode = -1, mystrVar, partVars[7];
static BuiltinFunc stringFn, charAtFn;

/* draw_map_surf(i, j) runs for each of the pause map's 1,682 cells and spends ~30 us a cell turning the cell's string
 * into seven of the caller's variables; here they are set the same way, before the same draw_mapblock. */
static RValue wii_draw_map_surf(VMContext *ctx, RValue *args, int32_t argCount)
{
	Instance *self = ctx->currentInstance, *g = ctx->globalScopeInstance;
	RValue cellVal, mystr, parts[7], call[10], r;
	int32_t cell, k;

	if (argCount != 2 || !self || !g)
		return VM_callCodeIndex(ctx, mapSurfCode, args, argCount);
	cell = RValue_toInt32(args[0]) * GML_LEGACY_ARRAY_STRIDE + RValue_toInt32(args[1]);
	VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_X, "x", RValue_makeReal((RValue_toReal(args[0]) - 3) * 8), -1);
	VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_Y, "y", RValue_makeReal((RValue_toReal(args[1]) - 3) * 8), -1);
	cellVal = GMLArray_getOnArrayRef(IntRValueHashMap_getOrInsertUndefined(&g->selfVars, mapVar), cell);
	mystr = stringFn(ctx, &cellVal, 1);
	Instance_setSelfVar(self, mystrVar, mystr);
	if (mystr.type == RVALUE_STRING && mystr.string && strcmp(mystr.string, "0") != 0) {
		for (k = 0; k < 7; k++) {
			RValue a[2] = { mystr, RValue_makeInt32(k + 1) };

			parts[k] = charAtFn(ctx, a, 2);
			Instance_setSelfVar(self, partVars[k], parts[k]);
		}
		call[0] = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_X, "x", -1);
		call[1] = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_Y, "y", -1);
		for (k = 0; k < 6; k++)
			call[2 + k] = parts[k];
		call[8] = GMLArray_getOnArrayRef(IntRValueHashMap_getOrInsertUndefined(&g->selfVars, dmapVar), cell);
		call[9] = parts[6];
		r = VM_callCodeIndex(ctx, mapBlockCode, call, 10);
		RValue_free(&r);
		RValue_free(&call[0]);
		RValue_free(&call[1]);
		for (k = 0; k < 7; k++)
			RValue_free(&parts[k]);
	}
	RValue_free(&mystr);
	return RValue_makeUndefined();
}

/* Ours, then AM2R's, in its markup (* heading, / centred, a=b paired, ; next row). The roll scrolls at a speed set by
 * its row count to last as long as its music, so these rows make it a little faster, not longer. */
static const char credits[] = "*AM2R GX;;;;;;;;;;;;"
	"*Wii Port;/cmyksoda;;"
	"*Beta Testers;bonkmaykr=Erzaui;NiuuS=Splatterpunk;;"
	"*Controller Screen Artist;/Asophia Mills;;"
	"*Made Possible By;/AM2R Community Developers;/The Butterscotch Team;/devkitPro;/DoctorM64;/The Dolphin Team;;"
	"*Special Thanks;/My sweet girlfriend,;/for always supporting me <3;;/syl, for being the sweetest syl ever;;;;"
	"*Another ";

/* AM2R's title card (its name, then 11 blank rows) heads its section the way AM2R heads Metroid II's staff. */
#define AM2R_TITLE " 2 Remake;;;;;;;;;;;;"
#define AM2R_HEADER " 2 Remake Staff;;"

void Wii_addCredits(DataWin *dw)
{
	static char *am2r;

	repeat(dw->strg.count, i) {
		const char *s = dw->strg.strings[i];

		if (strcmp(s, "*Another ") == 0) {
			dw->strg.strings[i] = credits;
		} else if (!am2r && strncmp(s, AM2R_TITLE, sizeof AM2R_TITLE - 1) == 0) {
			am2r = safeMalloc(sizeof AM2R_HEADER + strlen(s + sizeof AM2R_TITLE - 1));
			strcpy(am2r, AM2R_HEADER);
			strcat(am2r, s + sizeof AM2R_TITLE - 1);
			dw->strg.strings[i] = am2r;
		}
	}
}

void Wii_hookAm2rScripts(VMContext *ctx)
{
	static const char *const partNames[7] = { "wallU", "wallR", "wallD", "wallL", "color", "special", "corner" };
	ptrdiff_t s = shgeti(ctx->codeIndexByName, "reset_map"), t = shgeti(ctx->codeIndexByName, "draw_cool_text");
	ptrdiff_t m = shgeti(ctx->codeIndexByName, "draw_map_surf"), b = shgeti(ctx->codeIndexByName, "draw_mapblock");
	bool resetMap, mapSurf;
	int k;

	resetMapCode = s >= 0 ? ctx->codeIndexByName[s].value : -1;
	coolTextCode = t >= 0 ? ctx->codeIndexByName[t].value : -1;
	mapVar = Wii_varId(ctx->runner, "map", INSTANCE_GLOBAL);
	dmapVar = Wii_varId(ctx->runner, "dmap", INSTANCE_GLOBAL);
	logOptionVar = Wii_varId(ctx->runner, "opwritelog", INSTANCE_GLOBAL);
	iVar = Wii_varId(ctx->runner, "i", INSTANCE_SELF);
	jVar = Wii_varId(ctx->runner, "j", INSTANCE_SELF);
	resetMap = resetMapCode >= 0 && mapVar >= 0 && dmapVar >= 0 && iVar >= 0 && jVar >= 0;
	mapSurfCode = m >= 0 ? ctx->codeIndexByName[m].value : -1;
	mapBlockCode = b >= 0 ? ctx->codeIndexByName[b].value : -1;
	mystrVar = Wii_varId(ctx->runner, "mystr", INSTANCE_SELF);
	stringFn = VM_findBuiltin(ctx, "string");
	charAtFn = VM_findBuiltin(ctx, "string_char_at");
	mapSurf = mapSurfCode >= 0 && mapBlockCode >= 0 && mapVar >= 0 && dmapVar >= 0 && mystrVar >= 0 && stringFn && charAtFn;
	for (k = 0; k < 7; k++) {
		partVars[k] = Wii_varId(ctx->runner, partNames[k], INSTANCE_SELF);
		mapSurf = mapSurf && partVars[k] >= 0;
	}
	repeat(ctx->funcCallCacheCount, fi) {
		const char *name = ctx->dataWin->func.functions[fi].name;
		void *standIn = NULL;

		if (name && resetMap && strcmp(name, "reset_map") == 0)
			standIn = (void *)wii_reset_map;
		else if (name && coolTextCode >= 0 && strcmp(name, "draw_cool_text") == 0)
			standIn = (void *)wii_draw_cool_text;
		else if (name && mapSurf && strcmp(name, "draw_map_surf") == 0)
			standIn = (void *)wii_draw_map_surf;
		if (standIn) {
			ctx->funcCallCache[fi].builtin = standIn;
			ctx->funcCallCache[fi].scriptCodeIndex = -1;
		}
	}
}
