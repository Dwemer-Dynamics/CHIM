#include "AudioManager.h"
#include <windows.h>
#include <x3daudio.h>
#include <xaudio2.h>

#include <vector>
#include <chrono>
#include <cstdint>
#include <new>
#include <string>

#pragma comment(lib, "xaudio2.lib")


typedef signed int DWORD_;


typedef struct __WAVEDESCR {
    BYTE riff[4];
    DWORD_ size;
    BYTE wave[4];

} _WAVEDESCR, *_LPWAVEDESCR;

typedef struct __WAVEFORMAT {
    BYTE id[4];
    DWORD_ size;
    SHORT format;
    SHORT channels;
    DWORD_ sampleRate;
    DWORD_ byteRate;
    SHORT blockAlign;
    SHORT bitsPerSample;

} _WAVEFORMAT, *_LPWAVEFORMAT;

typedef struct __DATA_CHUNK {
    BYTE ckID[4];
    DWORD_ ckSize;

} _DATA_CHUNK, *_LPDATA_CHUNK;



namespace logger = SKSE::log;

float currentVolume = 0.0f;
const float rampDuration = 0.05f;  // 50ms ramp
const int rampSteps = 10;
bool isRamping = false;
std::chrono::steady_clock::time_point rampStartTime;
bool isPaused = false;
bool legacyAudioNoattenuation = false;



AudioManager::AudioManager() {
    // Constructor implementation
}

AudioManager::~AudioManager() { 
    Stop();  // This will clean up copiedData and source voice
}




void AudioManager::Stop() {
    std::lock_guard<std::mutex> lock(voiceMtx);
    logger::debug("[AudioManager] Stopping playback");

    if (pSourceVoice) {
        HRESULT hr = pSourceVoice->Stop(0);
        if (SUCCEEDED(hr)) {
            logger::debug("[AudioManager] Successfully stopped source voice");
            pSourceVoice->DestroyVoice();
            pSourceVoice = nullptr;
        } else {
            logger::error("[AudioManager] Failed to stop source voice: {}", hr);
        }
    } else {
        logger::debug("[AudioManager] Stop called with null source voice (idle/already stopped)");
    }

    // Always free the buffer when stopping, regardless of pause state
    // This prevents memory leaks while still allowing pause/resume functionality
    if (copiedData) {
        delete[] copiedData;
        copiedData = nullptr;
        logger::debug("[AudioManager] Freed audio data buffer");
        isPaused = false; // Reset pause state since we're freeing the buffer
    }
}
               
void AudioManager::setDistanceScaler(float cds) {

    emitter.CurveDistanceScaler = cds;
    logger::info("[AudioManager] Set emitter CurveDistanceScaler to {}", cds);

}

               
float AudioManager::getDistanceScaler() {
    return emitter.CurveDistanceScaler;
}

void AudioManager::setLegacyDistanceScaler(float cds) {
    emitter.CurveDistanceScaler = cds;
    logger::info("[AudioManager Legacy] Set emitter CurveDistanceScaler to {}", cds);
}

void AudioManager::setMuffledPlayback(bool enabled)
{
    std::lock_guard<std::mutex> lock(voiceMtx);
    if (!pSourceVoice) {
        return;
    }

    XAUDIO2_FILTER_PARAMETERS filterParams{};
    filterParams.Type = LowPassFilter;
    // Gentle low-pass so blocked speech is only slightly muffled.
    filterParams.Frequency = enabled ? 0.65f : XAUDIO2_MAX_FILTER_FREQUENCY;
    filterParams.OneOverQ = 1.0f;

    const HRESULT hr = pSourceVoice->SetFilterParameters(&filterParams, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        logger::warn("[AudioManager] Failed to set muffled playback filter (enabled={}): {}", enabled ? 1 : 0, hr);
        return;
    }

    logger::info("[AudioManager] SpatialAudioDBG filter {} freq={:.2f}", enabled ? "enabled" : "disabled",
                 filterParams.Frequency);
}

void AudioManager::setSpatialUpdatesEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(voiceMtx);
    spatialUpdatesEnabled = enabled;
    if (enabled) {
        forceSpatialMatrixUpdate = true;
    }
}

