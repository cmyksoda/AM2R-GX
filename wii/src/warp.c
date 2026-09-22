#include "warp.h"

#include "instance.h"
#include "log.h"
#include "runner.h"
#include "rvalue.h"
#include "stb_ds.h"
#include "vm.h"
#include "wii_paths.h"

#include <stdio.h>
#include <string.h>

void Wii_setGlobal(Runner *runner, const char *name, int value)
{
	VMContext *vm = runner->vmContext;
	ptrdiff_t slot;

	if (!vm)
		return;
	slot = shgeti(vm->varNameMap, (char *)name);
	if (slot >= 0 && vm->globalScopeInstance)
		Instance_setSelfVar(vm->globalScopeInstance, vm->varNameMap[slot].value, RValue_makeInt32(value));
}

const Am2rIds *Wii_am2rIds(Runner *runner)
{
	static Am2rIds ids = { -1, -1, -1 };
	static bool resolved;
	DataWin *dw = runner->dataWin;
	uint32_t i;

	if (resolved || !dw)
		return &ids;
	for (i = 0; i < dw->objt.count; i++) {
		if (strcmp(dw->objt.objects[i].name, "oCharacter") == 0)
			ids.character = (int32_t)i;
		else if (strcmp(dw->objt.objects[i].name, "oCamera") == 0)
			ids.camera = (int32_t)i;
	}
	for (i = 0; i < dw->room.count; i++)
		if (strcmp(dw->room.rooms[i].name, "rm_a0h01") == 0)
			ids.landingRoom = (int32_t)i;
	resolved = true;
	logInfo("Wii: oCharacter=%d oCamera=%d rm_a0h01=%d\n", (int)ids.character, (int)ids.camera, (int)ids.landingRoom);
	return &ids;
}

/* The move a door makes. oCharacter's Room Start re-places Samus from global.target*+offset*,
 * so the globals are what actually position her; the direct x/y only covers the first frame. */
void Wii_warpTo(Runner *runner, int room, int x, int y, int camx, int camy)
{
	const Am2rIds *ids;
	Instance **list;
	Instance *inst;
	int32_t n, i;

	if (!runner->instancesByExactObject || !runner->dataWin || !runner->vmContext)
		return;
	ids = Wii_am2rIds(runner);
	if (ids->character < 0)
		return;
	Wii_setGlobal(runner, "targetx", x);
	Wii_setGlobal(runner, "targety", y);
	Wii_setGlobal(runner, "offsetx", 0);
	Wii_setGlobal(runner, "offsety", 0);
	Wii_setGlobal(runner, "camstartx", camx);
	Wii_setGlobal(runner, "camstarty", camy);
	list = runner->instancesByExactObject[ids->character];
	n = (int32_t)arrlen(list);
	for (i = 0; i < n; i++) {
		inst = list[i];
		if (!inst || inst->destroyed)
			continue;
		inst->x = inst->xprevious = inst->xstart = (float)x;
		inst->y = inst->yprevious = inst->ystart = (float)y;
		inst->spatialGridDirty = true;
	}
	if (ids->camera >= 0) {
		list = runner->instancesByExactObject[ids->camera];
		n = (int32_t)arrlen(list);
		for (i = 0; i < n; i++) {
			inst = list[i];
			if (!inst || inst->destroyed)
				continue;
			inst->x = (float)camx;
			inst->y = (float)camy;
		}
	}
	runner->pendingRoom = room;
	logInfo("Wii: warp to room %d at %d,%d\n", room, x, y);
}

/* Optional debug warp: warp.txt beside the game holds "room x y". */
void Wii_tryWarp(Runner *runner)
{
	static int armed;
	static int landed;
	static int room, x, y;
	FILE *f;
	char line[128];

	if (armed < 0)
		return;
	if (armed == 0) {
		f = fopen(WII_GAME_DIR "/warp.txt", "r");
		if (!f) {
			armed = -1;
			return;
		}
		room = x = y = -1;
		while (fgets(line, sizeof line, f)) {
			if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
				continue;
			if (sscanf(line, "%d %d %d", &room, &x, &y) == 3)
				break;
		}
		fclose(f);
		if (room < 0) {
			armed = -1;
			return;
		}
		armed = 1;
		logInfo("Wii: warp.txt room=%d (%d,%d)\n", room, x, y);
	}
	if (runner->currentRoomIndex != Wii_am2rIds(runner)->landingRoom)
		return;
	if (++landed != WARP_SETTLE_FRAMES)
		return;
	Wii_warpTo(runner, room, x, y, x, y);
}

/* FUSION marker (Dolphin only): oControl.mod_fusion = 1 once in the landing room, which is what
 * the Fusion difficulty sets - it is an unlockable, so a fresh save can't reach it by menu. */
void Wii_tryFusion(Runner *runner)
{
	static int armed;
	static int32_t varId = -1;
	VMContext *vm = runner ? runner->vmContext : NULL;
	Instance **list;
	int32_t n, i, control = -1;
	uint32_t o;
	FILE *f;

	if (armed < 0 || !vm || !runner->instancesByExactObject)
		return;
	if (armed == 0) {
		f = fopen(WII_GAME_DIR "/FUSION", "r");
		if (!f) {
			armed = -1;
			return;
		}
		fclose(f);
		armed = 1;
	}
	if (runner->currentRoomIndex != Wii_am2rIds(runner)->landingRoom)
		return;
	for (o = 0; o < runner->dataWin->objt.count; o++)
		if (strcmp(runner->dataWin->objt.objects[o].name, "oControl") == 0)
			control = (int32_t)o;
	if (varId < 0) {
		ptrdiff_t slot = shgeti(vm->varNameMap, "mod_fusion");

		varId = slot >= 0 ? vm->varNameMap[slot].value : -1;
	}
	if (control < 0 || varId < 0) {
		armed = -1;
		return;
	}
	/* msr_fusionsuit (default 1) draws separate Samus Returns-style sprites and never touches the
	 * palette shader; the AM2R-style suit (0) is the one that recolours through it. */
	list = runner->instancesByExactObject[control];
	n = (int32_t)arrlen(list);
	for (i = 0; i < n; i++) {
		ptrdiff_t ms = shgeti(vm->varNameMap, "msr_fusionsuit");

		if (!list[i] || list[i]->destroyed)
			continue;
		Instance_setSelfVar(list[i], varId, RValue_makeInt32(1));
		if (ms >= 0)
			Instance_setSelfVar(list[i], vm->varNameMap[ms].value, RValue_makeInt32(0));
	}
	logInfo("Wii: FUSION -> oControl.mod_fusion = 1, msr_fusionsuit = 0 (%d instances)\n", (int)n);
	armed = -1;
}

/* global.controltype picks the prompt glyph set: 0 keyboard, 1 numbered pad, 2 named A/B pad.
 * check_keys resets it every step from the last input seen; our keyboard synthesis makes it
 * flip. Pin it to 2 after the step so prompts stay the named-button set. Gameplay unaffected. */
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
