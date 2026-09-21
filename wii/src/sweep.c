#include "sweep.h"

#include "instance.h"
#include "log.h"
#include "profiler.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"
#include "warp.h"

#include <gccore.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#define SWEEP_FILE "sd:/apps/am2r/game/sweep.txt"
#define SWEEP_MAX 400
#define SETTLE_FRAMES 30

typedef struct {
	int room, x, y, camx, camy;
} SweepStop;

static SweepStop stops[SWEEP_MAX];
static int stopCount;
static int state; /* 0 unread, -1 off, 1 waiting for gameplay, 2 dwelling, 3 done */
static int cur = -1;
static int dwell = 300;
static int landed, framesIn;
static uint64_t lastOps, opsSum;
static uint32_t opsMax, arenaMin;

static bool sweep_load(void)
{
	FILE *f = fopen(SWEEP_FILE, "r");
	char line[160];
	SweepStop s;

	if (!f)
		return false;
	while (fgets(line, sizeof line, f) && stopCount < SWEEP_MAX) {
		if (sscanf(line, "dwell %d", &dwell) == 1)
			continue;
		if (sscanf(line, "%d %d %d %d %d", &s.room, &s.x, &s.y, &s.camx, &s.camy) == 5)
			stops[stopCount++] = s;
	}
	fclose(f);
	if (dwell < SETTLE_FRAMES * 2)
		dwell = SETTLE_FRAMES * 2;
	logInfo("SWEEP armed stops=%d dwell=%d\n", stopCount, dwell);
	return stopCount > 0;
}

static uint64_t vm_ops(Runner *runner)
{
	VMContext *vm = runner->vmContext;

	return vm && vm->profiler ? vm->profiler->instructionCount : 0;
}

static void sweep_enter(Runner *runner)
{
	SweepStop *s = &stops[cur];

	logInfo("SWEEP enter i=%d/%d room=%d frame=%d\n", cur + 1, stopCount, s->room, runner->frameCount);
	Wii_warpTo(runner, s->room, s->x, s->y, s->camx, s->camy);
	framesIn = 0;
	opsSum = 0;
	opsMax = 0;
	arenaMin = 0xFFFFFFFFu;
	lastOps = vm_ops(runner);
}

static void sweep_shot(Runner *runner)
{
	Instance **list = runner->instancesByExactObject[266];
	Instance *samus = arrlen(list) > 0 ? list[0] : NULL;
	GMLCamera *cam = Runner_getCameraForView(runner, 0);

	logInfo("SWEEP shot room=%d actual=%d sx=%d sy=%d vis=%d vx=%d vy=%d\n",
	        stops[cur].room, runner->currentRoomIndex,
	        samus ? (int)samus->x : -1, samus ? (int)samus->y : -1, samus ? (int)samus->visible : -1,
	        cam ? (int)cam->viewX : -1, cam ? (int)cam->viewY : -1);
}

static void sweep_leave(Runner *runner)
{
	int32_t i, n = (int32_t)arrlen(runner->instances), live = 0;
	struct mallinfo mi = mallinfo();

	for (i = 0; i < n; i++)
		if (runner->instances[i] && !runner->instances[i]->destroyed)
			live++;
	logInfo("SWEEP leave room=%d actual=%d inst=%d ops_avg=%u ops_max=%u arena_min=%u heapfree=%d\n",
	        stops[cur].room, runner->currentRoomIndex, live,
	        (unsigned)(opsSum / (uint64_t)(dwell - SETTLE_FRAMES)), (unsigned)opsMax,
	        (unsigned)arenaMin, mi.fordblks);
}

void Wii_sweepTick(Runner *runner)
{
	uint64_t ops;
	uint32_t delta, arena;

	if (state < 0 || state == 3)
		return;
	if (state == 0) {
		state = sweep_load() ? 1 : -1;
		return;
	}
	if (state == 1) {
		if (runner->currentRoomIndex != 21 || ++landed < 40)
			return;
		state = 2;
		cur = 0;
		sweep_enter(runner);
		return;
	}

	ops = vm_ops(runner);
	delta = (uint32_t)(ops - lastOps);
	lastOps = ops;
	arena = SYS_GetArena1Size() + SYS_GetArena2Size();
	if (arena < arenaMin)
		arenaMin = arena;
	if (delta > opsMax)
		opsMax = delta;
	/* The first frames carry room-start cost; they count toward max but not the average. */
	if (framesIn >= SETTLE_FRAMES)
		opsSum += delta;
	framesIn++;

	if (framesIn == dwell / 2)
		sweep_shot(runner);
	if (framesIn < dwell)
		return;

	sweep_leave(runner);
	if (++cur >= stopCount) {
		logInfo("SWEEP done\n");
		state = 3;
		return;
	}
	sweep_enter(runner);
}

bool Wii_sweepFinished(void)
{
	return state == 3;
}