void AudioManager::LogDebug() {
    logger::debug("============================================================");
    logger::debug("[AudioManager] DEBUG AUDIO CONFIGURATION");
    logger::debug("============================================================");

    // ------------------------------------------------------------
    // XAudio2
    // ------------------------------------------------------------

    logger::debug("[XAudio2]");

    if (pXAudio2) {
        logger::debug("  pXAudio2       : {}", static_cast<const void*>(pXAudio2));
    } else {
        logger::debug("  pXAudio2       : NULL");
    }

    if (pMasterVoice) {
        XAUDIO2_VOICE_DETAILS details{};
        pMasterVoice->GetVoiceDetails(&details);

        logger::debug("  MasterChannels : {}", details.InputChannels);
        logger::debug("  SampleRate     : {}", details.InputSampleRate);

        DWORD channelMask = 0;
        const HRESULT hr = pMasterVoice->GetChannelMask(&channelMask);

        if (SUCCEEDED(hr)) {
            logger::debug("  ChannelMask    : 0x{:08X}", channelMask);

            logger::debug("  Channel layout :");

            if (channelMask & SPEAKER_FRONT_LEFT) logger::debug("    FRONT_LEFT");

            if (channelMask & SPEAKER_FRONT_RIGHT) logger::debug("    FRONT_RIGHT");

            if (channelMask & SPEAKER_FRONT_CENTER) logger::debug("    FRONT_CENTER");

            if (channelMask & SPEAKER_LOW_FREQUENCY) logger::debug("    LFE");

            if (channelMask & SPEAKER_BACK_LEFT) logger::debug("    BACK_LEFT");

            if (channelMask & SPEAKER_BACK_RIGHT) logger::debug("    BACK_RIGHT");

            if (channelMask & SPEAKER_FRONT_LEFT_OF_CENTER) logger::debug("    FRONT_LEFT_OF_CENTER");

            if (channelMask & SPEAKER_FRONT_RIGHT_OF_CENTER) logger::debug("    FRONT_RIGHT_OF_CENTER");

            if (channelMask & SPEAKER_BACK_CENTER) logger::debug("    BACK_CENTER");

            if (channelMask & SPEAKER_SIDE_LEFT) logger::debug("    SIDE_LEFT");

            if (channelMask & SPEAKER_SIDE_RIGHT) logger::debug("    SIDE_RIGHT");

            if (channelMask & SPEAKER_TOP_CENTER) logger::debug("    TOP_CENTER");

            if (channelMask & SPEAKER_TOP_FRONT_LEFT) logger::debug("    TOP_FRONT_LEFT");

            if (channelMask & SPEAKER_TOP_FRONT_CENTER) logger::debug("    TOP_FRONT_CENTER");

            if (channelMask & SPEAKER_TOP_FRONT_RIGHT) logger::debug("    TOP_FRONT_RIGHT");

            if (channelMask & SPEAKER_TOP_BACK_LEFT) logger::debug("    TOP_BACK_LEFT");

            if (channelMask & SPEAKER_TOP_BACK_CENTER) logger::debug("    TOP_BACK_CENTER");

            if (channelMask & SPEAKER_TOP_BACK_RIGHT) logger::debug("    TOP_BACK_RIGHT");
        } else {
            logger::debug("  GetChannelMask  : FAILED 0x{:08X}", static_cast<unsigned>(hr));
        }
    } else {
        logger::debug("  MasterVoice     : NULL");
    }

    // ------------------------------------------------------------
    // Source voice
    // ------------------------------------------------------------

    logger::debug("[SourceVoice]");

    if (pSourceVoice) {
        XAUDIO2_VOICE_DETAILS details{};
        pSourceVoice->GetVoiceDetails(&details);

        logger::debug("  Channels       : {}", details.InputChannels);
        logger::debug("  SampleRate     : {}", details.InputSampleRate);
        logger::debug("  Voice          : {}", static_cast<const void*>(pSourceVoice));
    } else {
        logger::debug("  Voice          : NULL");
    }

    // ------------------------------------------------------------
    // WAV format
    // ------------------------------------------------------------

    logger::debug("[WAV Format]");

    logger::debug("  FormatTag      : 0x{:04X}", wfx.wFormatTag);
    logger::debug("  Channels       : {}", wfx.nChannels);
    logger::debug("  SampleRate     : {}", wfx.nSamplesPerSec);
    logger::debug("  BitsPerSample  : {}", wfx.wBitsPerSample);
    logger::debug("  BlockAlign     : {}", wfx.nBlockAlign);
    logger::debug("  AvgBytesSec    : {}", wfx.nAvgBytesPerSec);
    logger::debug("  ExtraSize      : {}", wfx.cbSize);

    // ------------------------------------------------------------
    // X3DAudio
    // ------------------------------------------------------------

    logger::debug("[X3DAudio]");

    logger::debug("  SrcChannels    : {}", dspSettings.SrcChannelCount);
    logger::debug("  DstChannels    : {}", dspSettings.DstChannelCount);
    logger::debug("  MatrixPointer  : {}", static_cast<const void*>(dspSettings.pMatrixCoefficients));

    logger::debug("  SpeedOfSound   : {}", X3DAUDIO_SPEED_OF_SOUND);

    // ------------------------------------------------------------
    // Listener
    // ------------------------------------------------------------

    logger::debug("[Listener]");

    logger::debug("  Position       : ({}, {}, {})", listener.Position.x, listener.Position.y, listener.Position.z);

    logger::debug("  OrientFront    : ({}, {}, {})", listener.OrientFront.x, listener.OrientFront.y,
                  listener.OrientFront.z);

    logger::debug("  OrientTop      : ({}, {}, {})", listener.OrientTop.x, listener.OrientTop.y, listener.OrientTop.z);

    // ------------------------------------------------------------
    // Emitter
    // ------------------------------------------------------------

    logger::debug("[Emitter]");

    logger::debug("  Channels       : {}", emitter.ChannelCount);
    logger::debug("  DistanceScaler : {}", emitter.CurveDistanceScaler);
    logger::debug("  InnerRadius    : {}", emitter.InnerRadius);
    logger::debug("  InnerRadiusAng : {}", emitter.InnerRadiusAngle);

    logger::debug("  Position       : ({}, {}, {})", emitter.Position.x, emitter.Position.y, emitter.Position.z);

    logger::debug("  OrientFront    : ({}, {}, {})", emitter.OrientFront.x, emitter.OrientFront.y,
                  emitter.OrientFront.z);

    logger::debug("  OrientTop      : ({}, {}, {})", emitter.OrientTop.x, emitter.OrientTop.y, emitter.OrientTop.z);

    // ------------------------------------------------------------
    // Runtime settings
    // ------------------------------------------------------------

    logger::debug("[Runtime]");

    logger::debug("  DefaultVolume  : {}", defaultVolume.load());
    logger::debug("  CurrentVolume  : {}", currentVolume);
    logger::debug("  DistanceScaler : {}", getDistanceScaler());
    logger::debug("  LegacyNoAtten  : {}", getLegacyAudioNoattenuation());
    logger::debug("  SpatialUpdates : {}", spatialUpdatesEnabled);
    logger::debug("  ForceMatrix    : {}", forceSpatialMatrixUpdate);
    logger::debug("  Ramping        : {}", isRamping);

    // ------------------------------------------------------------
    // DSP matrix contents
    // ------------------------------------------------------------

    if (dspSettings.pMatrixCoefficients && dspSettings.SrcChannelCount > 0 && dspSettings.DstChannelCount > 0) {
        const UINT count = dspSettings.SrcChannelCount * dspSettings.DstChannelCount;

        logger::debug("[DSP Matrix]");
        logger::debug("  Dimensions     : {} x {}", dspSettings.SrcChannelCount, dspSettings.DstChannelCount);

        for (UINT i = 0; i < count; ++i) {
            logger::debug("  Matrix[{}]      : {}", i, dspSettings.pMatrixCoefficients[i]);
        }
    } else {
        logger::debug("[DSP Matrix] INVALID / NOT ALLOCATED");
    }

    logger::debug("============================================================");
    logger::debug("[AudioManager] END DEBUG AUDIO CONFIGURATION");
    logger::debug("============================================================");
}


