#pragma once

#include <windows.h>
#include <x3daudio.h>
#include <xaudio2.h>
#include <chrono>

#include <string>

class AudioManager {
public:
    AudioManager();
    ~AudioManager();

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
                const X3DAUDIO_VECTOR& lookingAt,float headingAngle);
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

    float defaultVolume = 1.0f;
    bool spatialUpdatesEnabled = true;

    // Volume ramping members
    float currentVolume = 0.0f;
    const float rampDuration = 0.05f; // 50ms ramp
    const int rampSteps = 10;
    bool isRamping = false;
    std::chrono::steady_clock::time_point rampStartTime;

    // Other members for audio data, position, etc.

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
