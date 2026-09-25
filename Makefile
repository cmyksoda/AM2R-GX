# AM2R-Wii build. The wii target resets and re-patches the pinned Butterscotch submodule.

BUTTER := third_party/butterscotch
WII_BUILD := build/wii
HOST_BUILD := build/host
SHA := aee2c4b1e50d7fe9ae6d8ee78abdd92234cc3d80
AM2R_DATA ?= working/extracted/win11
SD_GAME := dist/apps/am2r/game
DOLPHIN_USER := project-notes/research/evidence-I/dolphin-user
WII_SD := $(DOLPHIN_USER)/Load/WiiSDSync
WII_CMAKE_FLAGS := \
	-DCMAKE_TOOLCHAIN_FILE="$(CURDIR)/wii/cmake/wii.cmake" \
	-DPLATFORM=wii \
	-DCMAKE_BUILD_TYPE=Release \
	-DAUDIO_BACKEND=none \
	-DENABLE_WAD14=ON -DENABLE_WAD16=ON -DENABLE_WAD17=OFF \
	-DENABLE_NOOP_RENDERER=OFF -DENABLE_LEGACY_GL=OFF -DENABLE_MODERN_GL=OFF \
	-DENABLE_VM_GML_PROFILER=ON -DENABLE_VM_STUB_LOGS=ON -DENABLE_VM_TRACING=OFF \
	-DENABLE_ASAN=OFF \
	-DBUTTERSCOTCH_COMMIT_HASH="$(SHA)" \
	-DBUTTERSCOTCH_COMMIT_DATE="2026-09-10"

.PHONY: check check-bless all hello run run-wii clean wii wii-warn wii-sd sd-pack patchkit host-smoke

all: hello

hello:
	$(MAKE) -C hello

run: hello
	$(MAKE) -C hello run

# Push the AM2R DOL to a networked Homebrew Channel. Needs WIILOAD=tcp:<wii-ip>.
# Game data still comes from the SD card; profile.txt/saves land there too.
run-wii:
	@test -f $(WII_BUILD)/boot.dol || { echo "no $(WII_BUILD)/boot.dol — run 'make wii' first"; exit 1; }
	@test -n "$(WIILOAD)" || { echo "set WIILOAD=tcp:<wii-ip> (HBC at its main menu)"; exit 1; }
	wiiload $(WII_BUILD)/boot.dol

# Overlay our wii/ onto the pinned clone, apply patches, cross-compile.
wii: $(BUTTER)/CMakeLists.txt
	mkdir -p $(BUTTER)/src/wii
	rsync -a --delete wii/src/ $(BUTTER)/src/wii/
	# The local test hooks (wii/test/, never in the repository) join the build when they are on disk.
	if [ -d wii/test/src ]; then rsync -a wii/test/src/ $(BUTTER)/src/wii/; fi
	cd $(BUTTER) && git checkout -- CMakeLists.txt src/gettime.c src/loop.c src/runner.c src/vm_builtins.c src/vm_builtins.h
	cd $(BUTTER) && git apply ../../wii/patches/0001-cmake-add-PLATFORM-wii.patch
	cd $(BUTTER) && git apply ../../wii/patches/0002-gettime-wii.patch
	cd $(BUTTER) && git apply ../../wii/patches/0003-loop-no-sigaction-on-wii.patch
	cd $(BUTTER) && git apply ../../wii/patches/0004-loop-gx-renderer.patch
	cd $(BUTTER) && git apply ../../wii/patches/0005-skip-bgscreenguide.patch
	cd $(BUTTER) && git apply ../../wii/patches/0006-loop-gui-window-size.patch
	cd $(BUTTER) && git apply ../../wii/patches/0008-vm-file-copy.patch
	cd $(BUTTER) && git apply ../../wii/patches/0009-loop-wii-audio.patch
	cd $(BUTTER) && git apply ../../wii/patches/0010-vm-audio-is-paused-wii.patch
	cd $(BUTTER) && git apply ../../wii/patches/0011-runner-no-collision-events-after-destroy.patch
	cd $(BUTTER) && git apply ../../wii/patches/0012-vm-game-id.patch
	cd $(BUTTER) && git apply ../../wii/patches/0013-runner-persistent-room-keeps-payload.patch
	cd $(BUTTER) && git apply ../../wii/patches/0014-runner-persistent-room-keeps-inactive.patch
	if [ -f wii/test/patches/0099-debug-collision-trace.patch ]; then cd $(BUTTER) && git apply ../../wii/test/patches/0099-debug-collision-trace.patch; fi
	cmake -B $(WII_BUILD) -S $(BUTTER) $(WII_CMAKE_FLAGS)
	cmake --build $(WII_BUILD) -j$(shell nproc)
	mkdir -p dist/apps/am2r
	cp -f $(WII_BUILD)/boot.dol dist/apps/am2r/boot.dol
	cp -f wii/hbc/meta.xml dist/apps/am2r/meta.xml
	cp -f wii/hbc/icon.png dist/apps/am2r/icon.png
	cp -f LICENSE dist/apps/am2r/LICENSE.txt
	cp -f NOTICE dist/apps/am2r/NOTICE.txt
	cp -f wii/src/xdelta3/LICENSE dist/apps/am2r/LICENSE-xdelta3.txt

