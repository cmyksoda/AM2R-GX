#include "wii_audio_system.h"

#include "common.h"
#include "data_win.h"
#include "log.h"

#include <aesndlib.h>
#include <gccore.h>
#include <malloc.h>
#include <ogc/lwp_watchdog.h>
#include <stdlib.h>
#include <string.h>

#define WII_VOICES 16
#define TAIL_MARGIN_MS 120                // keep a finished one-shot's buffer alive so the DSP can drain it
#define MAX_SFX_BYTES (2 * 1024 * 1024)   // embedded SFX are short; refuse anything larger (music is external OGG)
#define INSTANCE_ID_BASE 100000           // keep instance ids clear of small SOND indices

typedef struct {
    AESNDPB* pb;
    void* buf;              // 32-byte aligned, byte-swapped PCM; owned
    bool playing;
    bool loop;
    int32_t instanceId;
    int32_t soundIndex;
    uint32_t baseFreq;      // source sample rate, for pitch scaling
    uint64_t freeAt;        // gettime() tick at which a one-shot's buffer may be freed; 0 while looping
} WiiVoice;

typedef struct {
    AudioSystem base;
    DataWin* dw;
    WiiVoice voices[WII_VOICES];
    int32_t nextInstanceId;
    float masterGain;
    int32_t dbg;            // throttle for first-N playSound diagnostics
} WiiAudioSystem;

static AudioSystemVtable wiiAudioVtable;

// Walk RIFF chunks to the 'data' payload. Returns its start, or nullptr.
static const uint8_t* findWavDataChunk(const uint8_t* data, uint32_t dataSize, uint32_t* lenOut) {
    if (data == nullptr || dataSize < 12) return nullptr;
    if (data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F') return nullptr;
    uint32_t offset = 12;
    while (offset + 8 <= dataSize) {
        uint32_t chunkLen = data[offset+4] | (data[offset+5] << 8) | (data[offset+6] << 16) | (data[offset+7] << 24);
        uint32_t chunkDataOffset = offset + 8;
        uint32_t available = dataSize - chunkDataOffset;
        if (chunkLen > available) chunkLen = available;
        if (data[offset] == 'd' && data[offset+1] == 'a' && data[offset+2] == 't' && data[offset+3] == 'a') {
            *lenOut = chunkLen;
            return data + chunkDataOffset;
        }
        offset = chunkDataOffset + chunkLen;
        if (offset & 1) offset++;
    }
    return nullptr;
}

static bool parseWavHeader(const uint8_t* data, uint32_t dataSize, uint32_t* chans,
                           uint32_t* rate, uint32_t* bits, const uint8_t** audio, uint32_t* audioLen) {
    if (data == nullptr || dataSize < 36) return false;
    if (data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F') return false;
    *chans = data[22] | (data[23] << 8);
    *rate  = data[24] | (data[25] << 8) | (data[26] << 16) | (data[27] << 24);
    *bits  = data[34] | (data[35] << 8);
    uint32_t len = 0;
    const uint8_t* found = findWavDataChunk(data, dataSize, &len);
    *audio = found;
    *audioLen = len;
    return found != nullptr && len > 0 && *chans > 0 && *rate > 0 && *bits > 0;
}

static void releaseBuf(WiiVoice* v) {
    if (v->buf != nullptr) { free(v->buf); v->buf = nullptr; }
}

// Free one-shot buffers whose playback time (plus tail margin) has elapsed.
static void reap(WiiAudioSystem* a) {
    uint64_t now = gettime();
    for (int i = 0; i < WII_VOICES; i++) {
        WiiVoice* v = &a->voices[i];
        if (v->playing && !v->loop && v->freeAt != 0 && now >= v->freeAt) {
            AESND_SetVoiceStop(v->pb, true);
            releaseBuf(v);
            v->playing = false;
            v->instanceId = -1;
            v->soundIndex = -1;
            v->freeAt = 0;
        }
    }
}

static WiiVoice* findFree(WiiAudioSystem* a) {
    for (int i = 0; i < WII_VOICES; i++)
        if (!a->voices[i].playing) return &a->voices[i];
    return nullptr;
}

// GML passes either a returned instance id or a SOND index; match both.
static WiiVoice* findVoice(WiiAudioSystem* a, int32_t idOrSound) {
    for (int i = 0; i < WII_VOICES; i++) {
        WiiVoice* v = &a->voices[i];
        if (v->playing && (v->instanceId == idOrSound || v->soundIndex == idOrSound)) return v;
    }
    return nullptr;
}

static void wiiInit(AudioSystem* audio, DataWin* dataWin, MAYBE_UNUSED FileSystem* fs) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    a->dw = dataWin;
    a->base.dw = dataWin;
    AESND_Init();
    AESND_Pause(false);
    for (int i = 0; i < WII_VOICES; i++) {
        a->voices[i].pb = AESND_AllocateVoice(nullptr);   // left un-stopped; hello never stopped a voice and was audible
        a->voices[i].instanceId = -1;
        a->voices[i].soundIndex = -1;
    }
    logInfo("WiiAudio: AESND up, %d voices\n", WII_VOICES);
}

static void wiiStopAll(AudioSystem* audio) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    for (int i = 0; i < WII_VOICES; i++) {
        WiiVoice* v = &a->voices[i];
        if (v->pb != nullptr) AESND_SetVoiceStop(v->pb, true);
        releaseBuf(v);
        v->playing = false;
        v->loop = false;
        v->instanceId = -1;
        v->soundIndex = -1;
        v->freeAt = 0;
    }
}

