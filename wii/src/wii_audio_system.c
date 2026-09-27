#include "wii_audio_system.h"

#include "common.h"
#include "data_win.h"
#include "file_system.h"
#include "log.h"

#include <aesndlib.h>
#include <gccore.h>
#include <malloc.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// stb_vorbis leaks a few short macros; keep it last and clean up after it.
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#undef L
#undef C
#undef R

#define WII_VOICES 24                     // 16 sounding at once, plus stopped ones still draining
#define WII_STREAMS 4
#define TAIL_MARGIN_MS 120                // keep a finished one-shot's buffer alive so the DSP can drain it
#define MAX_SFX_BYTES (2 * 1024 * 1024)   // embedded SFX are short; refuse anything larger (music is external OGG)
#define INSTANCE_ID_BASE 100000           // keep instance ids clear of small SOND indices

// AESND zero-pads any chunk that is not a whole number of DSP fills, so chunks are a multiple of 1152 bytes.
#define STREAM_CHUNK_BYTES (DSP_STREAMBUFFER_SIZE * 4)
#define STREAM_RING_CHUNKS 40             // ~1 s of 44.1 kHz stereo: covers a long frame without starving
#define STREAM_PRIME_CHUNKS 8
#define STREAM_FILL_PER_UPDATE 4
#define VORBIS_ALLOC_BYTES (192 * 1024)   // AM2R's encodes need ~185 KB of stb_vorbis setup+temp memory
#define STREAM_STATS_FRAMES 1800
// Top the rings up from a worker thread: a long main-thread frame (a room load) otherwise
// starves them, audibly cutting the music out right after a track starts.
#ifndef WII_AUDIO_DECODE_THREAD
#define WII_AUDIO_DECODE_THREAD 1
#endif
#define DECODE_STACK_BYTES (64 * 1024)
#define DECODE_THREAD_PRIO 80
#define DECODE_NAP_US 4000
#define BLOB_CACHE_BYTES (6u << 20)       // raw WAVs kept for replay; a playing voice owns its own swapped copy
#define BLOB_BIG_BYTES (256u << 10)       // ambience-sized: cheap to re-read once, expensive to keep resident
#define DRAIN_UPDATES 3                   // updates a stopped voice must stay stopped before its buffer is freed

// libaesnd copies each voice back from its working copy after the DSP mixes it, flags included, so a stop, start or
// volume set that lands mid-mix is undone. The head of libogc 3.1.0's struct aesndpb_t, to see which of ours survived.
typedef struct {
    u32 out_buf, buf_start, buf_end, buf_curr;
    u16 yn1, yn2, pds, freq_h, freq_l, counter;
    s16 left, right;
    u16 volume_l, volume_r;
    u32 delay;
    u32 flags;
} PbHead;
#define PB_STOPPED 0x00200000

typedef struct {
    float gain, target, from;
    float total, remaining;   // seconds
} Fade;

typedef struct {
    AESNDPB* pb;
    void* buf;              // 32-byte aligned, byte-swapped PCM; owned
    bool playing;
    bool loop;
    bool paused;
    int32_t instanceId;
    int32_t soundIndex;
    uint32_t baseFreq;      // source sample rate, for pitch scaling
    Fade fade;
    uint64_t freeAt;        // gettime() tick at which a one-shot's buffer may be freed; 0 while looping
    uint64_t pausedAt;
    int draining;           // updates left before a stopped voice's buffer is freed and the voice reused
} WiiVoice;

typedef struct {
    AESNDPB* pb;
    stb_vorbis* vorbis;
    char* vorbisMem;                // stb_vorbis_alloc arena, so decoding never touches malloc/alloca
    uint8_t* ring;                  // STREAM_RING_CHUNKS * STREAM_CHUNK_BYTES, 32-byte aligned
    volatile uint32_t readChunk;    // advanced by the DSP callback
    volatile uint32_t writeChunk;   // advanced by the decoder
    volatile uint32_t played;       // chunks handed to the DSP
    volatile uint32_t starved;      // callbacks that had to serve silence mid-track; never reset, so reuse can't hide it
    uint32_t starvedLogged;
    volatile uint32_t drained;      // callbacks after EOF with nothing left
    bool active;
    bool loop;
    bool paused;
    bool eof;
    int channels;
    uint32_t rate;
    int32_t instanceId;
    int32_t soundIndex;
    Fade fade;
    float lengthSeconds;            // < 0 until asked for
} WiiStream;

