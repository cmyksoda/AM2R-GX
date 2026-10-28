# AM2R-Wii build. The wii target resets and re-patches the pinned Butterscotch submodule.

BUTTER := third_party/butterscotch
WII_BUILD := build/wii
HOST_BUILD := build/host
SHA := d05526846e75449295ed2596bd86e9db0108408a
AM2R_DATA ?= working/extracted/win11
SD_GAME := dist/apps/am2r/game
DOLPHIN_USER := project-notes/research/evidence-I/dolphin-user
# HOOKS=0 leaves the local test hooks and the per-script profiler out of the build (make release does).
HOOKS ?= 1
# Build jobs: the PC is often shared with other emulator runs.
JOBS ?= $(shell nproc)
VERSION ?= $(shell sed -n 's/^.define AM2R_GX_VERSION "\([^"]*\)".*/\1/p' wii/src/main.c)
META ?= wii/hbc/meta.xml
WII_SD := $(DOLPHIN_USER)/Load/WiiSDSync
WII_CMAKE_FLAGS := \
	-DCMAKE_TOOLCHAIN_FILE="$(CURDIR)/wii/cmake/wii.cmake" \
	-DPLATFORM=wii \
	-DCMAKE_BUILD_TYPE=Release \
	-DAUDIO_BACKEND=none \
	-DENABLE_WAD14=ON -DENABLE_WAD16=ON -DENABLE_WAD17=OFF \
	-DENABLE_NOOP_RENDERER=OFF -DENABLE_LEGACY_GL=OFF -DENABLE_MODERN_GL=OFF \
	-DENABLE_VM_GML_PROFILER=$(if $(filter 0,$(HOOKS)),OFF,ON) -DENABLE_VM_STUB_LOGS=ON -DENABLE_VM_TRACING=OFF -DENABLE_VM_OPCODE_PROFILER=OFF \
	-DENABLE_ASAN=OFF \
	-DBUTTERSCOTCH_COMMIT_HASH="$(SHA)" \
	-DBUTTERSCOTCH_COMMIT_DATE="2026-09-23"

.PHONY: check check-bless all run-wii clean wii wii-warn wii-sd sd-pack patchkit host-smoke release