static void wiiDestroy(AudioSystem* audio) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    wiiStopAll(audio);
    for (int i = 0; i < WII_VOICES; i++)
        if (a->voices[i].pb != nullptr) AESND_FreeVoice(a->voices[i].pb);
    free(a);
}

static void wiiUpdate(AudioSystem* audio, MAYBE_UNUSED float dt) {
    reap((WiiAudioSystem*) audio);
}

static int32_t wiiPlaySound(AudioSystem* audio, int32_t soundIndex, MAYBE_UNUSED int32_t priority, bool loop) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    DataWin* dw = a->dw;
    bool dbg = (a->dbg < 24);
    if (dbg) a->dbg++;

    reap(a);
    if (dw == nullptr || soundIndex < 0 || (uint32_t) soundIndex >= dw->sond.count) {
        if (dbg) logInfo("WiiAudio: snd %d out of range\n", soundIndex);
        return -1;
    }
    Sound* sound = &dw->sond.sounds[soundIndex];
    if (dbg) logInfo("WiiAudio: play? snd %d '%s' flags=0x%x grp=%d file=%d\n",
                     soundIndex, sound->name ? sound->name : "?", (unsigned) sound->flags,
                     sound->audioGroup, sound->audioFile);

    bool embedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0
                 || (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0
                 || (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) != AUDIO_ENTRY_FLAG_REGULAR;
    if (!embedded) { if (dbg) logInfo("  skip: not embedded\n"); return -1; }
    if (sound->audioGroup != 0) { if (dbg) logInfo("  skip: group %d\n", sound->audioGroup); return -1; }

    DataWin* group = dw;        // group 0 is data.win; AM2R 1.1 keeps SFX there
    if (sound->audioFile < 0 || (uint32_t) sound->audioFile >= group->audo.count) {
        if (dbg) logInfo("  skip: bad audioFile\n");
        return -1;
    }
    DataWin_loadAudoIfNeeded(group, (uint32_t) sound->audioFile);
    AudioEntry* entry = &group->audo.entries[sound->audioFile];
    if (entry->data == nullptr || entry->dataSize == 0) { if (dbg) logInfo("  skip: no AUDO data\n"); return -1; }

    uint32_t chans = 0, rate = 0, bits = 0, audioLen = 0;
    const uint8_t* audioData = nullptr;
    bool parsed = parseWavHeader(entry->data, entry->dataSize, &chans, &rate, &bits, &audioData, &audioLen);
    if (dbg) logInfo("  audo size=%u first=%02x%02x%02x%02x parsed=%d ch=%u rate=%u bits=%u dlen=%u\n",
                     entry->dataSize, entry->data[0], entry->data[1], entry->data[2], entry->data[3],
                     parsed, chans, rate, bits, audioLen);
    if (!parsed) return -1;
    if (bits != 16 || chans < 1 || chans > 2 || audioData == nullptr) { if (dbg) logInfo("  skip: fmt\n"); return -1; }
    if (audioLen == 0 || audioLen > MAX_SFX_BYTES) { if (dbg) logInfo("  skip: len %u\n", audioLen); return -1; }

    uint32_t frame = chans * 2;
    uint32_t len = audioLen - (audioLen % frame);
    if (len == 0) return -1;

    uint32_t allocSize = (len + 31) & ~31u;
    uint8_t* buf = (uint8_t*) memalign(32, allocSize);
    if (buf == nullptr) return -1;
    memset(buf, 0, allocSize);
    // WAV is little-endian; the DSP wants native big-endian, so swap each 16-bit sample (byte-wise: source may be unaligned).
    for (uint32_t i = 0; i + 1 < len; i += 2) {
        buf[i] = audioData[i + 1];
        buf[i + 1] = audioData[i];
    }
    DCFlushRange(buf, allocSize);

    WiiVoice* v = findFree(a);
    if (v == nullptr) { free(buf); return -1; }

    v->buf = buf;
    v->playing = true;
    v->loop = loop;
    v->soundIndex = soundIndex;
    v->instanceId = a->nextInstanceId++;
    v->baseFreq = rate;
    if (loop) {
        v->freeAt = 0;
    } else {
        uint32_t frames = len / frame;
        uint32_t ms = (uint32_t) (((uint64_t) frames * 1000) / rate) + TAIL_MARGIN_MS;
        v->freeAt = gettime() + millisecs_to_ticks(ms);
    }

    u32 fmt = (chans == 2) ? VOICE_STEREO16 : VOICE_MONO16;
    AESND_PlayVoice(v->pb, fmt, buf, len, (f32) rate, 0, loop);
    AESND_SetVoiceStop(v->pb, false);   // ensure a reused/previously-stopped voice actually runs
    if (dbg) logInfo("  PLAY ch=%u rate=%u len=%u voice=%d\n", chans, rate, len, (int) (v - a->voices));
    return v->instanceId;
}