typedef struct {
    AudioSystem base;
    DataWin* dw;
    FileSystem* fs;
    WiiVoice voices[WII_VOICES];
    WiiStream streams[WII_STREAMS];
    int32_t nextInstanceId;
    float masterGain;
    uint32_t statFrames;
    uint64_t statTicks, statMaxTicks;
    uint32_t* blobUsed;     // per AUDO entry: tick of last play, for LRU
    uint32_t blobTick;
    uint32_t blobBytes;
    int32_t stoppedThisStep[WII_VOICES]; // SOND indices whose effect voices were stopped since the last update
    int stoppedCount;
    uint32_t undoneStops, undoneStarts, undoneLogged;
} WiiAudioSystem;

static AudioSystemVtable wiiAudioVtable;
static uint8_t silence[STREAM_CHUNK_BYTES] ATTRIBUTE_ALIGN(32);

// ===[ Fades ]===

static void fadeSet(Fade* f, float gain, uint32_t ms) {
    if (ms == 0) {
        f->gain = f->target = gain;
        f->remaining = 0.0f;
        return;
    }
    f->from = f->gain;
    f->target = gain;
    f->total = (float) ms / 1000.0f;
    f->remaining = f->total;
}

static bool fadeStep(Fade* f, float dt) {
    if (f->remaining <= 0.0f) return false;
    f->remaining -= dt;
    if (f->remaining <= 0.0f) {
        f->remaining = 0.0f;
        f->gain = f->target;
    } else {
        f->gain = f->from + (f->target - f->from) * (1.0f - f->remaining / f->total);
    }
    return true;
}

// libaesnd mixes (sample * volume) >> 8, so 256 == unity.
static void setVoiceVolume(AESNDPB* pb, float g) {
    if (pb == nullptr) return;
    if (g < 0.0f) g = 0.0f;
    int vol = (int) (g * 256.0f + 0.5f);
    if (vol > 65535) vol = 65535;
    AESND_SetVoiceVolume(pb, (u16) vol, (u16) vol);
}

// ===[ Embedded SFX ]===

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

// Lazy AUDO loading never frees, so without this every sound ever heard stays in RAM.
static void blobTrim(WiiAudioSystem* a, uint32_t keep) {
    Audo* audo = &a->dw->audo;
    for (;;) {
        uint32_t victim = UINT32_MAX, oldest = UINT32_MAX;
        bool over = a->blobBytes > BLOB_CACHE_BYTES;
        for (uint32_t i = 0; i < audo->count; i++) {
            if (audo->entries[i].data == nullptr || i == keep) continue;
            if (audo->entries[i].dataSize > BLOB_BIG_BYTES) { victim = i; break; }
            if (over && a->blobUsed[i] < oldest) { oldest = a->blobUsed[i]; victim = i; }
        }
        if (victim == UINT32_MAX) return;
        free(audo->entries[victim].data);
        audo->entries[victim].data = nullptr;
        a->blobBytes -= audo->entries[victim].dataSize;
    }
}

static void releaseBuf(WiiVoice* v) {
    if (v->buf != nullptr) { free(v->buf); v->buf = nullptr; }
}

static bool pbStopped(AESNDPB* pb) {
    return (((volatile PbHead*) pb)->flags & PB_STOPPED) != 0;
}

// The buffer outlives the stop until the voice has stayed stopped for DRAIN_UPDATES updates (see PbHead).
static void voiceStop(WiiVoice* v) {
    if (v->pb != nullptr) AESND_SetVoiceStop(v->pb, true);
    if (v->buf != nullptr) v->draining = DRAIN_UPDATES;
    v->playing = false;
    v->loop = false;
    v->paused = false;
    v->instanceId = -1;
    v->soundIndex = -1;
    v->freeAt = 0;
}

static void reap(WiiAudioSystem* a) {
    uint64_t now = gettime();
    for (int i = 0; i < WII_VOICES; i++) {
        WiiVoice* v = &a->voices[i];
        if (v->playing && !v->loop && !v->paused && v->freeAt != 0 && now >= v->freeAt) voiceStop(v);
    }
}

static WiiVoice* findFree(WiiAudioSystem* a) {
    for (int i = 0; i < WII_VOICES; i++)
        if (!a->voices[i].playing && a->voices[i].draining == 0) return &a->voices[i];
    return nullptr;
}

static void applyVolume(WiiAudioSystem* a, WiiVoice* v);

