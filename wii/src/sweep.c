#include "sweep.h"

#include "data_win.h"
#include "gx_renderer.h"
#include "instance.h"
#include "log.h"
#include "profiler.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"
#include "warp.h"
#include "wii_paths.h"

#include <gccore.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#define SWEEP_FILE WII_GAME_DIR "/sweep.txt"
#define SHOTS_FILE WII_GAME_DIR "/shots.txt"
#define SHOTS_MAX 64
#define SHOT_HOLD_VSYNCS 60
#define SWEEP_MAX 400
#define SETTLE_FRAMES 30
#define SOAK_TICK_FRAMES 600
#define SOAK_TOP 6
#define SOAK_MAX_OBJECTS 2048

typedef struct {
	int room, x, y, camx, camy;
	int dropX, dropY, dropEvery; /* optional: re-drop Samus here every N frames (splash soaks) */
} SweepStop;

static SweepStop stops[SWEEP_MAX];
static int stopCount;
static int state; /* 0 unread, -1 off, 1 waiting for gameplay, 2 dwelling, 3 done */
static int cur = -1;
static int dwell = 300;
static int landed, framesIn;
static uint64_t lastOps, opsSum;
static uint32_t opsMax, arenaMin, soakOps; /* soakOps: worst frame since the last soak line */
static bool shotDue, shotLast;
static char shotLine[160];

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
		memset(&s, 0, sizeof s);
		if (sscanf(line, "%d %d %d %d %d %d %d %d", &s.room, &s.x, &s.y, &s.camx, &s.camy,
		           &s.dropX, &s.dropY, &s.dropEvery) >= 5)
			stops[stopCount++] = s;
	}
	fclose(f);
	if (dwell < SETTLE_FRAMES * 2)
		dwell = SETTLE_FRAMES * 2;
	logInfo("SWEEP armed stops=%d dwell=%d\n", stopCount, dwell);
	return stopCount > 0;
}