bool AudioManager::Initialize() {
    logger::debug("[AudioManager] Starting initialization");
    
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        logger::error("[AudioManager] Failed to initialize COM: {}", hr);
        return false;
    }

    hr = XAudio2Create(&pXAudio2);
    if (FAILED(hr)) {
        logger::error("[AudioManager] Failed to create XAudio2 instance: {}", hr);
        return false;
    }

    hr = pXAudio2->CreateMasteringVoice(&pMasterVoice, XAUDIO2_DEFAULT_CHANNELS);
    if (FAILED(hr)) {
        logger::error("[AudioManager] Failed to create mastering voice: {}", hr);
        pXAudio2->Release();
        pXAudio2 = nullptr;
        return false;
    }

    XAUDIO2_VOICE_DETAILS details;
    pMasterVoice->GetVoiceDetails(&details);
    logger::debug("[AudioManager] Mastering voice created with {} input channels", details.InputChannels);

    dspSettings = {};
    FLOAT32* matrix = new FLOAT32[details.InputChannels];
    dspSettings.SrcChannelCount = 1;
    dspSettings.DstChannelCount = details.InputChannels;
    dspSettings.pMatrixCoefficients = matrix;

    DWORD dwChannelMask;
    pMasterVoice->GetChannelMask(&dwChannelMask); 
    
    // Initialize X3DAudio
    X3DAudioInitialize(dwChannelMask, X3DAUDIO_SPEED_OF_SOUND, x3DInstance);
    logger::debug("[AudioManager] X3DAudio initialized with channel mask: {}", dwChannelMask);

    listener = {};
    emitter = {};

    listener.Position = {0.0f, 0.0f, 0.0f};
    listener.Velocity = {0.0f, 0.0f, 0.0f};
    listener.OrientFront = {0, -1.0f, 0.0f};
    listener.OrientTop = {0.0f, 0.0f, 1.0f};

    // Emitter

    emitter.ChannelCount = 1;
    // 8.0 forced MCM slider >100% which clipped XAudio2 → callback deadlock.
    // 2.0 stays audible without amplification. Runtime override: setDistanceScaler.
    emitter.CurveDistanceScaler = 2.0;
    emitter.Position = {0.0f, 0.0f, 0.0f};
    emitter.Velocity = {0.0f, 0.0f, 0.0f};
    emitter.OrientFront = {0.0f, 1.0f, 0.0f};
    emitter.OrientTop = {0.0f, 0.0f, 1.0f};
    emitter.pCone = NULL;

    emitter.InnerRadius = 0.0f;
    emitter.InnerRadiusAngle = X3DAUDIO_PI / 4.0f;

   

    // Other initialization steps such as loading audio data, setting source properties, etc.
    logger::info("[AudioManager] Successfully initialized with {} channels", details.InputChannels);


    LogDebug();  // Log the debug information after initialization

    return true;
}

