#include "warp.h"

#include "instance.h"
#include "log.h"
#include "runner.h"
#include "rvalue.h"
#include "stb_ds.h"
#include "vm.h"

#include <stdio.h>
#include <string.h>

static void set_global(VMContext *vm, const char *name, int value)
{
	ptrdiff_t slot = shgeti(vm->varNameMap, (char *)name);

	if (slot >= 0 && vm->globalScopeInstance)
		Instance_setSelfVar(vm->globalScopeInstance, vm->varNameMap[slot].value, RValue_makeInt32(value));
}

/* The move a door makes. oCharacter's Room Start re-places Samus from global.target*+offset*,
 * so the globals are what actually position her; the direct x/y only covers the first frame. */
void Wii_warpTo(Runner *runner, int room, int x, int y, int camx, int camy)
{
	Instance **list;
	Instance *inst;
	int32_t n, i, nobj;

	if (!runner->instancesByExactObject || !runner->dataWin || !runner->vmContext)
		return;
	nobj = (int32_t)runner->dataWin->objt.count;
	if (nobj <= 266)
		return;
	set_global(runner->vmContext, "targetx", x);
	set_global(runner->vmContext, "targety", y);
	set_global(runner->vmContext, "offsetx", 0);
	set_global(runner->vmContext, "offsety", 0);
	set_global(runner->vmContext, "camstartx", camx);
	set_global(runner->vmContext, "camstarty", camy);
	list = runner->instancesByExactObject[266];
	n = (int32_t)arrlen(list);
	for (i = 0; i < n; i++) {
		inst = list[i];
		if (!inst || inst->destroyed)
			continue;
		inst->x = inst->xprevious = inst->xstart = (float)x;
		inst->y = inst->yprevious = inst->ystart = (float)y;
		inst->spatialGridDirty = true;
	}
	if (nobj > 426) {
		list = runner->instancesByExactObject[426];
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

/* Optional sd:/apps/am2r/game/warp.txt: "room x y". Used to reach water without a 4k-px walk. */
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
		f = fopen("sd:/apps/am2r/game/warp.txt", "r");
		if (!f)
			f = fopen("game/warp.txt", "r");
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
	if (runner->currentRoomIndex != 21)
		return;
	if (++landed != 40)
		return;
	Wii_warpTo(runner, room, x, y, x, y);
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
