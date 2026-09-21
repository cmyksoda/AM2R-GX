#include "input.h"
#include "log.h"
#include "loop.h"
#include "overscan.h"
#include "wii_log.h"
#include "wii_mem.h"
#include "wii_video.h"

#include <fat.h>
#include <gccore.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GAME_DIR "sd:/apps/am2r"
#define DATA_WIN "sd:/apps/am2r/game/data.win"
#define SAVE_DIR "sd:/apps/am2r/save"
#define LOG_PATH "sd:/apps/am2r/profile.txt"

static void wait_home(const char *why)
{
	logError("%s\nHold HOME to quit.\n", why);
	WiiVideo_panic("startup failed", why);
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

/* Dolphin hands out zeroed RAM; a real Wii doesn't. Dirtying the unused arenas makes
 * uninitialised reads misbehave in the emulator the way they would on hardware. */
static void poison_arenas(void)
{
	memset(SYS_GetArena1Lo(), 0xA5, SYS_GetArena1Size());
	memset(SYS_GetArena2Lo(), 0xA5, SYS_GetArena2Size());
	logInfo("POISON sentinel found -- unused arenas filled with 0xA5\n");
}

int main(int argc, char **argv)
{
	CommandLineArgs args;
	int rc;

	(void)argc;

	WiiMem_init();
	WiiInput_init();
	if (!WiiVideo_init()) {
		wait_home("WiiVideo_init failed");
		return 1;
	}

	if (!fatInitDefault()) {
		wait_home("fatInitDefault failed (no SD?)");
		return 1;
	}

	mkdir(GAME_DIR, 0777);
	mkdir(GAME_DIR "/game", 0777);
	mkdir(SAVE_DIR, 0777);

	if (chdir(GAME_DIR) != 0)
		logWarn("chdir(%s) failed — paths stay absolute\n", GAME_DIR);

	if (!WiiLog_open(LOG_PATH))
		logWarn("could not open %s — console only\n", LOG_PATH);

	Overscan_load();

	logInfo("AM2R GX (live input)\n");
	logInfo("mode %ux%u xfb=%u aspect=%s\n", WiiVideo_fbWidth(), WiiVideo_efbHeight(),
		WiiVideo_rmode() ? WiiVideo_rmode()->xfbHeight : 0,
		WiiVideo_isWide() ? "16:9 stretch" : "4:3 fill");
	log_mallinfo("boot");

	if (access(DATA_WIN, R_OK) != 0) {
		wait_home("data.win not found at sd:/apps/am2r/game/ -- run 'make sd-pack' and copy apps/ onto the SD card");
		WiiLog_close();
		return 1;
	}

	memset(&args, 0, sizeof(args));
	args.exitAtFrame = -1; // run until the player holds HOME
	args.speedMultiplier = 1.0;
	args.fastForwardSpeed = 0.0;
	args.osType = OS_WINDOWS;
	args.windowWidth = WII_FB_W;
	args.windowHeight = WII_FB_H;
	args.profilerFramesBetween = 200; // periodic ms/frame; enough for perf without per-frame SD spam
	args.loadType = DATAWINLOADTYPE_LOAD_PER_CHUNK;
	args.lazyRooms = true;
	args.lazyTextures = true;
	args.lazyAudio = true;
	args.headless = false;
	args.traceFrames = false;
	args.renderer = NOOP;
	args.dataWinPath = DATA_WIN;
	args.saveFolder = SAVE_DIR;

	/* Dolphin-only tape replay: a PLAYBACK sentinel beside the game enables it.
	 * Never present on real SD cards, so hardware keeps live controller input. */
	if (access(GAME_DIR "/game/PLAYBACK", R_OK) == 0) {
		args.playbackInputsPath = GAME_DIR "/game/am2r_inputs.json";
		WiiLog_enableGecko();
		logInfo("PLAYBACK sentinel found -- replaying %s\n", args.playbackInputsPath);
	}
	if (access(GAME_DIR "/game/POISON", R_OK) == 0)
		poison_arenas();

	rc = loop(args, argv[0] ? argv[0] : "boot.dol");
	log_mallinfo("exit");
	logInfo("loop returned %d\n", rc);
	freeCommandLineArgs(&args);
	WiiLog_close();
	return rc;
}
