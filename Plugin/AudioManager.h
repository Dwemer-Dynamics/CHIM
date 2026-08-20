#pragma once

// Force Win10 NT target before any Windows headers. CommonLibSSE / vcpkg
// propagation can re-define _WIN32_WINNT to 0x0601 (Win7) which makes
// x3daudio.h emit a "XAudio2 needs Win8 or later" #error. Putting the
// override in this header means any TU that pulls in AudioManager.h gets
// x3daudio.h with the right _WIN32_WINNT regardless of dependency ordering.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000

#include <windows.h>
#include <x3daudio.h>
#include <xaudio2.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include <string>

class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    // Non-copyable: callers must use AudioManagerController::GetInstance() and
    // hold a reference. Copying would clone pSourceVoice ownership semantics
    // and race with the mutex-protected lifetime management.
    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    bool Initialize();
    bool LoadWAV(BYTE* buffer, size_t dataSize);
    bool Play();
    void Stop();
    void Pause();
    void Resume();
    void setVolume(float vol);
    void setDistanceScaler(float vol);
    void setMuffledPlayback(bool enabled);
    void setSpatialUpdatesEnabled(bool enabled);
    void Update(const X3DAUDIO_VECTOR& sourcePosition, const X3DAUDIO_VECTOR& listenerPosition,
                float headingAngle);
    void UpdateLegacy(const X3DAUDIO_VECTOR& sourcePosition, const X3DAUDIO_VECTOR& listenerPosition, float headingAngle);
    float GetElapsedTimeSeconds();
    bool isPlaying() const;
    static X3DAUDIO_VECTOR ConvertNiPoint3ToX3DAUDIO_VECTOR(const RE::NiPoint3& niPoint);


    IXAudio2* pXAudio2 = nullptr;
    IXAudio2MasteringVoice* pMasterVoice = nullptr;
    IXAudio2SourceVoice* pSourceVoice = nullptr;
    X3DAUDIO_HANDLE x3DInstance;
    X3DAUDIO_LISTENER listener;
    X3DAUDIO_EMITTER emitter;
    X3DAUDIO_DSP_SETTINGS dspSettings;
    WAVEFORMATEX wfx = {};
    BYTE* copiedData = nullptr;

    std::atomic<float> defaultVolume{1.0f};
    bool spatialUpdatesEnabled = true;
    // Dirty flag: forces next Update() to recompute X3DAudio matrix even if emitter position hasn't moved past threshold. Set on Play/setSpatialUpdatesEnabled, cleared after successful SetOutputMatrix.
    bool forceSpatialMatrixUpdate = true;

    // Volume ramping members
    float currentVolume = 0.0f;
    const float rampDuration = 0.05f; // 50ms ramp
    const int rampSteps = 10;
    bool isRamping = false;
    std::chrono::steady_clock::time_point rampStartTime;

    // Other members for audio data, position, etc.

    // Serializes all access to pSourceVoice (Stop/Pause/Resume/Play/setVolume/
    // setMuffledPlayback/Update) and the LoadWAV atomic-swap of the voice
    // pointer. Without this, concurrent ProcessActor threads in the playback
    // wait loop call Update() while another thread's LoadWAV is mid-swap and
    // about to DestroyVoice() the previous voice — IXAudio2's graph-mod
    // operations (DestroyVoice in particular) are NOT thread-safe and will
    // deadlock on XAudio2's internal cleanup spinlock when raced with
    // SetVolume / SetOutputMatrix / Stop on the same voice. This pattern is
    // the port of the b7aa815 commit ("Complete mutex guard for AudioManager
    // thread safety", 2026-03-30) which action_rework regressed by removing.
    // Update() uses try_to_lock so its 90Hz tight loop never starves Stop().
    mutable std::mutex voiceMtx;

    void InitializeXAudio2();
    void CleanupXAudio2();
};

class AudioManagerController {
public:
    static AudioManager& GetInstance() {
        // Ensure initialization is done only once
        static AudioManager instance;
        static bool isInitialized = false;
        if (!isInitialized) {
            if (instance.Initialize()) {
                isInitialized = true;
            } else {
                // Handle initialization error
            }
        }
        return instance;
    }

private:
    AudioManagerController() = default;
    ~AudioManagerController() = default;
    AudioManagerController(const AudioManagerController&) = delete;
    AudioManagerController& operator=(const AudioManagerController&) = delete;
};
