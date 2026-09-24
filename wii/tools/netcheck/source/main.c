/* Diagnostic for the hands-free runner: shows and records what wiiload's arguments arrive as, and exercises
 * the game's LOGTO exit path (netlog.c, linked from wii/src) without the game. Returns to the loader by itself. */
#include "netlog.h"
#include "wii_paths.h"

#include <fat.h>
#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void *xfb;

int main(int argc, char **argv)
{
	GXRModeObj *rmode;
	FILE *f;
	int i;

	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	printf("\n\nnetcheck: argc=%d\n", argc);
	for (i = 0; i < argc; i++)
		printf("  argv[%d] = \"%s\"\n", i, argv[i] ? argv[i] : "(null)");
	for (i = 0; i < argc; i++)
		if (argv[i] && !strncmp(argv[i], "LOGTO=", 6))
			NetLog_setTarget(argv[i] + 6);

	if (!fatInitDefault()) {
		printf("no SD\n");
	} else {
		f = fopen(WII_LOG_PATH, "w");
		if (f) {
			fprintf(f, "argc=%d\n", argc);
			for (i = 0; i < argc; i++)
				fprintf(f, "argv[%d]=%s\n", i, argv[i] ? argv[i] : "(null)");
			fclose(f);
			printf("wrote " WII_LOG_PATH "\n");
		}
		NetLog_init();
		NetLog_send();
	}
	printf("back to the loader in 3 s\n");
	for (i = 0; i < 180; i++)
		VIDEO_WaitVSync();
	return 0;
}
