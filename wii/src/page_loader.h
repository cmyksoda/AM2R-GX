#ifndef AM2R_WII_PAGE_LOADER_H
#define AM2R_WII_PAGE_LOADER_H

#include "png_stream.h"

/* One texture page read from data.win and decoded on a low-priority thread, into memory the main
 * thread allocated and keeps owning. The thread never allocates, logs or touches the engine. */
typedef struct {
	u32 offset, size; /* the page's PNG inside data.win */
	u8 *blob;         /* size bytes to read it into */
	void *clear;      /* zeroed before decoding: the padding the rows never reach */
	u32 clearBytes;
	WiiPngRowFn emit;
	void *user;
} PageJob;

bool PageLoader_start(const char *dataWinPath);
void PageLoader_stop(void);
bool PageLoader_busy(void);
/* False while a job is still out. */
bool PageLoader_submit(const PageJob *job);
/* True once the job is over (whether it decoded goes in *ok); never blocks. */
bool PageLoader_poll(bool *ok);
/* Blocks until the job is over; true if it decoded. */
bool PageLoader_wait(void);

#endif