// Put back whatever the DSP's copy-back undid: stopped and paused voices stay stopped, loops keep running.
static void settleVoices(WiiAudioSystem* a) {
    for (int i = 0; i < WII_VOICES; i++) {
        WiiVoice* v = &a->voices[i];
        if (v->pb == nullptr) continue;
        bool stopped = pbStopped(v->pb);
        if ((v->draining > 0 || (v->playing && v->paused)) && !stopped) {
            AESND_SetVoiceStop(v->pb, true);
            a->undoneStops++;
            if (v->draining > 0) v->draining = DRAIN_UPDATES;
        } else if (v->playing && v->loop && !v->paused && stopped) {
            AESND_SetVoiceStop(v->pb, false);
            a->undoneStarts++;
        }
        if (v->draining > 0 && --v->draining == 0) releaseBuf(v);
        if (v->playing) applyVolume(a, v);
    }
}

static bool voiceMatches(WiiVoice* v, int32_t idOrSound) {
    return v->playing && (v->instanceId == idOrSound || v->soundIndex == idOrSound);
}

// GML passes either a returned instance id or a SOND index; match both.
static WiiVoice* findVoice(WiiAudioSystem* a, int32_t idOrSound) {
    for (int i = 0; i < WII_VOICES; i++)
        if (voiceMatches(&a->voices[i], idOrSound)) return &a->voices[i];
    return nullptr;
}

static void applyVolume(WiiAudioSystem* a, WiiVoice* v) {
    setVoiceVolume(v->pb, a->masterGain * v->fade.gain);
}

// ===[ OGG streams ]===

// DSP interrupt context: pointer swaps only, no decoding or logging.
static void streamVoiceCallback(AESNDPB* pb, u32 state, void* arg) {
    WiiStream* s = (WiiStream*) arg;
    if (state != VOICE_STATE_STREAM) return;
    if (s->writeChunk != s->readChunk) {
        AESND_SetVoiceBuffer(pb, s->ring + (s->readChunk % STREAM_RING_CHUNKS) * STREAM_CHUNK_BYTES, STREAM_CHUNK_BYTES);
        s->readChunk++;
        s->played++;
    } else {
        AESND_SetVoiceBuffer(pb, silence, STREAM_CHUNK_BYTES);
        if (s->eof) s->drained++; else s->starved++;
    }
}

static bool streamMatches(WiiStream* s, int32_t idOrSound) {
    return s->active && (s->instanceId == idOrSound || s->soundIndex == idOrSound);
}

static WiiStream* findStream(WiiAudioSystem* a, int32_t idOrSound) {
    for (int i = 0; i < WII_STREAMS; i++)
        if (streamMatches(&a->streams[i], idOrSound)) return &a->streams[i];
    return nullptr;
}

static void applyStreamVolume(WiiAudioSystem* a, WiiStream* s) {
    setVoiceVolume(s->pb, a->masterGain * s->fade.gain);
}

#if WII_AUDIO_DECODE_THREAD
static mutex_t g_decodeMutex;   // recursive: streamPlay can evict (streamStop) while holding it
static lwp_t g_decodeThread = LWP_THREAD_NULL;
static volatile bool g_decodeQuit;
#define DECODE_LOCK()   LWP_MutexLock(g_decodeMutex)
#define DECODE_UNLOCK() LWP_MutexUnlock(g_decodeMutex)
#else
#define DECODE_LOCK()   ((void) 0)
#define DECODE_UNLOCK() ((void) 0)
#endif

static void streamStop(WiiStream* s) {
    if (s->pb != nullptr) AESND_SetVoiceStop(s->pb, true);
    DECODE_LOCK();
    s->active = false;
    if (s->vorbis != nullptr) { stb_vorbis_close(s->vorbis); s->vorbis = nullptr; }   // also closes the FILE
    DECODE_UNLOCK();
    s->paused = false;
    s->instanceId = -1;
    s->soundIndex = -1;
}

// AM2R fades old tracks to 0 and leaves them running; reclaim one of those when slots run out.
static WiiStream* findFreeStream(WiiAudioSystem* a) {
    for (int i = 0; i < WII_STREAMS; i++)
        if (a->streams[i].pb != nullptr && !a->streams[i].active) return &a->streams[i];
    for (int i = 0; i < WII_STREAMS; i++) {
        WiiStream* s = &a->streams[i];
        if (s->pb != nullptr && s->fade.gain <= 0.0f && s->fade.remaining <= 0.0f) {
            logInfo("WiiAudio: evicting silent stream snd %d\n", s->soundIndex);
            streamStop(s);
            return s;
        }
    }
    return nullptr;
}