bool AudioManager::LoadWAV(BYTE* originalbuffer, size_t dataSize) {
    logger::debug("[AudioManager] Starting WAV loading - Data size: {} bytes", dataSize);

    // Atomic-swap: build new voice in locals, briefly take voiceMtx to swap pointers,
    // DestroyVoice the old one OUTSIDE the lock. Prevents Update/LoadWAV race that
    // landed SetVolume on a freshly-destroyed voice → XAudio2 deadlock.

    if (dataSize < sizeof(_WAVEDESCR)) {
        logger::error("[AudioManager] WAV data too small ({} bytes)", dataSize);
        return false;
    }

    BYTE* newCopiedData = new (std::nothrow) BYTE[dataSize];
    if (newCopiedData == nullptr) {
        logger::error("[AudioManager] Failed to allocate memory for audio data");
        return false;
    }

    memcpy(newCopiedData, originalbuffer, dataSize);
    logger::debug("[AudioManager] Audio data copied to internal buffer");

    _WAVEDESCR* waveDescr = (_WAVEDESCR*)newCopiedData;
    if (memcmp(waveDescr->riff, "RIFF", 4) != 0 || memcmp(waveDescr->wave, "WAVE", 4) != 0) {
        logger::error("[AudioManager] Invalid WAV file header");
        delete[] newCopiedData;
        return false;
    }

    std::uint16_t formatTag = WAVE_FORMAT_PCM;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t byteRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t extraSize = 0;
    BYTE* audioData = nullptr;
    size_t audioDataSize = 0;
    bool foundFmtChunk = false;
    bool foundDataChunk = false;

    size_t offset = sizeof(_WAVEDESCR);
    while (offset + sizeof(_DATA_CHUNK) <= dataSize) {
        auto* chunk = reinterpret_cast<_DATA_CHUNK*>(newCopiedData + offset);
        const size_t chunkDataOffset = offset + sizeof(_DATA_CHUNK);
        const size_t chunkSize = static_cast<size_t>(chunk->ckSize);

        if (chunkDataOffset + chunkSize > dataSize) {
            logger::error("[AudioManager] Invalid WAV chunk '{}' size {} exceeds buffer {}",
                         std::string(reinterpret_cast<char*>(chunk->ckID), 4), chunkSize, dataSize);
            delete[] newCopiedData;
            return false;
        }

        if (memcmp(chunk->ckID, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                logger::error("[AudioManager] WAV fmt chunk too small: {}", chunkSize);
                delete[] newCopiedData;
                return false;
            }

            BYTE* fmtData = newCopiedData + chunkDataOffset;
            formatTag = *reinterpret_cast<std::uint16_t*>(fmtData + 0);
            channels = *reinterpret_cast<std::uint16_t*>(fmtData + 2);
            sampleRate = *reinterpret_cast<std::uint32_t*>(fmtData + 4);
            byteRate = *reinterpret_cast<std::uint32_t*>(fmtData + 8);
            blockAlign = *reinterpret_cast<std::uint16_t*>(fmtData + 12);
            bitsPerSample = *reinterpret_cast<std::uint16_t*>(fmtData + 14);
            if (chunkSize >= 18) {
                extraSize = *reinterpret_cast<std::uint16_t*>(fmtData + 16);
            }
            foundFmtChunk = true;
        } else if (memcmp(chunk->ckID, "data", 4) == 0) {
            audioData = newCopiedData + chunkDataOffset;
            audioDataSize = chunkSize;
            foundDataChunk = true;
        }

        offset = chunkDataOffset + chunkSize + (chunkSize % 2);
    }

    if (!foundFmtChunk || !foundDataChunk || !audioData || audioDataSize == 0) {
        logger::error("[AudioManager] Missing WAV fmt/data chunks (fmt={}, data={}, audioSize={})",
                     foundFmtChunk ? 1 : 0, foundDataChunk ? 1 : 0, audioDataSize);
        delete[] newCopiedData;
        return false;
    }

    logger::debug("[AudioManager] WAV format - Tag: {}, Channels: {}, Sample Rate: {}, Bits: {}, Data: {} bytes",
                 formatTag, channels, sampleRate, bitsPerSample, audioDataSize);

    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = static_cast<UINT32>(audioDataSize);
    buffer.pAudioData = audioData;
    buffer.Flags = XAUDIO2_END_OF_STREAM;

    WAVEFORMATEX newWfx = {};
    newWfx.wFormatTag = formatTag;
    newWfx.nChannels = channels;
    newWfx.nSamplesPerSec = sampleRate;
    newWfx.wBitsPerSample = bitsPerSample;
    newWfx.nBlockAlign = blockAlign ? blockAlign : static_cast<WORD>((newWfx.nChannels * newWfx.wBitsPerSample) / 8);
    newWfx.nAvgBytesPerSec = byteRate ? byteRate : (newWfx.nSamplesPerSec * newWfx.nBlockAlign);
    newWfx.cbSize = extraSize;

    IXAudio2SourceVoice* newSourceVoice = nullptr;
    HRESULT hr =
        pXAudio2->CreateSourceVoice(&newSourceVoice, &newWfx, XAUDIO2_VOICE_USEFILTER, XAUDIO2_DEFAULT_FREQ_RATIO, 0, nullptr, nullptr);
    if (FAILED(hr)) {
        logger::error("[AudioManager] Failed to create source voice: {}", hr);
        delete[] newCopiedData;
        return false;
    }

    hr = newSourceVoice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr)) {
        logger::error("[AudioManager] Failed to submit source buffer: {}", hr);
        newSourceVoice->DestroyVoice();
        delete[] newCopiedData;
        return false;
    }

    // Atomic swap. After this critical section, any concurrent Update()
    // calling under voiceMtx will see the new voice; old pointers are now
    // ours alone to dispose of below.
    IXAudio2SourceVoice* oldSourceVoice = nullptr;
    BYTE* oldCopiedData = nullptr;
    {
        std::lock_guard<std::mutex> lock(voiceMtx);
        oldSourceVoice = pSourceVoice;
        oldCopiedData = copiedData;
        pSourceVoice = newSourceVoice;
        copiedData = newCopiedData;
        wfx = newWfx;
        forceSpatialMatrixUpdate = true;
    }

    // Cleanup OUTSIDE the lock — DestroyVoice can grab XAudio2's internal
    // cleanup spinlock; holding voiceMtx during that would re-create the
    // exact deadlock scenario we're trying to avoid.
    if (oldSourceVoice) {
        oldSourceVoice->Stop(0);
        oldSourceVoice->DestroyVoice();
    }
    if (oldCopiedData) {
        delete[] oldCopiedData;
    }

    logger::info("[AudioManager] Successfully loaded WAV file - Duration: ~{:.2f} seconds",
                 static_cast<float>(audioDataSize) / (newWfx.nAvgBytesPerSec));
    return true;
}


