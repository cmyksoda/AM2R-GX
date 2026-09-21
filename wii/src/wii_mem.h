#ifndef AM2R_WII_MEM_H
#define AM2R_WII_MEM_H

#include <gctypes.h>

/* Call before the big allocations, while sbrk is still in MEM1. */
void WiiMem_init(void);

/* Bytes malloc can still hand out (free heap + arena sbrk can actually reach). */
u32 WiiMem_room(void);

/* 32-byte-aligned texture memory. Prefers the part of MEM1 that sbrk abandons. */
void *WiiMem_texAlloc(u32 bytes);
void WiiMem_texFree(void *p);

#endif
