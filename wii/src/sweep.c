#include "sweep.h"

#include "data_win.h"
#include "gx_renderer.h"
#include "instance.h"
#include "log.h"
#include "profiler.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "stb_ds.h"
#include "vm.h"
#include "warp.h"
#include "wii_mem.h"
#include "wii_paths.h"

#include <gccore.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#define SWEEP_FILE WII_GAME_DIR "/sweep.txt"
#define SHOTS_FILE WII_GAME_DIR "/shots.txt"
#define SHOTS_MAX 64
#define SHOT_HOLD_VSYNCS 60
#define USAGE_LINE_BYTES 96
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
static uint8_t *usage;
static int tapeRoom = -1;
static bool tour; /* "tour 1" in sweep.txt: late-game kit, and every room gets the action script */

/* One short burst of ordinary play, the same in every room: run, jump, shoot, aim up, morph and
 * bomb, then a held jump to the right (space jump + screw attack). `every` > 0 taps instead of holds. */
typedef struct {
	int from, to, vk, every;
} TourKey;
static const TourKey tourScript[] = {
	{ 40, 160, VK_RIGHT, 0 },  { 80, 100, 'Z', 0 },       { 60, 160, 'X', 8 },
	{ 170, 290, VK_LEFT, 0 },  { 210, 230, 'Z', 0 },      { 170, 290, 'X', 8 },
	{ 300, 340, VK_UP, 0 },    { 300, 340, 'X', 6 },
	{ 350, 354, VK_DOWN, 0 },  { 360, 364, VK_DOWN, 0 },  { 370, 420, 'X', 12 },
	{ 370, 420, VK_RIGHT, 0 }, { 425, 429, VK_UP, 0 },    { 435, 439, VK_UP, 0 },
	{ 450, 560, VK_RIGHT, 0 }, { 450, 560, 'Z', 22 },
};
static const char *const tourKit[] = {
	"morphball", "jumpball", "powergrip", "spacejump", "screwattack", "hijump", "spiderball",
	"speedbooster", "bomb", "ibeam", "wbeam", "pbeam", "sbeam", "cbeam",
};
static uint32_t usageBytes;
static char shotLine[160];

static bool sweep_load(void)
{
	FILE *f = fopen(SWEEP_FILE, "r");
	char line[160];
	SweepStop s;
	int i;

	if (!f)
		return false;
	while (fgets(line, sizeof line, f) && stopCount < SWEEP_MAX) {
		if (sscanf(line, "dwell %d", &dwell) == 1)
			continue;
		if (sscanf(line, "tour %d", &i) == 1) {
			tour = i != 0;
			continue;
		}
		memset(&s, 0, sizeof s);
		if (sscanf(line, "%d %d %d %d %d %d %d %d", &s.room, &s.x, &s.y, &s.camx, &s.camy,
		           &s.dropX, &s.dropY, &s.dropEvery) >= 5)
			stops[stopCount++] = s;
	}
	fclose(f);
	if (dwell < SETTLE_FRAMES * 2)
		dwell = SETTLE_FRAMES * 2;
	logInfo("SWEEP armed stops=%d dwell=%d tour=%d\n", stopCount, dwell, (int)tour);
	return stopCount > 0;
}

static Instance *sweep_samus(Runner *runner)
{
	int32_t character = Wii_am2rIds(runner)->character;
	Instance **list = character >= 0 ? runner->instancesByExactObject[character] : NULL;

	return arrlen(list) > 0 ? list[0] : NULL;
}

/* Topped up on every room so ten seconds of clumsy play cannot end in a game over. */
static void tour_kit(Runner *runner)
{
	uint32_t i;

	for (i = 0; i < sizeof tourKit / sizeof tourKit[0]; i++)
		Wii_setGlobal(runner, tourKit[i], 1);
	Wii_setGlobal(runner, "currentsuit", 2);
	Wii_setGlobal(runner, "etanks", 10);
	Wii_setGlobal(runner, "maxhealth", 1099);
	Wii_setGlobal(runner, "playerhealth", 1099);
	Wii_setGlobal(runner, "maxmissiles", 250);
	Wii_setGlobal(runner, "missiles", 250);
	Wii_setGlobal(runner, "maxsmissiles", 30);
	Wii_setGlobal(runner, "smissiles", 30);
	Wii_setGlobal(runner, "maxpbombs", 20);
	Wii_setGlobal(runner, "pbombs", 20);
}