static Instance *sweep_samus(Runner *runner)
{
	Instance **list = runner->instancesByExactObject[AM2R_OBJ_CHARACTER];

	return arrlen(list) > 0 ? list[0] : NULL;
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

/* Long dwells (soaks) want a time series: whatever keeps growing is the leak. */
static void sweep_soak_tick(Runner *runner)
{
	static uint16_t counts[SOAK_MAX_OBJECTS];
	int32_t i, n = (int32_t)arrlen(runner->instances), live = 0, t, best;
	uint32_t nobj = runner->dataWin->objt.count;
	char line[256];
	int len;

	if (nobj > SOAK_MAX_OBJECTS)
		nobj = SOAK_MAX_OBJECTS;
	memset(counts, 0, sizeof counts);
	for (i = 0; i < n; i++) {
		Instance *inst = runner->instances[i];

		if (!inst || inst->destroyed)
			continue;
		live++;
		if (inst->objectIndex >= 0 && (uint32_t)inst->objectIndex < nobj)
			counts[inst->objectIndex]++;
	}
	{
		Instance *samus = sweep_samus(runner);

		len = snprintf(line, sizeof line, "SWEEP soak room=%d f=%d inst=%d ops=%u sy=%d room_mem=%u top=",
		               stops[cur].room, framesIn, live, (unsigned)soakOps, samus ? (int)samus->y : -1,
		               (unsigned)(SYS_GetArena1Size() + SYS_GetArena2Size()));
		soakOps = 0;
	}
	for (t = 0; t < SOAK_TOP && len < (int)sizeof line - 40; t++) {
		best = -1;
		for (i = 0; i < (int32_t)nobj; i++)
			if (counts[i] && (best < 0 || counts[i] > counts[best]))
				best = i;
		if (best < 0)
			break;
		len += snprintf(line + len, sizeof line - len, "%s%s:%u", t ? "," : "",
		                runner->dataWin->objt.objects[best].name, counts[best]);
		counts[best] = 0;
	}
	logInfo("%s\n", line);
}

/* In-room move only: no room change, so oCharacter's Room Start doesn't re-place her. */
static void sweep_drop(Runner *runner)
{
	Instance *samus = sweep_samus(runner);

	if (!samus || samus->destroyed)
		return;
	samus->x = samus->xprevious = (float)stops[cur].dropX;
	samus->y = samus->yprevious = (float)stops[cur].dropY;
	samus->spatialGridDirty = true;
}

static void sweep_shot(Runner *runner)
{
	Instance *samus = sweep_samus(runner);
	GMLCamera *cam = Runner_getCameraForView(runner, 0);

	snprintf(shotLine, sizeof shotLine, "SWEEP shot room=%d actual=%d sx=%d sy=%d vis=%d vx=%d vy=%d\n",
	        stops[cur].room, runner->currentRoomIndex,
	        samus ? (int)samus->x : -1, samus ? (int)samus->y : -1, samus ? (int)samus->visible : -1,
	        cam ? (int)cam->viewX : -1, cam ? (int)cam->viewY : -1);
	shotDue = true;
}

static void sweep_leave(Runner *runner)
{
	int32_t i, n = (int32_t)arrlen(runner->instances), live = 0;
	struct mallinfo mi = mallinfo();
	Audo *audo = &runner->dataWin->audo;
	uint32_t k, audoBytes = 0, surfN, surfBytes, pageBytes;

	for (i = 0; i < n; i++)
		if (runner->instances[i] && !runner->instances[i]->destroyed)
			live++;
	GxRenderer_memStats(&surfN, &surfBytes, &pageBytes);
	for (k = 0; k < audo->count; k++)
		if (audo->entries[k].data)
			audoBytes += audo->entries[k].dataSize;
	logInfo("SWEEP leave room=%d actual=%d inst=%d ops_avg=%u ops_max=%u arena_min=%u heapfree=%d audo=%u used=%u surfs=%u surfbytes=%u pagebytes=%u\n",
	        stops[cur].room, runner->currentRoomIndex, live,
	        (unsigned)(opsSum / (uint64_t)(dwell - SETTLE_FRAMES)), (unsigned)opsMax,
	        (unsigned)arenaMin, mi.fordblks, (unsigned)audoBytes, (unsigned)mi.uordblks,
	        (unsigned)surfN, (unsigned)surfBytes, (unsigned)pageBytes);
}

/* shots.txt: frame numbers at which to announce "TAPE shot", so the host can screenshot a
 * tape run at exact frames however slowly the emulator is going. */
static void shots_tick(Runner *runner)
{
	static int shotFrames[SHOTS_MAX];
	static int shotCount = -1, next;
	FILE *f;

	if (shotCount < 0) {
		shotCount = 0;
		f = fopen(SHOTS_FILE, "r");
		if (!f)
			return;
		while (shotCount < SHOTS_MAX && fscanf(f, "%d", &shotFrames[shotCount]) == 1)
			shotCount++;
		fclose(f);
		logInfo("TAPE armed shots=%d\n", shotCount);
	}
	if (next >= shotCount || runner->frameCount < shotFrames[next])
		return;
	snprintf(shotLine, sizeof shotLine, "TAPE shot frame=%d room=%d\n", runner->frameCount, runner->currentRoomIndex);
	shotLast = ++next == shotCount;
	shotDue = true;
}

/* Announce only once the frame is on screen, then freeze it: the host's grab lands up to half a
 * second late, and a scrolling backdrop made identical builds look 79,000 pixels apart. */
void Wii_shotHold(void)
{
	int i;

	if (!shotDue)
		return;
	shotDue = false;
	logInfo("%s", shotLine);
	for (i = 0; i < SHOT_HOLD_VSYNCS; i++)
		VIDEO_WaitVSync();
	if (shotLast)
		logInfo("TAPE done\n");
	shotLast = false;
}

void Wii_sweepTick(Runner *runner)
{
	uint64_t ops;
	uint32_t delta, arena;

	shots_tick(runner);
	if (state < 0 || state == 3)
		return;
	if (state == 0) {
		state = sweep_load() ? 1 : -1;
		return;
	}
	if (state == 1) {
		if (runner->currentRoomIndex != AM2R_ROOM_LANDING || ++landed < WARP_SETTLE_FRAMES)
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
	if (delta > soakOps)
		soakOps = delta;
	/* The first frames carry room-start cost; they count toward max but not the average. */
	if (framesIn >= SETTLE_FRAMES)
		opsSum += delta;
	framesIn++;

	if (stops[cur].dropEvery > 0 && framesIn > SETTLE_FRAMES && (framesIn % stops[cur].dropEvery) == 0)
		sweep_drop(runner);
	if (framesIn == dwell / 2)
		sweep_shot(runner);
	if (dwell > SOAK_TICK_FRAMES && (framesIn % SOAK_TICK_FRAMES) == 0)
		sweep_soak_tick(runner);
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