# The first-boot patch kit: the community 1.1 -> 1.5.5 delta, the texture plan the console follows, and
# the loose 1.5.5 files (no game data inside). Ends up next to boot.dol as apps/am2r/patch/.
AUTOPATCHER ?= working/extracted/autopatcher/windows/data
CU155 ?= working/extracted/cu155/data.win
GROUPS ?= working/repacked/cu155/groups-v3.json
patchkit:
	mkdir -p dist/apps/am2r/patch/files build
	# Re-encoded with an 8 MB source window: the community delta copies from all over the 1.1 file and
	# the console's 14 MB source cache re-read 2.7 GB (vWii: 328 s of an 8:49 first boot); this one re-reads 27 MB.
	xdelta3 -e -f -S lzma -B 8388608 -W 8388608 -s $(AM2R_DATA)/data.win $(CU155) dist/apps/am2r/patch/data.xdelta
	# Effect sprites on a page of their own, which the game keeps resident (gx_pin_effects).
	python3 tools/effects_group.py $(CU155) $(GROUPS) build/groups-effects.json
	python3 tools/repack_textures.py $(CU155) build/plan-repacked.win --groups build/groups-effects.json --report build/plan.json
	python3 tools/repack_plan.py $(CU155) build/plan.json dist/apps/am2r/patch/plan.bin
	rsync -a --delete $(AUTOPATCHER)/files_to_copy/ dist/apps/am2r/patch/files/
	cp -f $(AUTOPATCHER)/../LICENSE dist/apps/am2r/patch/LICENSE.txt
	du -sh dist/apps/am2r/patch

# Same tree, second build dir, unused/shadow warnings on for src/wii/ only (upstream's are noise).
wii-warn: wii
	cmake -B $(WII_BUILD)-warn -S $(BUTTER) -DWII_STRICT=ON $(WII_CMAKE_FLAGS)
	cmake --build $(WII_BUILD)-warn -j$(shell nproc) 2>&1 | grep -E "src/wii/.*warning" || echo "no warnings in src/wii/"
	@ls -l dist/apps/am2r/boot.dol

wii-sd:
	mkdir -p $(SD_GAME)/lang $(WII_SD)/apps/am2r/game/lang $(WII_SD)/apps/am2r/save
	ln -sfn "$(CURDIR)/$(AM2R_DATA)/data.win" $(SD_GAME)/data.win
	cp -f project-notes/research/evidence-I/am2r_inputs.json $(SD_GAME)/am2r_inputs.json
	cp -f project-notes/research/evidence-I/warp.txt $(SD_GAME)/warp.txt
	cp -f $(AM2R_DATA)/lang/languages.txt $(AM2R_DATA)/lang/english.ini $(SD_GAME)/lang/
	ln -sfn "$(CURDIR)/$(AM2R_DATA)/data.win" $(WII_SD)/apps/am2r/game/data.win
	cp -f project-notes/research/evidence-I/am2r_inputs.json $(WII_SD)/apps/am2r/game/am2r_inputs.json
	cp -f project-notes/research/evidence-I/warp.txt $(WII_SD)/apps/am2r/game/warp.txt
	cp -f $(AM2R_DATA)/lang/languages.txt $(AM2R_DATA)/lang/english.ini $(WII_SD)/apps/am2r/game/lang/
	@echo "SD image: $(WII_SD)/apps/am2r/"

# Assemble a copy-to-SD tree (boot.dol + your data.win + lang). One-time setup;
# after that, iterate with 'make wii && make run-wii'. Never commits data.win.
sd-pack:
	tools/sd-pack.sh "$(CURDIR)/$(AM2R_DATA)" "$(CURDIR)/dist/sd"

# Regression check in Dolphin: two tapes shot against a blessed reference DOL, then a short room sweep.
# The harness lives in project-notes/research/scripts (untracked); bless a build you trust first.
CHECK := project-notes/research/scripts/check.py
check: wii
	@test -f $(CHECK) || { echo "no $(CHECK): the Dolphin harness is not on this machine"; exit 1; }
	python3 $(CHECK) --dol $(WII_BUILD)/boot.dol

check-bless: wii
	python3 $(CHECK) --dol $(WII_BUILD)/boot.dol --bless

host-smoke: $(BUTTER)/CMakeLists.txt
	cmake -B $(HOST_BUILD) -S $(BUTTER) \
		-DPLATFORM=cli -DBACKEND=noop -DAUDIO_BACKEND=none \
		-DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=OFF \
		-DENABLE_WAD14=ON -DENABLE_WAD16=OFF -DENABLE_WAD17=OFF
	cmake --build $(HOST_BUILD) -j$(shell nproc)

clean:
	$(MAKE) -C hello clean
	rm -rf dist $(WII_BUILD) $(HOST_BUILD)