static void wiiStopSound(AudioSystem* audio, int32_t idOrSound) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    for (int i = 0; i < WII_VOICES; i++) {
        WiiVoice* v = &a->voices[i];
        if (v->playing && (v->instanceId == idOrSound || v->soundIndex == idOrSound)) {
            AESND_SetVoiceStop(v->pb, true);
            releaseBuf(v);
            v->playing = false;
            v->loop = false;
            v->instanceId = -1;
            v->soundIndex = -1;
            v->freeAt = 0;
        }
    }
}

static bool wiiIsPlaying(AudioSystem* audio, int32_t idOrSound) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    reap(a);
    WiiVoice* v = findVoice(a, idOrSound);
    if (v == nullptr) return false;
    return v->loop || gettime() < v->freeAt;
}

// Per-sound pause isn't distinguishable from stop on AESND; game-wide pause uses AESND_Pause below.
static void wiiPauseSound(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t idOrSound) {}
static void wiiResumeSound(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t idOrSound) {}
static void wiiPauseAll(MAYBE_UNUSED AudioSystem* audio) { AESND_Pause(true); }
static void wiiResumeAll(MAYBE_UNUSED AudioSystem* audio) { AESND_Pause(false); }
static void wiiSuspend(MAYBE_UNUSED AudioSystem* audio) { AESND_Pause(true); }
static void wiiResume(MAYBE_UNUSED AudioSystem* audio) { AESND_Pause(false); }