all: wii

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
	if [ "$(HOOKS)" != 0 ] && [ -d wii/test/src ]; then rsync -a wii/test/src/ $(BUTTER)/src/wii/; fi
	xxd -i -n controller_art_png wii/art/classic-controller-pro.png > $(BUTTER)/src/wii/controller_art.h
	cd $(BUTTER) && git checkout -- CMakeLists.txt src/collision.h src/data_win.c src/data_win.h src/gettime.c src/ini.c src/instance.c src/instance.h src/loop.c src/matrix_math.h src/random.c src/random.h src/real_type.h src/renderer.h src/runner.c src/runner.h src/spatial_grid.c src/spatial_grid.h src/vm.c src/vm.h src/vm_builtins.c src/vm_builtins.h
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
	cd $(BUTTER) && git apply ../../wii/patches/0015-runner-destroyed-self-still-reaches-other.patch
	cd $(BUTTER) && git apply ../../wii/patches/0016-ini-skip-utf8-bom.patch
	cd $(BUTTER) && git apply ../../wii/patches/0017-spatial-grid-dirty-flag.patch
	cd $(BUTTER) && git apply ../../wii/patches/0018-runner-clear-view-surfaces.patch
	cd $(BUTTER) && git apply ../../wii/patches/0019-vm-tile-background-and-scale.patch
	cd $(BUTTER) && git apply ../../wii/patches/0020-vm-random-get-seed.patch
	cd $(BUTTER) && git apply ../../wii/patches/0021-runner-waking-instance-joins-grid.patch
	cd $(BUTTER) && git apply ../../wii/patches/0022-random-set-seed-resets-index.patch
	cd $(BUTTER) && git apply ../../wii/patches/0023-vm-string-char-at-no-copies.patch
	cd $(BUTTER) && git apply ../../wii/patches/0024-vm-export-array-set.patch
	cd $(BUTTER) && git apply ../../wii/patches/0025-runner-destroy-scans-id-holders.patch
	cd $(BUTTER) && git apply ../../wii/patches/0026-vm-collision-line-uses-the-grid.patch
	cd $(BUTTER) && git apply ../../wii/patches/0027-spatial-grid-live-instances-skip-full-scan.patch
	cd $(BUTTER) && git apply ../../wii/patches/0028-matrix-zero-angle-transform.patch
	cd $(BUTTER) && git apply ../../wii/patches/0029-collision-line-reuse-bounds.patch
	cd $(BUTTER) && git apply ../../wii/patches/0031-direction-range-modulo.patch
	cd $(BUTTER) && git apply ../../wii/patches/0032-collision-line-grid-bounds.patch
	cd $(BUTTER) && git apply ../../wii/patches/0033-collision-clip-divisions.patch
	cd $(BUTTER) && git apply ../../wii/patches/0034-list-set-arguments.patch
	cd $(BUTTER) && git apply ../../wii/patches/0035-seed-unused-argument.patch
	cd $(BUTTER) && git apply ../../wii/patches/0036-event-slot-reuse.patch
	cd $(BUTTER) && git apply ../../wii/patches/0037-collision-unit-scale.patch
	cd $(BUTTER) && git apply ../../wii/patches/0038-cardinal-motion.patch
	cd $(BUTTER) && git apply ../../wii/patches/0039-reentrant-event-snapshots.patch
	cd $(BUTTER) && git apply ../../wii/patches/0040-collision-query-wrap.patch
	cd $(BUTTER) && git apply ../../wii/patches/0041-mouse-snapshot-relocation.patch
	cd $(BUTTER) && git apply ../../wii/patches/0042-event-null-snapshots.patch
	cd $(BUTTER) && git apply ../../wii/patches/0043-draw-list-keeps-sorted-tiles.patch
	cd $(BUTTER) && git apply ../../wii/patches/0044-floor-without-libm.patch
	cd $(BUTTER) && git apply ../../wii/patches/0045-bounding-boxes-are-remembered.patch
	cd $(BUTTER) && git apply ../../wii/patches/0046-draw-events-walk-only-instances.patch
	cd $(BUTTER) && git apply ../../wii/patches/0047-tiles-share-their-layer-lookup.patch
	cd $(BUTTER) && git apply ../../wii/patches/0048-call-frames-recycle-their-blocks.patch
	cd $(BUTTER) && git apply ../../wii/patches/0049-operand-sizes-from-a-table.patch
	cd $(BUTTER) && git apply ../../wii/patches/0050-region-activation-checks-remembered-boxes-inline.patch
	cd $(BUTTER) && git apply ../../wii/patches/0051-code-entries-can-run-native-code.patch
	cd $(BUTTER) && git apply ../../wii/patches/0052-tiles-are-found-by-id-through-an-index.patch
	cd $(BUTTER) && git apply ../../wii/patches/0054-a-discarded-view-passes-over-its-tiles.patch
	cd $(BUTTER) && git apply ../../wii/patches/0055-instances-that-last-entered-a-depth-draw-first.patch
	cd $(BUTTER) && git apply ../../wii/patches/0056-collision-events-find-hits-through-the-grid.patch
	cd $(BUTTER) && git apply ../../wii/patches/0057-point-and-rectangle-queries-use-the-grid.patch
	cd $(BUTTER) && git apply ../../wii/patches/0058-precise-scans-turn-each-instance-once.patch
	cd $(BUTTER) && git apply ../../wii/patches/0059-collision-walks-reuse-the-last-sorted-order.patch
	cd $(BUTTER) && git apply ../../wii/patches/0060-place-checks-use-the-grid.patch
	cd $(BUTTER) && git apply ../../wii/patches/0061-destroys-look-only-at-variables-given-ids.patch
	cd $(BUTTER) && git apply ../../wii/patches/0062-region-activation-uses-an-index-of-inactive-instances.patch
	cd $(BUTTER) && git apply ../../wii/patches/0063-tile-edits-keep-the-array-and-index-current.patch
	cd $(BUTTER) && git apply ../../wii/patches/0064-draw-list-keeps-its-tile-run.patch
	cd $(BUTTER) && git apply ../../wii/patches/0065-off-screen-tiles-are-passed-over-through-a-grid.patch
	cd $(BUTTER) && git apply ../../wii/patches/0066-activation-index-survives-creates-wakes-and-destroys.patch
	cd $(BUTTER) && git apply ../../wii/patches/0067-crowded-cells-look-for-the-first-hit-in-the-target-list.patch
	cd $(BUTTER) && git apply ../../wii/patches/0068-tile-index-settles-a-miss.patch
	if [ "$(HOOKS)" != 0 ] && [ -f wii/test/patches/0099-debug-collision-trace.patch ]; then cd $(BUTTER) && git apply ../../wii/test/patches/0099-debug-collision-trace.patch; fi
	cmake -B $(WII_BUILD) -S $(BUTTER) $(WII_CMAKE_FLAGS)
	cmake --build $(WII_BUILD) -j$(JOBS)
	mkdir -p dist/apps/am2r
	cp -f $(WII_BUILD)/boot.dol dist/apps/am2r/boot.dol
	# The Homebrew Channel art and metadata are not in the repository; they join the release when on disk.
	for f in meta.xml icon.png; do [ ! -f wii/hbc/$$f ] || cp -f wii/hbc/$$f dist/apps/am2r/$$f; done
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
	cmake --build $(WII_BUILD)-warn -j$(JOBS) > $(WII_BUILD)-warn/build.log 2>&1 || { tail -20 $(WII_BUILD)-warn/build.log; exit 1; }
	grep -E "src/wii/.*warning" $(WII_BUILD)-warn/build.log || echo "no warnings in src/wii/"
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