void AudioManager::Pause() {
    std::lock_guard<std::mutex> lock(voiceMtx);
    //logger::debug("[AudioManager] Pausing playback");

    if (pSourceVoice) {
        HRESULT hr = pSourceVoice->Stop(XAUDIO2_PLAY_TAILS);  // Pause instead of stopping completely
        if (SUCCEEDED(hr)) {
            isPaused = true;
            logger::debug("[AudioManager] Successfully paused source voice");
        } else {
            logger::error("[AudioManager] Failed to pause source voice: {}", hr);
        }
    } else {
        logger::warn("[AudioManager] Pause called with null source voice");
    }
}

void AudioManager::Resume() {
    std::lock_guard<std::mutex> lock(voiceMtx);
    //logger::debug("[AudioManager] Resuming playback");

    if (pSourceVoice) {
        HRESULT hr = pSourceVoice->Start(0);  // Resumes playback
        if (SUCCEEDED(hr)) {
            isPaused = false;
            logger::debug("[AudioManager] Successfully resumed source voice");
        } else {
            logger::error("[AudioManager] Failed to resume source voice: {}", hr);
        }
    } else {
        logger::warn("[AudioManager] Resume called with null source voice");
    }
}

bool AudioManager::Play() {
    std::lock_guard<std::mutex> lock(voiceMtx);
    if (!pSourceVoice) {
        logger::error("[AudioManager] Play called with null source voice");
        return false;
    }

    // Start with volume at 0 and ramp up
    currentVolume = 0.0f;
    pSourceVoice->SetVolume(currentVolume);
    logger::debug("[AudioManager] Starting playback with initial volume: {}", currentVolume);

    HRESULT hr = pSourceVoice->Start(0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        logger::error("[AudioManager] Failed to start playback: {}", hr);
        return false;
    }

    // Initialize ramping
    isRamping = true;
    rampStartTime = std::chrono::steady_clock::now();

    //logger::info("[AudioManager] Started audio playback with volume ramping");
    return true;
}

