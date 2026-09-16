#include "warp.h"

#include "instance.h"
#include "log.h"
#include "runner.h"
#include "stb_ds.h"

#include <stdio.h>
#include <string.h>

/* Optional sd:/apps/am2r/game/warp.txt: "room x y". Used to reach water without a 4k-px walk. */
void Wii_tryWarp(Runner *runner)
{
	static int armed;
	static int landed;
	static int room, x, y;
	FILE *f;
	char line[128];
	Instance **list;
	Instance *inst;
	int32_t n, i, nobj;

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
	if (++landed < 40)
		return;
	if (landed != 40)
		return;
	if (!runner->instancesByExactObject || !runner->dataWin)
		return;
	nobj = (int32_t)runner->dataWin->objt.count;
	if (nobj <= 266)
		return;
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
			inst->x = (float)x;
			inst->y = (float)y;
		}
	}
	runner->pendingRoom = room;
	logInfo("Wii: warp to room %d at %d,%d\n", room, x, y);
}