static void streamDecodeChunk(WiiStream* s) {
    int16_t* dst = (int16_t*) (s->ring + (s->writeChunk % STREAM_RING_CHUNKS) * STREAM_CHUNK_BYTES);
    int want = STREAM_CHUNK_BYTES / (s->channels * 2);
    int got = 0;
    bool wrapped = false;
    while (got < want) {
        int n = stb_vorbis_get_samples_short_interleaved(s->vorbis, s->channels, dst + got * s->channels,
                                                         (want - got) * s->channels);
        if (n > 0) { got += n; continue; }
        if (s->loop && !wrapped) { stb_vorbis_seek_start(s->vorbis); wrapped = true; continue; }
        memset(dst + got * s->channels, 0, (size_t) (want - got) * s->channels * 2);
        s->eof = true;
        break;
    }
    s->writeChunk++;
}

static void streamFill(WiiStream* s, int maxChunks) {
    // Leave one slot for the chunk the DSP is still reading (readChunk-1); filling all
    // STREAM_RING_CHUNKS would overwrite it mid-DMA, splicing garbage into the output.
    while (maxChunks-- > 0 && !s->eof && (s->writeChunk - s->readChunk) < STREAM_RING_CHUNKS - 1)
        streamDecodeChunk(s);
}

static char* resolveOggPath(WiiAudioSystem* a, Sound* sound) {
    if (a->fs == nullptr || sound->file == nullptr || sound->file[0] == '\0') return nullptr;
    char name[256];
    snprintf(name, sizeof(name), strchr(sound->file, '.') ? "%s" : "%s.ogg", sound->file);
    return a->fs->vtable->resolvePath(a->fs, name);
}

static int32_t streamPlayLocked(WiiAudioSystem* a, int32_t soundIndex, Sound* sound, bool loop, const char* path);

static int32_t streamPlay(WiiAudioSystem* a, int32_t soundIndex, Sound* sound, bool loop) {
    char* path = resolveOggPath(a, sound);
    if (path == nullptr) { logWarn("WiiAudio: no path for '%s'\n", sound->name); return -1; }

    DECODE_LOCK();
    int32_t id = streamPlayLocked(a, soundIndex, sound, loop, path);
    DECODE_UNLOCK();
    free(path);
    return id;
}

static int32_t streamPlayLocked(WiiAudioSystem* a, int32_t soundIndex, Sound* sound, bool loop, const char* path) {
    WiiStream* s = findFreeStream(a);
    if (s == nullptr) { logWarn("WiiAudio: no stream slot for '%s'\n", sound->name); return -1; }

    FILE* f = fopen(path, "rb");
    if (f == nullptr) { logWarn("WiiAudio: cannot open %s\n", path); return -1; }
    setvbuf(f, nullptr, _IOFBF, 16384);   // fewer SD hits per packet

    int err = 0;
    stb_vorbis_alloc alloc = { s->vorbisMem, VORBIS_ALLOC_BYTES };
    s->vorbis = stb_vorbis_open_file(f, 1, &err, &alloc);   // closes f itself on failure
    if (s->vorbis == nullptr) { logWarn("WiiAudio: stb_vorbis err %d on %s\n", err, path); return -1; }

    stb_vorbis_info info = stb_vorbis_get_info(s->vorbis);
    if (info.channels < 1 || info.channels > 2 || info.sample_rate == 0) {
        logWarn("WiiAudio: unsupported ogg ch=%d rate=%u %s\n", info.channels, info.sample_rate, path);
        stb_vorbis_close(s->vorbis);
        s->vorbis = nullptr;
        return -1;
    }
    s->channels = info.channels;
    s->rate = info.sample_rate;
    s->loop = loop;
    s->paused = false;
    s->eof = false;
    s->readChunk = s->writeChunk = 0;
    s->played = s->drained = 0;
    s->lengthSeconds = -1.0f;
    s->soundIndex = soundIndex;
    s->instanceId = a->nextInstanceId++;
    fadeSet(&s->fade, sound->volume, 0);

    uint64_t t0 = gettime();
    streamFill(s, STREAM_PRIME_CHUNKS);
    s->readChunk = 1;   // chunk 0 goes straight to the voice; the callback continues from chunk 1
    s->played = 1;
    s->active = true;
    AESND_PlayVoice(s->pb, (s->channels == 2) ? VOICE_STEREO16 : VOICE_MONO16,
                    s->ring, STREAM_CHUNK_BYTES, (f32) s->rate, 0, true);
    applyStreamVolume(a, s);
    logInfo("WiiAudio: stream snd %d '%s' ch=%d rate=%u loop=%d slot=%d prime=%u ms\n",
            soundIndex, sound->name, s->channels, s->rate, loop, (int) (s - a->streams),
            (unsigned) ticks_to_millisecs(gettime() - t0));
    return s->instanceId;
}

