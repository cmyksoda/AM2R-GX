#ifndef AM2R_WII_MEM_H
#define AM2R_WII_MEM_H

#include <gctypes.h>
#include <stdbool.h>

/* Call before the big allocations, while sbrk is still in MEM1. */
void WiiMem_init(void);

/* Bytes malloc can still hand out (free heap + arena sbrk can actually reach). */
u32 WiiMem_room(void);

/* Hand the top of MEM2 to textures, leaving malloc heapReserve bytes of it. Call once the game
 * data is loaded, before the first texture. */
void WiiMem_reserveTextures(u32 heapReserve);
/* True while there is no arena and textures still come from malloc (evicting them frees heap). */
bool WiiMem_texturesInHeap(void);

/* 32-byte-aligned texture memory from the arena. NULL when full: evict something and retry. */
void *WiiMem_texAlloc(u32 bytes);
void WiiMem_texFree(void *p);
u32 WiiMem_texFreeBytes(void);

#endif