# Assemble a copy-to-SD tree from an already-patched 1.5.5 folder (boot.dol, data.win, lang, mods, music), for
# development. Never commits data.win.
sd-pack:
	tools/sd-pack.sh "$(CURDIR)/$(dir $(CU155))" "$(CURDIR)/dist/sd"

# The release zip: the DOL built without test hooks (its symbols must say so), meta.xml at the same version, the
# patch kit and the licenses, as dist/am2r-gx-VERSION.zip. The ELF stays beside it to read a crash dump's addresses.
REL := dist/package-$(VERSION)/am2r-gx-$(VERSION)/apps/am2r
release:
	@test ! -e dist/am2r-gx-$(VERSION).zip || { echo "dist/am2r-gx-$(VERSION).zip exists: bump AM2R_GX_VERSION"; exit 1; }
	@grep -q "<version>$(VERSION)</version>" $(META) || { echo "$(META) is not version $(VERSION)"; exit 1; }
	@test -f dist/apps/am2r/patch/data.xdelta || { echo "no patch kit in dist/apps/am2r/patch: make patchkit"; exit 1; }
	$(MAKE) wii HOOKS=0 WII_BUILD=build/wii-release
	@! $(DEVKITPPC)/bin/powerpc-eabi-nm build/wii-release/butterscotch | grep -q -e " T Wii_hooksBoot" -e NetLog_init || \
		{ echo "build/wii-release has the test hooks in it"; exit 1; }
	rm -rf dist/package-$(VERSION)
	mkdir -p $(REL)
	cp build/wii-release/boot.dol wii/hbc/icon.png $(REL)/
	cp $(META) $(REL)/meta.xml
	cp LICENSE $(REL)/LICENSE.txt
	cp NOTICE $(REL)/NOTICE.txt
	cp wii/src/xdelta3/LICENSE $(REL)/LICENSE-xdelta3.txt
	rsync -a dist/apps/am2r/patch $(REL)/
	cd dist/package-$(VERSION) && zip -qr ../am2r-gx-$(VERSION).zip am2r-gx-$(VERSION)
	cp build/wii-release/butterscotch dist/am2r-gx-$(VERSION).elf
	@sha1sum dist/am2r-gx-$(VERSION).zip $(REL)/boot.dol

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
	cmake --build $(HOST_BUILD) -j$(JOBS)

clean:
	rm -rf dist $(WII_BUILD) $(HOST_BUILD)