static float streamPosition(WiiStream* s) {
    uint32_t framesPerChunk = STREAM_CHUNK_BYTES / (s->channels * 2);
    return (float) s->played * (float) framesPerChunk / (float) s->rate;
}

static void streamsUpdate(WiiAudioSystem* a, float dt) {
#if !WII_AUDIO_DECODE_THREAD
    uint64_t t0 = gettime();
#endif
    for (int i = 0; i < WII_STREAMS; i++) {
        WiiStream* s = &a->streams[i];
        if (s->pb == nullptr) continue;
        bool stopped = pbStopped(s->pb);
        if ((!s->active || s->paused) && !stopped) {
            AESND_SetVoiceStop(s->pb, true);
            a->undoneStops++;
        } else if (s->active && !s->paused && stopped) {
            AESND_SetVoiceStop(s->pb, false);
            a->undoneStarts++;
        }
        if (!s->active) continue;
        fadeStep(&s->fade, dt);
        applyStreamVolume(a, s);
        if (s->starved != s->starvedLogged) {
            uint32_t n = s->starved - s->starvedLogged;
            s->starvedLogged = s->starved;
            logWarn("WiiAudio: stream snd %d starved %u chunks (~%u ms of silence)\n", s->soundIndex, (unsigned) n,
                    (unsigned) (n * (STREAM_CHUNK_BYTES / (s->channels * 2)) * 1000 / s->rate));
        }
        if (s->paused) continue;
#if !WII_AUDIO_DECODE_THREAD
        streamFill(s, STREAM_FILL_PER_UPDATE);
#endif
        if (s->eof && s->drained > 0) {
            logInfo("WiiAudio: stream snd %d finished\n", s->soundIndex);
            streamStop(s);
        }
    }
#if !WII_AUDIO_DECODE_THREAD
    uint64_t dtTicks = gettime() - t0;
    a->statTicks += dtTicks;
    if (dtTicks > a->statMaxTicks) a->statMaxTicks = dtTicks;
#endif
    if (++a->statFrames >= STREAM_STATS_FRAMES) {
        uint32_t starved = 0;
        int active = 0;
        for (int i = 0; i < WII_STREAMS; i++) { starved += a->streams[i].starved; active += a->streams[i].active; }
#if WII_AUDIO_DECODE_THREAD
        // The worker does the decoding, so a main-thread timing here would only measure the fade loop.
        if (active > 0) logInfo("WiiAudio: %d streams, starved=%u\n", active, starved);
#else
        if (active > 0)
            logInfo("WiiAudio: decode avg %u us/frame max %u us, %d streams, starved=%u\n",
                    (unsigned) (ticks_to_microsecs(a->statTicks) / STREAM_STATS_FRAMES),
                    (unsigned) ticks_to_microsecs(a->statMaxTicks), active, starved);
#endif
        if (a->undoneStops + a->undoneStarts != a->undoneLogged) {
            a->undoneLogged = a->undoneStops + a->undoneStarts;
            logInfo("WiiAudio: redid %u stops and %u starts the DSP copy-back undid\n", a->undoneStops, a->undoneStarts);
        }
        a->statFrames = 0;
        a->statTicks = a->statMaxTicks = 0;
    }
}

#if WII_AUDIO_DECODE_THREAD
static void* decodeThreadMain(void* arg) {
    WiiAudioSystem* a = (WiiAudioSystem*) arg;
    while (!g_decodeQuit) {
        DECODE_LOCK();
        for (int i = 0; i < WII_STREAMS; i++) {
            WiiStream* s = &a->streams[i];
            if (s->active && !s->paused) streamFill(s, STREAM_FILL_PER_UPDATE);
        }
        DECODE_UNLOCK();
        usleep(DECODE_NAP_US);
    }
    return nullptr;
}
#endif

// ===[ Vtable ]===

