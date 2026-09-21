#include "wii_mem.h"

#include "log.h"

#include <gccore.h>
#include <malloc.h>
#include <string.h>

#define UNIT (1u << 20)
#define MAX_UNITS 24
#define RUN_CONT 0xFF

static void *arena2Base;
static u8 *poolLo;
static u32 unitCount;
static u8 unitRun[MAX_UNITS]; /* run length on a run's first unit, RUN_CONT on the rest, 0 = free */

void WiiMem_init(void)
{
	arena2Base = SYS_GetArena2Lo();
}

static bool sbrk_in_mem2(void)
{
	return SYS_GetArena2Lo() != arena2Base;
}

/* libogc's sbrk moves to MEM2 the first time a request doesn't fit in MEM1 and never comes
 * back, abandoning whatever MEM1 had left (~8 MB here). Take it over for texture pages. */
static void claim_mem1(void)
{
	u8 *lo, *hi;

	if (poolLo || !sbrk_in_mem2())
		return;
	lo = (u8 *)(((u32)SYS_GetArena1Lo() + 31) & ~31u);
	hi = (u8 *)SYS_GetArena1Hi();
	if (hi < lo + UNIT)
		return;
	unitCount = (u32)(hi - lo) / UNIT;
	if (unitCount > MAX_UNITS)
		unitCount = MAX_UNITS;
	SYS_SetArena1Lo(hi);
	poolLo = lo;
	logInfo("WiiMem: claimed %u MB of abandoned MEM1 for texture pages\n", (unsigned)unitCount);
}

u32 WiiMem_room(void)
{
	struct mallinfo mi = mallinfo();
	u32 arena = SYS_GetArena2Size();

	if (!sbrk_in_mem2())
		arena += SYS_GetArena1Size();
	return arena + (u32)mi.fordblks;
}

void *WiiMem_texAlloc(u32 bytes)
{
	u32 need = (bytes + UNIT - 1) / UNIT;
	u32 i, run = 0;

	claim_mem1();
	for (i = 0; poolLo && need <= RUN_CONT - 1 && i < unitCount; i++) {
		run = unitRun[i] ? 0 : run + 1;
		if (run < need)
			continue;
		i -= need - 1;
		memset(&unitRun[i], RUN_CONT, need);
		unitRun[i] = (u8)need;
		return poolLo + i * UNIT;
	}
	return memalign(32, bytes);
}

void WiiMem_texFree(void *p)
{
	u32 i;

	if (!p)
		return;
	if (!poolLo || (u8 *)p < poolLo || (u8 *)p >= poolLo + unitCount * UNIT) {
		free(p);
		return;
	}
	i = (u32)((u8 *)p - poolLo) / UNIT;
	memset(&unitRun[i], 0, unitRun[i]);
}
