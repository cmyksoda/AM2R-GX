#ifndef AM2R_WII_AUDIO_SYSTEM_H
#define AM2R_WII_AUDIO_SYSTEM_H

#include "audio_system.h"
#include "data_win.h"

/* AESND-backed audio: embedded WAV SFX from data.win. External OGG music is out of scope. */
AudioSystem* WiiAudioSystem_create(DataWin* dataWin);

#endif