void AudioManager::setVolume(float vol) {
    // Clamp upper bound raised to 5.0 to match MCM AI Voice Volume slider; protection no longer needed.
    float normalized = vol / 100.0f;
    if (normalized < 0.0f) normalized = 0.0f;
    else if (normalized > 5.0f) normalized = 5.0f;
    defaultVolume.store(normalized, std::memory_order_relaxed);
    logger::info("[AudioManager] Set default volume to: {:.2f}", normalized);
}


bool AudioManager::getLegacyAudioNoattenuation() {
    return legacyAudioNoattenuation; 
}

void AudioManager::setLegacyAudioNoattenuation(bool value) { 
    legacyAudioNoattenuation = value; 
}

void AudioManager::UpdateLegacy(const X3DAUDIO_VECTOR& emitterPosition, const X3DAUDIO_VECTOR& listenerPosition,
                                float headingAngle) {
    if (!pSourceVoice) {
        return;
    }

    // ------------------------------------------------------------
    // Volume ramping
    // ------------------------------------------------------------

    if (isRamping) {
        auto currentTime = std::chrono::steady_clock::now();
        float elapsedSeconds = std::chrono::duration<float>(currentTime - rampStartTime).count();

        if (elapsedSeconds < rampDuration) {
            float newVolume = (elapsedSeconds / rampDuration) * defaultVolume;

            if (std::abs(newVolume - currentVolume) > 0.01f) {
                currentVolume = newVolume;
                pSourceVoice->SetVolume(currentVolume);
            }
        } else {
            if (std::abs(defaultVolume - currentVolume) > 0.01f) {
                currentVolume = defaultVolume;
                pSourceVoice->SetVolume(currentVolume);
            }

            isRamping = false;
        }
    } else if (std::abs(defaultVolume - currentVolume) > 0.01f) {
        currentVolume = defaultVolume;
        pSourceVoice->SetVolume(currentVolume);
    }

    // ------------------------------------------------------------
    // Calculate emitter position relative to listener
    // ------------------------------------------------------------

    constexpr float PI = 3.14159265358979323846f;

    X3DAUDIO_VECTOR relativePosition;
    relativePosition.x = emitterPosition.x - listenerPosition.x;
    relativePosition.y = emitterPosition.y - listenerPosition.y;
    relativePosition.z = emitterPosition.z - listenerPosition.z;

    X3DAUDIO_VECTOR rotatedPosition;

    rotatedPosition.x = relativePosition.x * cos(headingAngle) - relativePosition.y * sin(headingAngle);
    rotatedPosition.y = relativePosition.x * sin(headingAngle) + relativePosition.y * cos(headingAngle);

    rotatedPosition.z = relativePosition.z;

    // ------------------------------------------------------------
    // Update emitter position only when it moved significantly
    // ------------------------------------------------------------

    constexpr float positionThreshold = 0.1f;

    const float newX = std::round(rotatedPosition.x) / 100.0f;
    const float newY = std::round(rotatedPosition.y) / 100.0f;
    const float newZ = std::round(rotatedPosition.z) / 100.0f;

    bool positionChanged = true;

    if (std::abs(emitter.Position.x - newX) > positionThreshold ||
        std::abs(emitter.Position.y - newY) > positionThreshold ||
        std::abs(emitter.Position.z - newZ) > positionThreshold) {
        emitter.Position.x = newX;
        emitter.Position.y = newY;
        emitter.Position.z = newZ;

        positionChanged = true;
    }

    if (!positionChanged) {
        return;
    }

    // ------------------------------------------------------------
    // Distance attenuation
    //
    // distanceScaler > 0:
    //     Normal X3DAudio distance attenuation.
    //
    // distanceScaler == 0:
    //     Keep 3D spatialization, but compensate for the
    //     distance-related gain reduction.
    // ------------------------------------------------------------

    const float distanceScaler = getDistanceScaler();

    if (legacyAudioNoattenuation) {
        // Constant-volume 3D mode.
        //
        // We still let X3DAudio calculate the spatial matrix so
        // emitter position affects left/right/multichannel
        // spatialization.
        //
        // We then normalize the matrix to remove the overall
        // distance attenuation.
        //logger::debug("[AudioManager Legacy] Using constant-volume 3D mode (distanceScaler={})", distanceScaler);
        emitter.CurveDistanceScaler = 1.0f;

        X3DAudioCalculate(x3DInstance, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dspSettings);

        float maxCoefficient = 0.0f;

        for (UINT i = 0; i < dspSettings.DstChannelCount; ++i) {
            maxCoefficient = std::max(maxCoefficient, dspSettings.pMatrixCoefficients[i]);
        }

        if (maxCoefficient > 0.0001f) {
            for (UINT i = 0; i < dspSettings.DstChannelCount; ++i) {
                dspSettings.pMatrixCoefficients[i] /= maxCoefficient;
            }
        }
    } else {
        // Normal distance-attenuated 3D mode.
        //logger::debug("[AudioManager Legacy] Using normal 3D mode (distanceScaler={})", distanceScaler);
        emitter.CurveDistanceScaler = distanceScaler;

        X3DAudioCalculate(x3DInstance, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dspSettings);
    }

    // ------------------------------------------------------------
    // Apply calculated spatialization matrix
    // ------------------------------------------------------------

    HRESULT hr = pSourceVoice->SetOutputMatrix(pMasterVoice, wfx.nChannels, dspSettings.DstChannelCount,
                                               dspSettings.pMatrixCoefficients);

    if (FAILED(hr)) {
        logger::error("[AudioManager Legacy] Failed to set output matrix: {}", hr);
    }
}