static void tour_keys(Runner *runner, int frame)
{
	static bool down[GML_KEY_COUNT];
	bool want[GML_KEY_COUNT] = { false };
	uint32_t i;
	int vk;

	for (i = 0; i < sizeof tourScript / sizeof tourScript[0]; i++) {
		const TourKey *k = &tourScript[i];

		if (frame >= k->from && frame < k->to && (k->every == 0 || (frame - k->from) % k->every < 3))
			want[k->vk] = true;
	}
	for (vk = 0; vk < GML_KEY_COUNT; vk++) {
		if (want[vk] && !down[vk])
			RunnerKeyboard_onKeyDown(runner->keyboard, vk);
		if (!want[vk] && down[vk])
			RunnerKeyboard_onKeyUp(runner->keyboard, vk);
		down[vk] = want[vk];
	}
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
	usage = GxRenderer_tpagUsage(&usageBytes);
	if (usage)
		memset(usage, 0, usageBytes);
	if (tour)
		tour_kit(runner);
}

/* Which graphics this room drew, as hex: the host packs rooms' graphics onto shared pages from it. */
static void sweep_usage(void)
{
	char hex[USAGE_LINE_BYTES * 2 + 1];
	uint32_t off, i, n;
	bool any;

	for (off = 0; usage && off < usageBytes; off += USAGE_LINE_BYTES) {
		n = usageBytes - off < USAGE_LINE_BYTES ? usageBytes - off : USAGE_LINE_BYTES;
		for (i = 0, any = false; i < n; i++) {
			any = any || usage[off + i];
			snprintf(hex + i * 2, 3, "%02x", usage[off + i]);
		}
		if (any)
			logInfo("SWEEP tpag room=%d off=%u %s\n", stops[cur].room, (unsigned)off, hex);
	}
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
	logInfo("SWEEP leave room=%d actual=%d inst=%d ops_avg=%u ops_max=%u arena_min=%u heapfree=%d audo=%u used=%u surfs=%u surfbytes=%u pagebytes=%u texfree=%u\n",
	        stops[cur].room, runner->currentRoomIndex, live,
	        (unsigned)(opsSum / (uint64_t)(dwell - SETTLE_FRAMES)), (unsigned)opsMax,
	        (unsigned)arenaMin, mi.fordblks, (unsigned)audoBytes, (unsigned)mi.uordblks,
	        (unsigned)surfN, (unsigned)surfBytes, (unsigned)pageBytes, (unsigned)WiiMem_texFreeBytes());
	sweep_usage();
}

/* A replay's fingerprint: two runs are in sync iff these lines match. The RNG word moves on every
 * random() call, so it catches a desync long before Samus ends up in a different room. */
static void tape_mark(Runner *runner, const char *what)
{
	Instance *samus = sweep_samus(runner);

	logInfo("TAPE %s frame=%d room=%d sx=%d sy=%d rng=%08x.%u\n", what, runner->frameCount,
	        runner->currentRoomIndex, samus ? (int)samus->x : -1, samus ? (int)samus->y : -1,
	        (unsigned)runner->random.state[runner->random.index & 15], (unsigned)runner->random.index);
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
	if (shotCount > 0 && runner->currentRoomIndex != tapeRoom) {
		tapeRoom = runner->currentRoomIndex;
		tape_mark(runner, "room");
	}
	if (next >= shotCount || runner->frameCount < shotFrames[next])
		return;
	tape_mark(runner, "mark");
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
		if (runner->currentRoomIndex != Wii_am2rIds(runner)->landingRoom || ++landed < WARP_SETTLE_FRAMES)
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

	if (tour && runner->keyboard)
		tour_keys(runner, framesIn);
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