static void wiiInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fs) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    a->dw = dataWin;
    a->fs = fs;
    a->base.dw = dataWin;
    a->blobUsed = (uint32_t*) calloc(dataWin->audo.count ? dataWin->audo.count : 1, sizeof(uint32_t));
    AESND_Init();
    AESND_Pause(false);
    for (int i = 0; i < WII_VOICES; i++) {
        a->voices[i].pb = AESND_AllocateVoice(nullptr);   // never stopped at allocation; wiiPlaySound un-stops a voice on reuse
        a->voices[i].instanceId = -1;
        a->voices[i].soundIndex = -1;
    }
    int streams = 0;
    for (int i = 0; i < WII_STREAMS; i++) {
        WiiStream* s = &a->streams[i];
        s->ring = (uint8_t*) memalign(32, STREAM_RING_CHUNKS * STREAM_CHUNK_BYTES);
        s->vorbisMem = (char*) malloc(VORBIS_ALLOC_BYTES);
        if (s->ring == nullptr || s->vorbisMem == nullptr) break;
        s->pb = AESND_AllocateVoiceWithArg(streamVoiceCallback, s);
        if (s->pb == nullptr) break;
        AESND_SetVoiceStream(s->pb, true);
        s->instanceId = -1;
        s->soundIndex = -1;
        streams++;
    }
#if WII_AUDIO_DECODE_THREAD
    LWP_MutexInit(&g_decodeMutex, true);
    g_decodeQuit = false;
    LWP_CreateThread(&g_decodeThread, decodeThreadMain, a, nullptr, DECODE_STACK_BYTES, DECODE_THREAD_PRIO);
#endif
    logInfo("WiiAudio: AESND up, %d voices, %d streams%s\n", WII_VOICES, streams,
            WII_AUDIO_DECODE_THREAD ? ", decode thread" : "");
}

static void wiiStopAll(AudioSystem* audio) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    for (int i = 0; i < WII_VOICES; i++) voiceStop(&a->voices[i]);
    for (int i = 0; i < WII_STREAMS; i++) streamStop(&a->streams[i]);
}

static void wiiDestroy(AudioSystem* audio) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
#if WII_AUDIO_DECODE_THREAD
    g_decodeQuit = true;
    if (g_decodeThread != LWP_THREAD_NULL) LWP_JoinThread(g_decodeThread, nullptr);
    g_decodeThread = LWP_THREAD_NULL;
#endif
    wiiStopAll(audio);
    for (int i = 0; i < WII_VOICES; i++) releaseBuf(&a->voices[i]);
#if WII_AUDIO_DECODE_THREAD
    LWP_MutexDestroy(g_decodeMutex);
#endif
    for (int i = 0; i < WII_VOICES; i++)
        if (a->voices[i].pb != nullptr) AESND_FreeVoice(a->voices[i].pb);
    for (int i = 0; i < WII_STREAMS; i++) {
        if (a->streams[i].pb != nullptr) AESND_FreeVoice(a->streams[i].pb);
        free(a->streams[i].ring);
        free(a->streams[i].vorbisMem);
    }
    free(a->blobUsed);
    free(a);
}

static void wiiUpdate(AudioSystem* audio, float dt) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    a->stoppedCount = 0;
    reap(a);
    for (int i = 0; i < WII_VOICES; i++) {
        WiiVoice* v = &a->voices[i];
        if (v->playing) fadeStep(&v->fade, dt);
    }
    settleVoices(a);
    streamsUpdate(a, dt);
}