void AudioManager::Update(const X3DAUDIO_VECTOR& emitterPosition,
                          const X3DAUDIO_VECTOR& listenerPosition, float headingAngle) {
    // try_to_lock: 90Hz from playback loop. Skip frame if Stop/LoadWAV is rebuilding
    // the voice — blocking starves Stop and races LoadWAV's swap → audio deadlock.
    std::unique_lock<std::mutex> lock(voiceMtx, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }

    if (!pSourceVoice) {
        //logger::warn("[AudioManager] Update called with null source voice");
        return;
    }

    // Handle volume ramping
    const float dv = defaultVolume.load(std::memory_order_relaxed);
    if (isRamping) {
        auto currentTime = std::chrono::steady_clock::now();
        float elapsedSeconds = std::chrono::duration<float>(currentTime - rampStartTime).count();

        if (elapsedSeconds < rampDuration) {
            float newVolume = (elapsedSeconds / rampDuration) * dv;
            if (std::abs(newVolume - currentVolume) > 0.01f) {
                currentVolume = newVolume;
                pSourceVoice->SetVolume(currentVolume);
            }
        } else {
            if (std::abs(dv - currentVolume) > 0.01f) {
                currentVolume = dv;
                pSourceVoice->SetVolume(currentVolume);
            }
            isRamping = false;
        }
    } else if (std::abs(dv - currentVolume) > 0.01f) {
        currentVolume = dv;
        pSourceVoice->SetVolume(currentVolume);
    }

    if (!spatialUpdatesEnabled) {
        return;
    }

    X3DAUDIO_VECTOR relativePosition;
    relativePosition.x = emitterPosition.x - listenerPosition.x;
    relativePosition.y = emitterPosition.y - listenerPosition.y;
    relativePosition.z = emitterPosition.z - listenerPosition.z;

    X3DAUDIO_VECTOR rotatedPosition;
    rotatedPosition.x = relativePosition.x * cos(headingAngle) - relativePosition.y * sin(headingAngle);
    rotatedPosition.y = relativePosition.x * sin(headingAngle) + relativePosition.y * cos(headingAngle);
    rotatedPosition.z = relativePosition.z;


    // Only update position if change is significant
    const float positionThreshold = 0.1f;
    bool positionChanged = forceSpatialMatrixUpdate;
    
    if (std::abs(emitter.Position.x - round(rotatedPosition.x)/100) > positionThreshold ||
        std::abs(emitter.Position.y - round(rotatedPosition.y)/100) > positionThreshold ||
        std::abs(emitter.Position.z - round(rotatedPosition.z)/100) > positionThreshold) {
        
        emitter.Position.x = round(rotatedPosition.x)/100;
        emitter.Position.y = round(rotatedPosition.y)/100;
        emitter.Position.z = round(rotatedPosition.z)/100;
        positionChanged = true;

       //logger::debug("[AudioManager] Updated 3D position - X: {:.2f}, Y: {:.2f}, Z: {:.2f}, Heading: {:.2f}°", emitter.Position.x, emitter.Position.y, emitter.Position.z, headingAngle);
    }

    if (positionChanged) {
        if (!dspSettings.pMatrixCoefficients || !pSourceVoice || !pMasterVoice) {
            return;
        }
        // Verify source voice is still active before updating spatial position.
        // XAudio2 crashes internally if the voice was destroyed/stopped on another
        // thread while we try to set the output matrix.
        try {
            XAUDIO2_VOICE_STATE state;
            pSourceVoice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued == 0) {
                return;  // Voice finished playing, don't update position
            }

            X3DAudioCalculate(x3DInstance, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dspSettings);

            HRESULT hr = pSourceVoice->SetOutputMatrix(pMasterVoice, wfx.nChannels, dspSettings.DstChannelCount,
                                           dspSettings.pMatrixCoefficients);
            if (FAILED(hr)) {
                static HRESULT lastLoggedOutputMatrixHr = S_OK;
                static auto lastOutputMatrixFailureLogTime = std::chrono::steady_clock::time_point{};
                const auto now = std::chrono::steady_clock::now();

                if (hr != lastLoggedOutputMatrixHr ||
                    now - lastOutputMatrixFailureLogTime > std::chrono::seconds(5)) {
                    logger::warn("[AudioManager] Failed to set output matrix: {}", hr);
                    lastLoggedOutputMatrixHr = hr;
                    lastOutputMatrixFailureLogTime = now;
                }
            } else {
                forceSpatialMatrixUpdate = false;
            }
        } catch (...) {
            // Voice was destroyed mid-update, safe to ignore
        }
    }
}

float AudioManager::GetElapsedTimeSeconds() {
    if (!pSourceVoice) {
        return 0.0f;
    }

    XAUDIO2_VOICE_STATE state;
    pSourceVoice->GetState(&state);

    // SamplesPlayed is total samples played since Start()
    UINT64 samplesPlayed = state.SamplesPlayed;

    // Convert samples to seconds
    float seconds = static_cast<float>(samplesPlayed) / wfx.nSamplesPerSec;

    return seconds;
}

X3DAUDIO_VECTOR AudioManager::ConvertNiPoint3ToX3DAUDIO_VECTOR(const RE::NiPoint3& niPoint) {
    X3DAUDIO_VECTOR x3dVector;
    x3dVector.x = niPoint.x;
    x3dVector.y = niPoint.y;
    x3dVector.z = niPoint.z;
    return x3dVector;
}

bool AudioManager::isPlaying() const {
    if (!pSourceVoice) {
        return false;  // No hay voz, no puede estar reproduciendo
    }

    XAUDIO2_VOICE_STATE state;
    pSourceVoice->GetState(&state);

    // Si hay buffers pendientes, sigue reproduciendo
    // También puedes combinar con isPaused para no confundir pausa con final
    return (state.BuffersQueued > 0) && !isPaused;
}
