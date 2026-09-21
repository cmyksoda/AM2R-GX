#ifndef AM2R_WII_AUDIO_SYSTEM_H
#define AM2R_WII_AUDIO_SYSTEM_H

#include "audio_system.h"
#include "data_win.h"

/* AESND-backed audio: embedded WAV SFX from data.win, external OGG music streamed via stb_vorbis. */
AudioSystem* WiiAudioSystem_create(DataWin* dataWin);

/* Backs the audio_is_paused builtin (patch 0010). */
bool WiiAudio_isPaused(AudioSystem* audio, int32_t idOrSound);

#endif