static int32_t wiiPlaySound(AudioSystem* audio, int32_t soundIndex, MAYBE_UNUSED int32_t priority, bool loop) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    DataWin* dw = a->dw;

    reap(a);
    if (dw == nullptr || soundIndex < 0 || (uint32_t) soundIndex >= dw->sond.count) return -1;
    Sound* sound = &dw->sond.sounds[soundIndex];

    bool embedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0
                 || (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0
                 || (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) != AUDIO_ENTRY_FLAG_REGULAR;
    if (!embedded) return streamPlay(a, soundIndex, sound, loop);
    if (sound->audioGroup != 0) return -1;

    DataWin* group = dw;        // group 0 is data.win; AM2R keeps its SFX there
    if (sound->audioFile < 0 || (uint32_t) sound->audioFile >= group->audo.count) return -1;
    AudioEntry* entry = &group->audo.entries[sound->audioFile];
    bool fresh = entry->data == nullptr;
    DataWin_loadAudoIfNeeded(group, (uint32_t) sound->audioFile);
    if (a->blobUsed != nullptr) {
        if (fresh && entry->data != nullptr) a->blobBytes += entry->dataSize;
        a->blobUsed[sound->audioFile] = ++a->blobTick;
        blobTrim(a, (uint32_t) sound->audioFile);
    }
    if (entry->data == nullptr || entry->dataSize == 0) return -1;

    uint32_t chans = 0, rate = 0, bits = 0, audioLen = 0;
    const uint8_t* audioData = nullptr;
    bool parsed = parseWavHeader(entry->data, entry->dataSize, &chans, &rate, &bits, &audioData, &audioLen);
    if (!parsed) return -1;
    if (bits != 16 || chans < 1 || chans > 2 || audioData == nullptr) return -1;
    if (audioLen == 0 || audioLen > MAX_SFX_BYTES) return -1;

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
    v->paused = false;
    v->soundIndex = soundIndex;
    v->instanceId = a->nextInstanceId++;
    v->baseFreq = rate;
    fadeSet(&v->fade, 1.0f, 0);
    if (loop) {
        v->freeAt = 0;
    } else {
        uint32_t frames = len / frame;
        uint32_t ms = (uint32_t) (((uint64_t) frames * 1000) / rate) + TAIL_MARGIN_MS;
        v->freeAt = gettime() + millisecs_to_ticks(ms);
    }

    u32 fmt = (chans == 2) ? VOICE_STEREO16 : VOICE_MONO16;
    AESND_PlayVoice(v->pb, fmt, buf, len, (f32) rate, 0, loop);
    applyVolume(a, v);
    AESND_SetVoiceStop(v->pb, false);   // ensure a reused/previously-stopped voice actually runs
    return v->instanceId;
}

static void wiiStopSound(AudioSystem* audio, int32_t idOrSound) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    for (int i = 0; i < WII_VOICES; i++) {
        if (!voiceMatches(&a->voices[i], idOrSound)) continue;
        if (idOrSound < INSTANCE_ID_BASE && a->stoppedCount < WII_VOICES) a->stoppedThisStep[a->stoppedCount++] = idOrSound;
        voiceStop(&a->voices[i]);
    }
    for (int i = 0; i < WII_STREAMS; i++)
        if (streamMatches(&a->streams[i], idOrSound)) streamStop(&a->streams[i]);
}

// GM reports paused sounds as still playing.
static bool wiiIsPlaying(AudioSystem* audio, int32_t idOrSound) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    reap(a);
    if (findStream(a, idOrSound) != nullptr) return true;
    // A stop lands at the end of the step: AM2R stops Samus's spin loop on entering water, then asks again the same step;
    // an instant "no" restarted it under the old id, orphaned for as long as she stayed in the water.
    for (int i = 0; i < a->stoppedCount; i++)
        if (a->stoppedThisStep[i] == idOrSound) return true;
    WiiVoice* v = findVoice(a, idOrSound);
    if (v == nullptr) return false;
    return v->loop || v->paused || gettime() < v->freeAt;
}

// Butterscotch derives audio_is_paused from !isPlaying, which can't work with GM's "paused still
// counts as playing". AM2R's mus_resume gates on it, so patch 0010 routes the builtin here.
bool WiiAudio_isPaused(AudioSystem* audio, int32_t idOrSound) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    WiiStream* s = findStream(a, idOrSound);
    if (s != nullptr) return s->paused;
    WiiVoice* v = findVoice(a, idOrSound);
    return v != nullptr && v->paused;
}

static void wiiPauseSound(AudioSystem* audio, int32_t idOrSound) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    for (int i = 0; i < WII_VOICES; i++) {
        WiiVoice* v = &a->voices[i];
        if (voiceMatches(v, idOrSound) && !v->paused) {
            v->paused = true;
            v->pausedAt = gettime();
            AESND_SetVoiceStop(v->pb, true);
        }
    }
    for (int i = 0; i < WII_STREAMS; i++) {
        WiiStream* s = &a->streams[i];
        if (streamMatches(s, idOrSound) && !s->paused) {
            s->paused = true;
            AESND_SetVoiceStop(s->pb, true);
        }
    }
}

static void wiiResumeSound(AudioSystem* audio, int32_t idOrSound) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    for (int i = 0; i < WII_VOICES; i++) {
        WiiVoice* v = &a->voices[i];
        if (voiceMatches(v, idOrSound) && v->paused) {
            v->paused = false;
            if (v->freeAt != 0) v->freeAt += gettime() - v->pausedAt;
            AESND_SetVoiceStop(v->pb, false);
        }
    }
    for (int i = 0; i < WII_STREAMS; i++) {
        WiiStream* s = &a->streams[i];
        if (streamMatches(s, idOrSound) && s->paused) {
            s->paused = false;
            AESND_SetVoiceStop(s->pb, false);
        }
    }
}

