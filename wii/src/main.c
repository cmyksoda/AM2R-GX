#include "gx_renderer.h"
#include "input.h"
#include "log.h"
#include "loop.h"
#include "hooks.h"
#include "overscan.h"
#include "patcher.h"
#include "wii_log.h"
#include "wii_mem.h"
#include "wii_paths.h"
#include "wii_video.h"

#include <fat.h>
#include <gccore.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAVE_DIR WII_APP_DIR "/save"

static void wait_home(const char *why, bool haveLog)
{
	logError("%s\nHold HOME to quit.\n", why);
	WiiVideo_panic("could not start", why, haveLog);
	while (SYS_MainLoop()) {
		if (WiiInput_shouldQuit())
			break;
		VIDEO_WaitVSync();
	}
}

static void log_mallinfo(const char *when)
{
	struct mallinfo mi = mallinfo();

	logInfo("mallinfo %s: arena=%d ordblks=%d uordblks=%d fordblks=%d keepcost=%d\n",
	        when, mi.arena, mi.ordblks, mi.uordblks, mi.fordblks, mi.keepcost);
	/* Once sbrk crosses into MEM2, mallinfo counts the ~240 MB address hole as in-use.
	 * The arenas are the trustworthy figure. */
	logInfo("arena %s: mem1_free=%u mem2_free=%u\n",
	        when, (unsigned)SYS_GetArena1Size(), (unsigned)SYS_GetArena2Size());
}

int main(int argc, char **argv)
{
	CommandLineArgs args;
	const char *tape;
	int rc;

	WiiMem_init();
	WiiInput_init();
	if (!WiiVideo_init()) {
		wait_home("the video could not start", false);
		return 1;
	}

	/* 256 pages (1 MB) instead of libfat's 16 KB default: the game stats a file every frame, and
	 * with the directory sectors evicted each stat cost 17 ms on a real card (vWii, session 9). */
	if (!fatInit(256, true)) {
		wait_home("No SD card found. Put the SD card with the apps/am2r folder into the Wii's front slot, then start AM2R GX again.", false);
		return 1;
	}

	mkdir(WII_APP_DIR, 0777);
	mkdir(WII_GAME_DIR, 0777);
	mkdir(SAVE_DIR, 0777);

	if (chdir(WII_APP_DIR) != 0)
		logWarn("chdir(%s) failed — paths stay absolute\n", WII_APP_DIR);

	if (!WiiLog_open(WII_LOG_PATH))
		logWarn("could not open %s — console only\n", WII_LOG_PATH);
	Wii_hooksBoot(argc, argv);

	Overscan_load();
	WiiVideo_setActiveWidth(Overscan_viWidth());

	logInfo("AM2R GX\n");
	logInfo("mode %ux%u xfb=%u aspect=%s\n", WiiVideo_fbWidth(), WiiVideo_efbHeight(),
		WiiVideo_rmode() ? WiiVideo_rmode()->xfbHeight : 0,
		WiiVideo_isWide() ? "16:9 stretch" : "4:3 fill");
	log_mallinfo("boot");

	WiiPatcher_run();
	if (access(WII_DATA_WIN, R_OK) != 0) {
		wait_home(WiiPatcher_error() ? WiiPatcher_error()
		                             : "No game data yet. Put your AM2R_11.zip (the AM2R 1.1 download) into " WII_APP_DIR
		                               "/, next to the patch folder, and start AM2R GX again: the first boot builds AM2R 1.5.5 from it.",
		          true);
		WiiLog_close();
		return 1;
	}

	memset(&args, 0, sizeof(args));
	args.exitAtFrame = -1; // run until the player holds HOME
	args.speedMultiplier = 1.0;
	args.osType = OS_WINDOWS;
	args.windowWidth = WII_FB_W;
	args.windowHeight = WII_FB_H;
	args.profilerFramesBetween = 200; // periodic ms/frame; enough for perf without per-frame SD spam
	args.loadType = DATAWINLOADTYPE_LOAD_PER_CHUNK;
	args.lazyRooms = true;
	args.lazyTextures = true;
	args.lazyAudio = true;
	args.renderer = NOOP;
	args.dataWinPath = WII_DATA_WIN;
	args.saveFolder = SAVE_DIR;

	if ((tape = Wii_hooksTape())) {
		args.playbackInputsPath = tape;
		/* 1.5.5 calls randomize() at boot; a tape only replays if that stays a no-op. */
		args.hasSeed = true;
		logInfo("replaying %s\n", tape);
	}
	Wii_hooksBeforeLoop();

	rc = loop(args, argv[0] ? argv[0] : "boot.dol");
	if (GxRenderer_fatal())
		wait_home(GxRenderer_fatal(), true);
	log_mallinfo("exit");
	logInfo("loop returned %d\n", rc);
	freeCommandLineArgs(&args);
	WiiLog_close();
	Wii_hooksExit(); /* after the close: the log file must be complete */
	return rc;
}