// Volume/fades deferred (AESND volume scale unverified on hardware); sounds play at AESND's default unity.
static void wiiSetSoundGain(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t idOrSound, MAYBE_UNUSED float gain, MAYBE_UNUSED uint32_t timeMs) {}
static float wiiGetSoundGain(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t idOrSound) { return 1.0f; }

static void wiiSetSoundPitch(AudioSystem* audio, int32_t idOrSound, float pitch) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    WiiVoice* v = findVoice(a, idOrSound);
    if (v != nullptr && v->pb != nullptr && pitch > 0.0f)
        AESND_SetVoiceFrequency(v->pb, (f32) v->baseFreq * pitch);
}
static float wiiGetSoundPitch(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t idOrSound) { return 1.0f; }

static float wiiGetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t idOrSound) { return 0.0f; }
static void wiiSetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t idOrSound, MAYBE_UNUSED float pos) {}
static float wiiGetSoundLength(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t idOrSound) { return 0.0f; }

static void wiiSetMasterGain(AudioSystem* audio, float gain) { ((WiiAudioSystem*) audio)->masterGain = gain; }
static void wiiSetMasterGainForListener(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED float gain, MAYBE_UNUSED int32_t listenerId) {}
static void wiiSetChannelCount(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t count) {}
static void wiiGroupLoad(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) {}
static bool wiiGroupIsLoaded(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) { return true; }
static int32_t wiiCreateStream(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED const char* filename) { return -1; }
static bool wiiDestroyStream(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t streamIndex) { return false; }

AudioSystem* WiiAudioSystem_create(DataWin* dataWin) {
    WiiAudioSystem* a = (WiiAudioSystem*) calloc(1, sizeof(WiiAudioSystem));
    if (a == nullptr) return nullptr;

    wiiAudioVtable.init = wiiInit;
    wiiAudioVtable.destroy = wiiDestroy;
    wiiAudioVtable.update = wiiUpdate;
    wiiAudioVtable.playSound = wiiPlaySound;
    wiiAudioVtable.stopSound = wiiStopSound;
    wiiAudioVtable.stopAll = wiiStopAll;
    wiiAudioVtable.isPlaying = wiiIsPlaying;
    wiiAudioVtable.pauseSound = wiiPauseSound;
    wiiAudioVtable.resumeSound = wiiResumeSound;
    wiiAudioVtable.pauseAll = wiiPauseAll;
    wiiAudioVtable.resumeAll = wiiResumeAll;
    wiiAudioVtable.suspend = wiiSuspend;
    wiiAudioVtable.resume = wiiResume;
    wiiAudioVtable.setSoundGain = wiiSetSoundGain;
    wiiAudioVtable.getSoundGain = wiiGetSoundGain;
    wiiAudioVtable.setSoundPitch = wiiSetSoundPitch;
    wiiAudioVtable.getSoundPitch = wiiGetSoundPitch;
    wiiAudioVtable.getTrackPosition = wiiGetTrackPosition;
    wiiAudioVtable.setTrackPosition = wiiSetTrackPosition;
    wiiAudioVtable.getSoundLength = wiiGetSoundLength;
    wiiAudioVtable.setMasterGain = wiiSetMasterGain;
    wiiAudioVtable.setMasterGainForListener = wiiSetMasterGainForListener;
    wiiAudioVtable.setChannelCount = wiiSetChannelCount;
    wiiAudioVtable.groupLoad = wiiGroupLoad;
    wiiAudioVtable.groupIsLoaded = wiiGroupIsLoaded;
    wiiAudioVtable.createStream = wiiCreateStream;
    wiiAudioVtable.destroyStream = wiiDestroyStream;

    a->base.vtable = &wiiAudioVtable;
    a->masterGain = 1.0f;
    a->nextInstanceId = INSTANCE_ID_BASE;
    return (AudioSystem*) a;
}
