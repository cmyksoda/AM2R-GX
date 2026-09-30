#include "page_loader.h"

#include <gccore.h>
#include <stdio.h>
#include <string.h>

/* Below the main thread (64) and the audio decoder (80): it only runs while both are asleep. */
#define LOADER_PRIO 32
#define LOADER_STACK (32 * 1024)
/* The card is let go between chunks, so a music read never waits behind a whole page. */
#define READ_CHUNK (64 * 1024)

enum { IDLE, RUNNING, OVER };

static lwp_t thread = LWP_THREAD_NULL;
static sem_t wake, over;
static FILE *file;
static PageJob job;
static volatile int state;
static volatile bool jobOk, quit;
static WiiPngWork work;

static bool read_blob(void)
{
	u32 done = 0;

	if (fseek(file, (long)job.offset, SEEK_SET) != 0)
		return false;
	while (done < job.size) {
		u32 n = job.size - done < READ_CHUNK ? job.size - done : READ_CHUNK;

		if (fread(job.blob + done, 1, n, file) != n)
			return false;
		done += n;
	}
	return true;
}

static void *loader_main(void *arg)
{
	(void)arg;
	for (;;) {
		LWP_SemWait(wake);
		if (quit)
			break;
		if (job.clear)
			memset(job.clear, 0, job.clearBytes);
		jobOk = read_blob() && WiiPng_decodeRowsIn(&work, job.blob, job.size, job.emit, job.user);
		state = OVER;
		LWP_SemPost(over);
	}
	return NULL;
}

bool PageLoader_start(const char *dataWinPath)
{
	if (thread != LWP_THREAD_NULL)
		return true;
	file = fopen(dataWinPath, "rb");
	if (!file)
		return false;
	/* Unbuffered: no stdio buffer to allocate from this thread, and the chunks go straight to the card. */
	setvbuf(file, NULL, _IONBF, 0);
	LWP_SemInit(&wake, 0, 1);
	LWP_SemInit(&over, 0, 1);
	state = IDLE;
	quit = false;
	if (LWP_CreateThread(&thread, loader_main, NULL, NULL, LOADER_STACK, LOADER_PRIO) != 0) {
		thread = LWP_THREAD_NULL;
		LWP_SemDestroy(wake);
		LWP_SemDestroy(over);
		fclose(file);
		file = NULL;
		return false;
	}
	return true;
}

void PageLoader_stop(void)
{
	if (thread == LWP_THREAD_NULL)
		return;
	PageLoader_wait();
	quit = true;
	LWP_SemPost(wake);
	LWP_JoinThread(thread, NULL);
	thread = LWP_THREAD_NULL;
	LWP_SemDestroy(wake);
	LWP_SemDestroy(over);
	fclose(file);
	file = NULL;
}

bool PageLoader_busy(void)
{
	return thread == LWP_THREAD_NULL || state != IDLE;
}

bool PageLoader_submit(const PageJob *j)
{
	if (PageLoader_busy())
		return false;
	job = *j;
	state = RUNNING;
	LWP_SemPost(wake);
	return true;
}

bool PageLoader_poll(bool *ok)
{
	if (state != OVER)
		return false;
	LWP_SemWait(over);
	state = IDLE;
	*ok = jobOk;
	return true;
}

bool PageLoader_wait(void)
{
	if (state == IDLE)
		return false;
	LWP_SemWait(over);
	state = IDLE;
	return jobOk;
}