static void wiiPauseAll(MAYBE_UNUSED AudioSystem* audio) { AESND_Pause(true); }
static void wiiResumeAll(MAYBE_UNUSED AudioSystem* audio) { AESND_Pause(false); }
static void wiiSuspend(MAYBE_UNUSED AudioSystem* audio) { AESND_Pause(true); }
static void wiiResume(MAYBE_UNUSED AudioSystem* audio) { AESND_Pause(false); }

static void wiiSetSoundGain(AudioSystem* audio, int32_t idOrSound, float gain, uint32_t timeMs) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    for (int i = 0; i < WII_VOICES; i++) {
        WiiVoice* v = &a->voices[i];
        if (voiceMatches(v, idOrSound)) { fadeSet(&v->fade, gain, timeMs); applyVolume(a, v); }
    }
    for (int i = 0; i < WII_STREAMS; i++) {
        WiiStream* s = &a->streams[i];
        if (streamMatches(s, idOrSound)) { fadeSet(&s->fade, gain, timeMs); applyStreamVolume(a, s); }
    }
}
static float wiiGetSoundGain(AudioSystem* audio, int32_t idOrSound) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    WiiStream* s = findStream(a, idOrSound);
    if (s != nullptr) return s->fade.gain;
    WiiVoice* v = findVoice(a, idOrSound);
    return v ? v->fade.gain : 1.0f;
}

static void wiiSetSoundPitch(AudioSystem* audio, int32_t idOrSound, float pitch) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    if (pitch <= 0.0f) return;
    WiiVoice* v = findVoice(a, idOrSound);
    if (v != nullptr && v->pb != nullptr) AESND_SetVoiceFrequency(v->pb, (f32) v->baseFreq * pitch);
    WiiStream* s = findStream(a, idOrSound);
    if (s != nullptr) AESND_SetVoiceFrequency(s->pb, (f32) s->rate * pitch);
}
static float wiiGetSoundPitch(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t idOrSound) { return 1.0f; }

static float wiiGetTrackPosition(AudioSystem* audio, int32_t idOrSound) {
    WiiStream* s = findStream((WiiAudioSystem*) audio, idOrSound);
    return s ? streamPosition(s) : 0.0f;
}

static void wiiSetTrackPosition(AudioSystem* audio, int32_t idOrSound, float pos) {
    WiiStream* s = findStream((WiiAudioSystem*) audio, idOrSound);
    if (s == nullptr || pos < 0.0f) return;
    DECODE_LOCK();
    stb_vorbis_seek(s->vorbis, (unsigned int) (pos * (float) s->rate));
    // Drop what is buffered; the callback may be mid-increment, so do it with interrupts off.
    u32 level;
    _CPU_ISR_Disable(level);
    s->writeChunk = s->readChunk;
    s->eof = false;
    _CPU_ISR_Restore(level);
    s->played = (uint32_t) (pos * (float) s->rate / (float) (STREAM_CHUNK_BYTES / (s->channels * 2)));
    DECODE_UNLOCK();
}

static float wiiGetSoundLength(AudioSystem* audio, int32_t idOrSound) {
    WiiStream* s = findStream((WiiAudioSystem*) audio, idOrSound);
    if (s == nullptr) return 0.0f;
    if (s->lengthSeconds < 0.0f) {
        DECODE_LOCK();
        s->lengthSeconds = stb_vorbis_stream_length_in_seconds(s->vorbis);
        DECODE_UNLOCK();
    }
    return s->lengthSeconds;
}

static void wiiSetMasterGain(AudioSystem* audio, float gain) {
    WiiAudioSystem* a = (WiiAudioSystem*) audio;
    a->masterGain = gain;
    for (int i = 0; i < WII_VOICES; i++)
        if (a->voices[i].playing) applyVolume(a, &a->voices[i]);
    for (int i = 0; i < WII_STREAMS; i++)
        if (a->streams[i].active) applyStreamVolume(a, &a->streams[i]);
}
static void wiiSetMasterGainForListener(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED float gain, MAYBE_UNUSED int32_t listenerId) {}
static void wiiSetChannelCount(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t count) {}
static void wiiGroupLoad(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) {}
static bool wiiGroupIsLoaded(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) { return true; }
// audio_create_stream is unused by AM2R; its music goes through SOND entries above.
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
