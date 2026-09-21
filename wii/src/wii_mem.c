#include "wii_mem.h"

#include "log.h"

#include <gccore.h>
#include <malloc.h>
#include <string.h>

#define UNIT (64u << 10)
#define MAX_UNITS 1024 /* 64 MB per region: more than either memory bank can spare */
#define RUN_CONT 0xFFFF

/* run[]: run length on a run's first unit, RUN_CONT on the rest, 0 = free */
typedef struct {
	u8 *lo;
	u32 units;
	u16 run[MAX_UNITS];
} Region;

static void *arena2Base;
static Region mem1, mem2;

void WiiMem_init(void)
{
	arena2Base = SYS_GetArena2Lo();
}

static bool sbrk_in_mem2(void)
{
	return SYS_GetArena2Lo() != arena2Base;
}

static void region_set(Region *r, u8 *lo, u8 *hi)
{
	lo = (u8 *)(((u32)lo + 31) & ~31u);
	if (hi < lo + UNIT)
		return;
	r->units = (u32)(hi - lo) / UNIT;
	if (r->units > MAX_UNITS)
		r->units = MAX_UNITS;
	r->lo = lo;
}

/* libogc's sbrk moves to MEM2 the first time a request doesn't fit in MEM1 and never comes
 * back, abandoning whatever MEM1 had left (~8 MB here). Take it over for textures. */
static void claim_mem1(void)
{
	u8 *hi = (u8 *)SYS_GetArena1Hi();

	if (mem1.lo || !sbrk_in_mem2())
		return;
	region_set(&mem1, (u8 *)SYS_GetArena1Lo(), hi);
	if (!mem1.lo)
		return;
	SYS_SetArena1Lo(hi);
	logInfo("WiiMem: claimed %u MB of abandoned MEM1 for textures\n", (unsigned)(mem1.units * UNIT >> 20));
}

/* Textures churn in megabyte blocks. Left in malloc's heap they leave holes between long-lived
 * small allocations that no full page fits again, so they get the top of MEM2 to themselves. */
void WiiMem_reserveTextures(u32 heapReserve)
{
	u8 *hi = (u8 *)SYS_GetArena2Hi();
	u32 spare = SYS_GetArena2Size();
	u8 *lo;

	if (mem2.lo || spare < heapReserve + UNIT)
		return;
	lo = hi - ((spare - heapReserve) / UNIT) * UNIT;
	region_set(&mem2, lo, hi);
	if (!mem2.lo)
		return;
	SYS_SetArena2Hi(lo);
	logInfo("WiiMem: texture arena %u MB of MEM2, malloc keeps %u MB\n",
	        (unsigned)(mem2.units * UNIT >> 20), (unsigned)(SYS_GetArena2Size() >> 20));
}

bool WiiMem_texturesInHeap(void)
{
	return !mem2.lo;
}

u32 WiiMem_room(void)
{
	struct mallinfo mi = mallinfo();
	u32 arena = SYS_GetArena2Size();

	if (!sbrk_in_mem2())
		arena += SYS_GetArena1Size();
	return arena + (u32)mi.fordblks;
}

/* Best fit: a small surface tile must not split the one hole a full page could have used. */
static void *region_alloc(Region *r, u32 need)
{
	u32 i, start = 0, run = 0, best = 0, bestRun = 0;

	for (i = 0; i <= r->units; i++) {
		if (i < r->units && !r->run[i]) {
			if (!run++)
				start = i;
			continue;
		}
		if (run >= need && (!bestRun || run < bestRun)) {
			best = start;
			bestRun = run;
		}
		run = 0;
	}
	if (!bestRun)
		return NULL;
	for (i = 1; i < need; i++)
		r->run[best + i] = RUN_CONT;
	r->run[best] = (u16)need;
	return r->lo + best * UNIT;
}

static bool region_free(Region *r, void *p)
{
	u32 i, n;

	if (!r->lo || (u8 *)p < r->lo || (u8 *)p >= r->lo + r->units * UNIT)
		return false;
	i = (u32)((u8 *)p - r->lo) / UNIT;
	n = r->run[i];
	memset(&r->run[i], 0, n * sizeof r->run[0]);
	return true;
}

void *WiiMem_texAlloc(u32 bytes)
{
	u32 need = (bytes + UNIT - 1) / UNIT;
	void *p = NULL;

	claim_mem1();
	if (mem1.lo && need < RUN_CONT)
		p = region_alloc(&mem1, need);
	if (!p && mem2.lo && need < RUN_CONT)
		p = region_alloc(&mem2, need);
	if (!p && WiiMem_texturesInHeap())
		p = memalign(32, bytes);
	return p;
}

void WiiMem_texFree(void *p)
{
	if (p && !region_free(&mem1, p) && !region_free(&mem2, p))
		free(p);
}

u32 WiiMem_texFreeBytes(void)
{
	u32 i, n = 0;

	for (i = 0; i < mem1.units; i++)
		n += !mem1.run[i];
	for (i = 0; i < mem2.units; i++)
		n += !mem2.run[i];
	return n * UNIT;
}
