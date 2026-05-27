#include "SpeakManager.h"

#include <Windows.h>
#include <WinInet.h>

#include <mmsystem.h>
#include <sphelper.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "Commands.h"
#include "Globals.h"
#include "Misc.h"
#include "Replacements.h"
#include "Conf.h"
#include "md5.h"
#include "xeme.h"
#include "json.hpp"
#include "AudioManager.h"
#include "SPGResponse.h"
#include "HTTPManager.h"
#include "PrismaUIBridge.h"
#include "SpatialAwareness.h"
#include <winhttp.h>
#include "ThreadPool.h"

#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "winhttp.lib")

namespace logger = SKSE::log;
using json = nlohmann::json;
extern const wchar_t* StringToWideString(std::string& str);

constexpr auto kVisemeTaskMinInterval = std::chrono::milliseconds(33);
constexpr auto kVisemeTaskStaleDisableAfter = std::chrono::milliseconds(500);
constexpr auto kVisemeTaskGuardLogInterval = std::chrono::seconds(5);
constexpr float kVisemeAbsoluteMaxIntensity = 0.82f;

extern bool GlobalEnable3DAudioPlayback;
extern bool GlobalInvertHeadingState;
extern bool GlobalCameraBasedAudio;
extern int GlobalConfiguredTimeout;
extern int GlobalRechatPolicyAsap;

extern std::chrono::high_resolution_clock::time_point controlLastBoredTriggerTS;

static void ExtendPostSpeechMaintenanceSuppress(std::chrono::seconds duration)
{
    ExtendPlayerSpeechMaintenanceSuppress(std::chrono::duration_cast<std::chrono::milliseconds>(duration));
}

// Get the actor's actual 3D head position for audio spatialization.
// During OStim/animation scenes, GetPosition() returns the scene origin which is
// often the player's position, causing NPC voice to come from inside the player's
// head. The skeleton head node tracks the actual visual position during animations.
// Falls back to GetPosition() if 3D or head node isn't available.
static RE::NiPoint3 GetActorHeadPosition(RE::Actor* actor) {
    if (!actor) return RE::NiPoint3();

    try {
        auto* root = actor->Get3D();
        if (root) {
            auto* headNode = root->GetObjectByName("NPC Head [Head]");
            if (headNode) {
                auto pos = headNode->world.translate;
                if (std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z)) {
                    return pos;
                }
            }
        }
    } catch (...) {
    }

    return actor->GetPosition();
}

static float GetYawFromQuaternionForAudio(const RE::NiQuaternion& q) {
    double siny_cosp = 2 * (q.w * q.z + q.x * q.y);
    double cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z);
    return std::atan2(siny_cosp, cosy_cosp) * -1.0f;
}

static void PushForcedPlayerSubtitle(RE::SubtitleManager* subtitleManager, RE::PlayerCharacter* player,
                                     const std::string& subtitleText)
{
    if (!subtitleManager || !player || subtitleText.empty()) {
        return;
    }

    RE::SubtitleInfo toSay;
    toSay.forceDisplay = true;
    toSay.targetDistance = 10;
    toSay.speaker = player->GetHandle();
    toSay.subtitle = subtitleText;
    toSay.pad04 = 0xabcd;

    subtitleManager->KillSubtitles();
    subtitleManager->subtitles.clear();
    subtitleManager->subtitles.push_back(toSay);
}

static int HoldTextOnlyPlayerSubtitle(SpeakManager& speakManager, const ScriptLine& scriptLine)
{
    constexpr auto kPlayerTextOnlySubtitleHoldDuration = std::chrono::seconds(2);
    constexpr auto kPlayerTextOnlySubtitleRefreshInterval = std::chrono::milliseconds(75);

    logger::info("[SpeakManager] Holding text-only player subtitle for {} ms: '{}'",
                 std::chrono::duration_cast<std::chrono::milliseconds>(kPlayerTextOnlySubtitleHoldDuration).count(),
                 scriptLine.subtitle);

    const auto holdUntil = std::chrono::steady_clock::now() + kPlayerTextOnlySubtitleHoldDuration;
    while (std::chrono::steady_clock::now() < holdUntil) {
        if (speakManager.shouldAbortPlaybackForSpeaker("Player")) {
            return 2;
        }

        speakManager.refreshPendingPlayerSubtitle();

        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(holdUntil - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            break;
        }

        std::this_thread::sleep_for(std::min(kPlayerTextOnlySubtitleRefreshInterval, remaining));
    }

    speakManager.refreshPendingPlayerSubtitle(true);
    return 3;
}

template <class TFaceGen>
static void ApplyVisemeFrame(TFaceGen* fgen, int lastViseme, int visemeCode, float intensity, float intensityStepDecal)
{
    for (int i = 0; i <= 15; i++) {
        if (i == visemeCode) {
            fgen->phenomeKeyFrame.SetValue(i, intensity);
        } else {
            float current = fgen->phenomeKeyFrame.values[i];
            current -= intensityStepDecal;

            if (lastViseme != i && false) {
                if (lastViseme == 0)
                    continue;
                else if (lastViseme == 1)
                    continue;
                else if (lastViseme == 5)
                    continue;
                else if (lastViseme == 6)
                    continue;
                else if (lastViseme == 8)
                    continue;
                else if (lastViseme == 11)
                    continue;
                else if (lastViseme == 12)
                    continue;
                else if (lastViseme == -1)
                    continue;
            }

            if (current < 0) current = 0.0f;
            fgen->phenomeKeyFrame.SetValue(i, current);
        }
    }
}

static void QueueMouthReset(RE::ActorHandle actorHandle)
{
    auto* taskInterface = SKSE::GetTaskInterface();
    if (!taskInterface) {
        logger::warn("[SpeakManager] Task interface unavailable for mouth reset");
        return;
    }

    taskInterface->AddTask([actorHandle]() {
        auto* actor = actorHandle.get().get();
        if (!actor || !actor->Is3DLoaded()) {
            return;
        }

        auto* fgen = actor->GetFaceGenAnimationData();
        if (!fgen) {
            return;
        }

        RE::BSSpinLockGuard locker(fgen->lock);
        for (int i = 0; i <= 15; ++i) {
            fgen->phenomeKeyFrame.SetValue(i, 0.0f);
        }
    });
}

static float GetVisemeMaxIntensity(int visemeCode)
{
    switch (visemeCode) {
        case -1:
            return 0.0f;
        case 0:   // aah
            return 0.56f;
        case 1:   // big aah
            return 0.68f;
        case 2:   // B, M, P
            return 0.54f;
        case 3:   // ch, J, sh
            return 0.51f;
        case 4:   // D, S, T
            return 0.38f;
        case 5:   // ee
            return 0.36f;
        case 6:   // eh
            return 0.49f;
        case 7:   // F, V
            return 0.49f;
        case 8:   // i
            return 0.36f;
        case 10:  // N
            return 0.29f;
        case 11:  // oh, ooh
            return 0.52f;
        case 13:  // R
            return 0.35f;
        case 14:  // th
            return 0.47f;
        case 15:  // W
            return 0.54f;
        default:
            return 0.43f;
    }
}

    //int animationDelayMicroSecs = 1000 * 1 ;  // 10 def value
int animationDelayMicroSecs = 500 * 1;  // 10 def value
float intensityModifier = 1.0f;

typedef unsigned long DWORD;

typedef unsigned char BYTE;
typedef unsigned int DWORD_;
typedef short SHORT;
typedef BYTE* LPBYTE;

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


static SpeakManager* speakManagerInstance = nullptr;
constexpr float DEFAULT_PLAYBACK_DROPOFF_PERCENT = 70.0f;
constexpr float MIN_PLAYBACK_DROPOFF_PERCENT = 25.0f;
constexpr float MAX_PLAYBACK_DROPOFF_PERCENT = 200.0f;
static std::atomic<float> g_playbackDropoffInsidePercent{ DEFAULT_PLAYBACK_DROPOFF_PERCENT };
static std::atomic<float> g_playbackDropoffOutsidePercent{ DEFAULT_PLAYBACK_DROPOFF_PERCENT };

static std::map<std::string, int> expressionMap = {
    {"DialogueAnger", 0},    {"DialogueFear", 1},    {"DialogueHappy", 2},     {"DialogueSad", 3},
    {"DialogueSurprise", 4}, {"DialoguePuzzled", 5}, {"DialogueDisgusted", 6}, {"MoodNeutral", 7},
    {"MoodAnger", 8},        {"MoodFear", 9},        {"MoodHappy", 10},        {"MoodSad", 11},
    {"MoodSurprise", 12},    {"MoodPuzzled", 13},    {"MoodDisgusted", 14},    {"CombatAnger", 15},
    {"CombatShout", 16}};

int findExpression(const std::string& key) {
    auto it = expressionMap.find(key);
    if (it != expressionMap.end()) {
        return it->second;  // Key found, return the associated value
    } else {
        return -1;  // Key not found
    }
}

static bool EqualsIgnoreCase(const std::string& left, const std::string& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }

    return true;
}

static std::string TrimCopy(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

static bool IsDirectlyAddressingPlayer(const std::string& targetName, AIAgentManager& aiam)
{
    const std::string normalizedTarget = TrimCopy(targetName);
    if (normalizedTarget.empty()) {
        return false;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return false;
    }

    const std::string playerName = TrimCopy(player->GetName());
    const std::string playerDisplayName = TrimCopy(player->GetDisplayFullName());
    const std::string configuredPlayerName = TrimCopy(aiam.getPlayerName());

    return EqualsIgnoreCase(normalizedTarget, playerName) ||
           EqualsIgnoreCase(normalizedTarget, playerDisplayName) ||
           (!configuredPlayerName.empty() && EqualsIgnoreCase(normalizedTarget, configuredPlayerName));
}

static std::string ResolveScriptLineListenerHint(const ScriptLine& scriptLine)
{
    return TrimCopy(scriptLine.action);
}

static std::string ResolveScriptLineRechatTargetHint(const ScriptLine& scriptLine)
{
    const std::string explicitHint = TrimCopy(scriptLine.rechatTargetHint);
    if (!explicitHint.empty()) {
        return explicitHint;
    }

    return TrimCopy(scriptLine.action);
}

static bool IsPlayerActorAlias(const std::string& actorName, AIAgentManager& aiam)
{
    const std::string normalizedActor = TrimCopy(actorName);
    if (normalizedActor.empty()) {
        return false;
    }

    if (EqualsIgnoreCase(normalizedActor, NARRATOR_NAME)) {
        return false;
    }

    if (EqualsIgnoreCase(normalizedActor, "Player")) {
        return true;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (player) {
        const std::string playerName = TrimCopy(player->GetName());
        if (!playerName.empty() && EqualsIgnoreCase(normalizedActor, playerName)) {
            return true;
        }

        const std::string playerDisplayName = TrimCopy(player->GetDisplayFullName());
        if (!playerDisplayName.empty() &&
            !EqualsIgnoreCase(playerDisplayName, NARRATOR_NAME) &&
            EqualsIgnoreCase(normalizedActor, playerDisplayName)) {
            return true;
        }
    }

    const std::string configuredPlayerName = TrimCopy(aiam.getPlayerName());
    return !configuredPlayerName.empty() && EqualsIgnoreCase(normalizedActor, configuredPlayerName);
}

static float ClampPlaybackDropoffPercent(float percent)
{
    if (!std::isfinite(percent)) {
        return DEFAULT_PLAYBACK_DROPOFF_PERCENT;
    }

    return std::clamp(percent, MIN_PLAYBACK_DROPOFF_PERCENT, MAX_PLAYBACK_DROPOFF_PERCENT);
}

static bool IsInteriorActor(RE::Actor* actor)
{
    if (!actor) {
        return false;
    }

    auto* cell = actor->GetParentCell();
    return cell && cell->IsInteriorCell();
}

static float ApplyDropoffAggressiveness(float volume, float dropoffPercent)
{
    const float clampedVolume = std::clamp(volume, 0.0f, 1.0f);
    const float attenuation = 1.0f - clampedVolume;
    const float attenuationScale = ClampPlaybackDropoffPercent(dropoffPercent) / 100.0f;
    const float tunedAttenuation = attenuation * attenuationScale;
    return std::clamp(1.0f - tunedAttenuation, 0.0f, 1.0f);
}

struct PlaybackSpatialAudioState
{
    float multiplier = 1.0f;
    float airDistance = 0.0f;
    float pathRatio = -1.0f;
    float rawSpatialVolume = 0.0f;
    float spatialVolume = 1.0f;
    float navmeshPenalty = 1.0f;
    float losPenalty = 1.0f;
    int openDoorCount = 0;
    int closedDoorCount = 0;
    bool canCommunicate = false;
    bool navmeshPathUsed = false;
    bool navmeshPathFound = false;
    bool losQueryOk = false;
    bool hasLineOfSight = false;
    bool muffled = false;
    bool losBlocked = false;
    bool interiorSpace = false;
    float dropoffPercent = DEFAULT_PLAYBACK_DROPOFF_PERCENT;
    std::string reason = "not_evaluated";
};

static PlaybackSpatialAudioState EvaluatePlaybackSpatialAudioForPlayer(RE::Actor* speaker, RE::Actor* listener)
{
    PlaybackSpatialAudioState state{};
    if (!speaker || !listener) {
        state.reason = "invalid_actor";
        return state;
    }

    const SpatialAwareness::Result spatial = SpatialAwareness::Evaluate(speaker, listener);
    state.airDistance = spatial.airDistance;
    state.reason = spatial.reason;
    state.pathRatio = spatial.pathRatio;
    state.rawSpatialVolume = spatial.volume;
    state.openDoorCount = spatial.openDoorCount;
    state.closedDoorCount = spatial.closedDoorCount;
    state.canCommunicate = spatial.canCommunicate;
    state.navmeshPathUsed = spatial.navmeshPathUsed;
    state.navmeshPathFound = spatial.navmeshPathFound;
    state.interiorSpace = IsInteriorActor(speaker);
    state.dropoffPercent = state.interiorSpace ? g_playbackDropoffInsidePercent.load() : g_playbackDropoffOutsidePercent.load();

    // Very light occlusion profile: keep cues, but avoid heavy drop-offs.
    const bool hardBarrier = (spatial.closedDoorCount > 0) || spatial.reason == "closed_door_between" ||
                             spatial.reason == "different_interior_cells" ||
                             spatial.reason == "interior_exterior_boundary";
    if (spatial.canCommunicate) {
        const float baseSpatialVolume = std::clamp(spatial.volume, 0.35f, 1.0f);
        state.spatialVolume = ApplyDropoffAggressiveness(baseSpatialVolume, state.dropoffPercent);
    } else if (hardBarrier) {
        state.spatialVolume = 0.15f;
        state.muffled = true;
    } else {
        state.spatialVolume = 0.25f;
    }

    if (spatial.navmeshPathUsed) {
        if (spatial.navmeshPathFound && std::isfinite(spatial.pathRatio) && spatial.pathRatio > 1.0f) {
            state.navmeshPenalty =
                std::clamp(1.0f / (1.0f + (spatial.pathRatio - 1.0f) * 0.05f), 0.95f, 1.0f);
        } else if (!spatial.navmeshPathFound) {
            state.navmeshPenalty = 0.90f;
        }
    }

    state.losQueryOk = spatial.losQueryOk;
    state.hasLineOfSight = spatial.losQueryOk && spatial.hasLineOfSight;
    state.losBlocked = spatial.losQueryOk && !spatial.hasLineOfSight;
    if (state.losBlocked) {
        state.muffled = true;
        state.losPenalty = 0.85f;
    }

    const bool closedDoorBarrier = (spatial.closedDoorCount > 0) || (spatial.reason == "closed_door_between");
    if (closedDoorBarrier) {
        state.muffled = true;
        state.losPenalty = std::min(state.losPenalty, 0.55f);
    }

    const float minPlaybackFloor = closedDoorBarrier ? 0.06f : 0.20f;
    state.multiplier = std::clamp(state.spatialVolume * state.navmeshPenalty * state.losPenalty, minPlaybackFloor, 1.0f);
    return state;
}


struct TextSegment {
    std::string text;
    std::string sourcetext;
    double duration;
    int visemeLength;
};

struct SilenceSegment {
    double start;
    double end;
};

double calculateWavDuration(LPBYTE wavData, DWORD& fileSize) {
    _LPWAVEFORMAT waveHeader = reinterpret_cast<_LPWAVEFORMAT>(wavData + sizeof(_WAVEDESCR));

    int32_t audioDataSize = fileSize - waveHeader->size;
    int32_t sampleRate = waveHeader->sampleRate;
    int16_t channels = waveHeader->channels;
    int16_t bitsPerSample = waveHeader->bitsPerSample;

    double duration = (double)audioDataSize / (sampleRate * channels * (bitsPerSample / 8.0));

    if (sampleRate == 24000)  // Azure
        if (duration > 1) duration = duration;

    return duration;
}


std::vector<SilenceSegment> detectSilences(LPBYTE wavData, DWORD& fileSize, double preClip,double silenceThreshold = 0.05,
                                           double minSilenceDuration = 150 ) {
    _LPWAVEFORMAT waveHeader = reinterpret_cast<_LPWAVEFORMAT>(wavData + sizeof(_WAVEDESCR));
    //DWORD audioDataSize = dataChunk->ckSize;

    size_t audioDataSize = fileSize - (sizeof(_WAVEDESCR) + sizeof(_WAVEFORMAT));


    short* audioData =
        reinterpret_cast<short*>(wavData + sizeof(_WAVEDESCR) + sizeof(_WAVEFORMAT) + sizeof(_DATA_CHUNK));

    int sampleRate = waveHeader->sampleRate;
    int channels = waveHeader->channels;
    int bitsPerSample = waveHeader->bitsPerSample;
    int samplesPerMillisecond = sampleRate / 1000;
    int silenceSampleThreshold =
        silenceThreshold * (pow(2, bitsPerSample - 1) - 1);  // assuming bitsPerSample bits audio

    //logger::debug("sampleRate : {}", sampleRate);
    //logger::debug("channels : {}", channels);
    //logger::debug("bitsPerSample : {}", bitsPerSample);
    //logger::debug("samplesPerMillisecond : {}", samplesPerMillisecond);
    //logger::debug("silenceSampleThreshold : {}", silenceSampleThreshold);
    //logger::debug("dataSize : {}", fileSize);
    //logger::debug("audioDataSize : {}", audioDataSize);

    std::vector<SilenceSegment> silences;
    bool inSilence = false;
    int silenceStart = 0;

    for (int i = 0; i < audioDataSize / sizeof(short); i += channels) {
        bool isSilent = false;
        for (int c = 0; c < channels; ++c) {
            if (std::abs(audioData[i + c]) < silenceSampleThreshold) {
                isSilent = true;
                //logger::info("silenceSampleThreshold : {},value {}", silenceSampleThreshold,std::abs(audioData[i + c]));
                break;
            } else {
                //logger::info("silenceSampleThreshold : {},value {}", silenceSampleThreshold,std::abs(audioData[i + c]));
            }
                
        }

        if (isSilent) {
            if (!inSilence) {
                inSilence = true;
                silenceStart = i / channels;
            }
        } else {
            if (inSilence) {
                int silenceEnd = i / channels;
                double silenceDuration = (silenceEnd - silenceStart) / static_cast<double>(samplesPerMillisecond);
                if ((silenceDuration) >= minSilenceDuration) {
                    auto ss = (SilenceSegment{silenceStart / static_cast<double>(samplesPerMillisecond) ,
                                              silenceEnd / static_cast<double>(samplesPerMillisecond) });

                    if (silenceStart / 1000 - preClip > 0) {
                        /*logger::info(
                            "Silence segment start {} secs, end {} secs, duration {} msecs, preclip used {}",
                            ss.start / 1000, ss.end / 1000, silenceDuration / 1000, preClip);*/
                        silences.push_back(ss);
                    } else {
                        /*logger::info(
                            "* discarded (begin of file?) silence segment start {} secs, end {} secs, duration {} msecs, preclip used {}",
                            ss.start / 1000, ss.end / 1000, silenceDuration / 1000, preClip);*/
                    }
                }
                inSilence = false;
            }
        }
    }

    if (inSilence) {
        int silenceEnd = audioDataSize / sizeof(short) / channels;
        double silenceDuration = (silenceEnd - silenceStart) / static_cast<double>(samplesPerMillisecond);
        if (silenceDuration >= minSilenceDuration) {
            auto ss = (SilenceSegment{silenceStart / static_cast<double>(samplesPerMillisecond),
                                      silenceEnd / static_cast<double>(samplesPerMillisecond)});
            silences.push_back(SilenceSegment{silenceStart / static_cast<double>(samplesPerMillisecond),
                                              silenceEnd / static_cast<double>(samplesPerMillisecond)});
            /*logger::info("Silence segment start {} secs, end {} secs, duration {} msecs, preclip used {}",
                         ss.start / 1000, ss.end / 1000, silenceDuration / 1000, preClip);*/
        }
    }
    int silenceEnd = audioDataSize / sizeof(short) / channels;

    /*logger::info("Detected {} silences, audio length {}", silences.size(),
                 silenceEnd / static_cast<double>(samplesPerMillisecond)/1000);*/
    return silences;
}

std::string toUppercase(const std::string& inputString) {
    std::string result;

    for (char c : inputString) {
        result += std::toupper(c);
    }

    return result;
}

std::vector<TextSegment> segmentText(const std::string& text_p, double soundDuration) {
    int currentPosition = 0;
    std::vector<TextSegment> segments;

    std::string text = toUppercase(text_p);

    // First pass: obtain segments prioritizing phonemes with 2 characters
    while (currentPosition < text.size()) {
        bool foundPhoneme = false;
        int viseme = -1;
        double visemeDuration = 0;
        int visemeLength;
        // Try to match phonemes with 2 characters
        for (const auto& entry : phonemeLabelToIdentifier) {
            const std::string& phoneme = entry.first;
            int phonemeIdentifier = entry.second;
            size_t phonemeLength = phoneme.length();

            if (phonemeLength == 2 && text.compare(currentPosition, phonemeLength, phoneme) == 0) {
                viseme = phonemeToViseme[phonemeIdentifier];
                // logger::debug("Phoneme 2 : {}", phonemeIdentifier);
                visemeDuration = (double(soundDuration) * phonemeLength) / text.length();
                visemeLength = 2;
                break;
            }
        }

        // If no 2-character phoneme is found, try 1-character phonemes
        if (viseme == -1) {
            for (const auto& entry : phonemeLabelToIdentifier) {
                const std::string& phoneme = entry.first;
                int phonemeIdentifier = entry.second;
                size_t phonemeLength = phoneme.length();

                if (phonemeLength == 1 && text[currentPosition] == phoneme[0]) {
                    viseme = phonemeToViseme[phonemeIdentifier];
                    //logger::debug("Phoneme 1 : {}", phonemeIdentifier);
                    visemeDuration = (double(soundDuration) * phonemeLength) / text.length();
                    visemeLength = 1;
                    break;
                }
            }
        }

        if (viseme != -1) {
            // Create a TextSegment and add it to the segments vector
            segments.push_back(
                {std::to_string(viseme), text.substr(currentPosition,visemeLength), visemeDuration, visemeLength});
            currentPosition += visemeLength;
            foundPhoneme = true;
        } else {
            //logger::debug("Phoneme not found for position : {}, {}", currentPosition, text[currentPosition]);
        }

        if (!foundPhoneme) {
            currentPosition++;
        }
    }

    // Second pass: update duration by interpolating based on total viseme length
    double totalVisemeDuration = 0;
    int totalVisemeLength = 0;
    for (const auto& segment : segments) {
        totalVisemeDuration += segment.duration;
        totalVisemeLength += segment.visemeLength;
    }

    for (auto& segment : segments) {
        double interpolatedDuration = (double(soundDuration) * segment.visemeLength) / totalVisemeLength;
        segment.duration = interpolatedDuration;
        //logger::info("Text {}, Viseme: {} {} ,Duration {}", text, segment.text, segment.sourcetext, segment.duration);
    }

    // TO close mouth at end
    segments.push_back({"-1", "_", 0.15, 1});

    return segments;
}


std::vector<TextSegment> segmentTextOrig(const std::string& text_p, double soundDuration) {
    int currentPosition = 0;
    double totalDuration = 0;
    std::vector<TextSegment> segments;

    std::string text = toUppercase(text_p);

    while (currentPosition < text.size()) {
        bool foundPhoneme = false;
        int viseme = -1;
        double visemeDuration = 0;

        // First, try to match phonemes with 2 characters
        for (const auto& entry : phonemeLabelToIdentifier) {
            const std::string& phoneme = entry.first;
            int phonemeIdentifier = entry.second;
            size_t phonemeLength = phoneme.length();

            if (phonemeLength == 2 && text.compare(currentPosition, phonemeLength, phoneme) == 0) {
                viseme = phonemeToViseme[phonemeIdentifier];
                visemeDuration = (double(soundDuration) * phonemeLength) / text.length();
                logger::info("Text {}, Current position {}, Phoneme {},Viseme: {},Duration {}", text, currentPosition,
                             phoneme, getVISEMEName(viseme), visemeDuration);
                break;
            }
        }

        // If no 2-character phoneme is found, try 1-character phonemes
        if (viseme == -1) {
            for (const auto& entry : phonemeLabelToIdentifier) {
                const std::string& phoneme = entry.first;
                int phonemeIdentifier = entry.second;
                size_t phonemeLength = phoneme.length();

                if (phonemeLength == 1 && text[currentPosition] == phoneme[0]) {
                    viseme = phonemeToViseme[phonemeIdentifier];
                    visemeDuration = (double(soundDuration) * phonemeLength) / text.length();

                    logger::info("Text {}, Current position {}, Phoneme {},Viseme: {},Duration {}", text,
                                 currentPosition, phoneme, getVISEMEName(viseme), visemeDuration);

                    break;
                }
            }
        }

        if (viseme != -1) {
            // Create a TextSegment and add it to the segments vector
            segments.push_back({std::to_string(viseme), "_",visemeDuration});
            totalDuration += visemeDuration;
            currentPosition += visemeDuration * text.length() / soundDuration;
            foundPhoneme = true;
        }

        if (!foundPhoneme) {
            currentPosition++;
        }
    }

    return segments;
}


bool isSilentAt(const std::vector<SilenceSegment>& silences, double elapsedSeconds) {
    for (const auto& segment : silences) {
        if ((elapsedSeconds * 1000) >= segment.start && (elapsedSeconds * 1000) <= segment.end) {
            return true;
        }
    }
    return false;
}


int DownloadAndPlay(std::string text, float preclip, float postclip, std::string speaker, std::string phonetic = "",
                    float volumeBoost = 1.0f, float forcedDuration = -1, bool applyMuffleFilter = false) {
    // [DAP-PHASE] markers: on freeze, last logged phase names the hung call.
    auto _dap_t0 = std::chrono::high_resolution_clock::now();
    auto _dap_phase = [&](const char* name) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::high_resolution_clock::now() - _dap_t0).count();
        logger::info("[DAP-PHASE name={} elapsed={}ms speaker={}]", name, ms, speaker);
    };

    _dap_phase("entry");
    logger::debug("[SpeakManager] Starting DownloadAndPlay - Text: '{}', PreClip: {}, PostClip: {}, Speaker: {}, Phonetic: {}, VolumeBoost: {}, Duration: {}",
                 text, preclip, postclip, speaker, phonetic, volumeBoost, forcedDuration);

    // Initialize the session
    HINTERNET hSession = WinHttpOpen(L"WinHTTP Example/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    _dap_phase("after_WinHttpOpen");
    if (!hSession) {
        logger::error("[SpeakManager] Failed to open WinHTTP session. Error: {}", GetLastError());
        return 1;
    }

    // Get server and port from configuration
    auto server = Conf::getInstance().getServer();
    auto port = std::stoi(Conf::getInstance().getPort());
    logger::debug("[SpeakManager] Connecting to server: {}:{}", server, port);

    // Convert server to wide string
    std::wstring wideServer = StringToWideString(server);

    // Connect to the server
    HINTERNET hConnect = WinHttpConnect(hSession, wideServer.c_str(), port, 0);
    _dap_phase("after_WinHttpConnect");
    if (!hConnect) {
        logger::error("[SpeakManager] Failed to connect to server. Error: {}", GetLastError());
        WinHttpCloseHandle(hSession);
        return 1;
    }

    // Construct file path and HTTP request path

    std::string path = Conf::getInstance().getPath();
    std::filesystem::path fullPath = path;
    std::filesystem::path dirName = fullPath.parent_path();
    path.assign(fullPath.parent_path().string());
    std::string hashedName = md5(trim(text),false);
    std::string fullPathFile = path.append("/soundcache/" + hashedName + ".wav").c_str();

    // Convert path to wide string
    std::wstring widePath = StringToWideString(fullPathFile);

    // Create the HTTP request
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", widePath.c_str(), NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_REFRESH);
    if (!hRequest) {
        logger::error("Failed to create HTTP request: {}", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 1;
    }

    // Send the request
    _dap_phase("before_WinHttpSendRequest");
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        logger::error("Failed to send HTTP request: {} {}", GetLastError(), fullPathFile);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 1;
    }
    _dap_phase("after_WinHttpSendRequest");

    // Wait for response
    _dap_phase("before_WinHttpReceiveResponse");
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        logger::error("Failed to receive HTTP response: {} {}", GetLastError(), fullPathFile);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 1;
    }
    _dap_phase("after_WinHttpReceiveResponse");

    // Query content length
    DWORD contentLength = 0;
    DWORD lengthSize = sizeof(contentLength);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, NULL, &contentLength,
                             &lengthSize, NULL)) {
        logger::error("Failed to query content length: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 1;
    }

    // Allocate buffer for the file content
    char* buffer = new char[contentLength];
    DWORD totalBytesRead = 0;
    DWORD bytesRead = 0;

    // Read the data
    logger::info("[SpeakManager] Reading data");
    _dap_phase("before_WinHttpReadData_loop");
    while (WinHttpReadData(hRequest, buffer + totalBytesRead, contentLength - totalBytesRead, &bytesRead) &&
           bytesRead > 0) {
        totalBytesRead += bytesRead;
    }
    _dap_phase("after_WinHttpReadData_loop");

    if (totalBytesRead != contentLength) {
        logger::info("Incomplete file download: expected {}, got {}", contentLength, totalBytesRead);
        delete[] buffer;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 1;
    }


    // Clean up
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    // Log success
    
    // Play the WAV file
    // UINT volume = 0xFFFF / 100;
    // waveOutSetVolume(NULL, MAKELONG(volume, volume));
    logger::info("Playing buffer: buffer size:  {} , bytes read  {}", contentLength, (bytesRead));

    std::shared_ptr<DWORD> contentLengthP = std::make_shared<DWORD>(contentLength);

    DWORD localContentLength = *contentLengthP;

    // removeLastHalfSecond(reinterpret_cast<BYTE*>(buffer), localContentLength);

    double duration = calculateWavDuration(reinterpret_cast<BYTE*>(buffer), localContentLength);

    auto& am = AudioManagerController::GetInstance();
    struct ScopedVolumeRestore {
        AudioManager& audio;
        float originalVolume;
        bool active;
        ~ScopedVolumeRestore() {
            if (active) {
                audio.setVolume(originalVolume * 100.0f);
            }
        }
    };
    ScopedVolumeRestore scopedVolumeRestore{am, am.defaultVolume, false};

    AIAgentManager& aiam = AIAgentManager::getInstance();

    auto currentActor = aiam.getAgentByName(speaker);
    RE::Actor* speakerActorPointer = nullptr;
    bool isNarrator = false;

    if (speaker == "Player") {
        speakerActorPointer = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        isNarrator = false;
    } else {
        if (!currentActor) {
            logger::info("{} is not an agent any more", speaker);
            return -1;
        }

        if (currentActor && currentActor->isNarrator()) {
            speakerActorPointer = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
            isNarrator = true;
        } else if (!currentActor && (speaker == RE::PlayerCharacter::GetSingleton()->GetDisplayFullName()))
            speakerActorPointer = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        else
            speakerActorPointer = currentActor.get()->getActor();
    }

    const float baseLineVolumeMultiplier = std::max(0.0f, volumeBoost);
    float runtimeLineVolumeMultiplier = baseLineVolumeMultiplier;
    bool runtimeMuffleFilter = applyMuffleFilter;

    auto* playbackListenerActor = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
    const bool dynamicSpatialPlayback =
        !isNarrator && speaker != "Player" && speakerActorPointer != nullptr && playbackListenerActor != nullptr;
    const bool enable3DAudioPlayback = dynamicSpatialPlayback && GlobalEnable3DAudioPlayback;

    if (enable3DAudioPlayback) {
        const PlaybackSpatialAudioState initialSpatialState =
            EvaluatePlaybackSpatialAudioForPlayer(speakerActorPointer, playbackListenerActor);
        runtimeLineVolumeMultiplier *= initialSpatialState.multiplier;
        runtimeMuffleFilter = runtimeMuffleFilter || initialSpatialState.muffled;
        logger::info(
            "[SpeakManager] SpatialAudioDBG init {} -> player: reason={} can={} dist={:.1f} rawVol={:.3f} tunedVol={:.3f} pathRatio={:.2f} pathUsed={} pathFound={} openDoors={} closedDoors={} losQueryOk={} los={} losPenalty={:.3f} navPenalty={:.3f} dropoffEnv={} dropoffPct={:.1f} lineMult={:.3f} muffled={}",
            speaker, initialSpatialState.reason, initialSpatialState.canCommunicate ? 1 : 0,
            initialSpatialState.airDistance, initialSpatialState.rawSpatialVolume, initialSpatialState.spatialVolume,
            initialSpatialState.pathRatio, initialSpatialState.navmeshPathUsed ? 1 : 0,
            initialSpatialState.navmeshPathFound ? 1 : 0, initialSpatialState.openDoorCount,
            initialSpatialState.closedDoorCount, initialSpatialState.losQueryOk ? 1 : 0,
            initialSpatialState.hasLineOfSight ? 1 : 0, initialSpatialState.losPenalty,
            initialSpatialState.navmeshPenalty, initialSpatialState.interiorSpace ? "interior" : "exterior",
            initialSpatialState.dropoffPercent, runtimeLineVolumeMultiplier, runtimeMuffleFilter ? 1 : 0);
    } else {
        logger::info(
            "[SpeakManager] SpatialAudioDBG skipped for '{}' (narrator={}, speakerPtr={}, listenerPtr={})",
            speaker, isNarrator ? 1 : 0, speakerActorPointer ? 1 : 0, playbackListenerActor ? 1 : 0);
    }

    //

    bool DXinitOK = false;

    bool hasBeenAborted = false;
    logger::debug("[SpeakManager] Loading WAV");
    am.setSpatialUpdatesEnabled(enable3DAudioPlayback);
    auto updatePlaybackSpatialPosition = [&]() {
        if (!enable3DAudioPlayback || !DXinitOK) {
            return;
        }

        auto headingAngle = RE::PlayerCharacter::GetSingleton()->GetAngleZ();
        auto camera = RE::PlayerCamera::GetSingleton();

        if (GlobalCameraBasedAudio && camera) {
            auto cameraState = camera->currentState.get();
            if (cameraState) {
                RE::NiQuaternion rotation;
                cameraState->GetRotation(rotation);
                auto cameraHeadingAngle = GetYawFromQuaternionForAudio(rotation);
                if (std::isfinite(cameraHeadingAngle)) {
                    headingAngle = cameraHeadingAngle;
                }
            }
        }

        if (GlobalInvertHeadingState)
            headingAngle += 3.14159265f;  // Add PI radians = 180 degrees

        auto speakerPos = GetActorHeadPosition(speakerActorPointer);

        // Use the player as the audio listener. In VR, cameraTarget can be a
        // transient/scene ref, collapsing NPC speech into the player's head.
        am.Update(AudioManager::ConvertNiPoint3ToX3DAUDIO_VECTOR(speakerPos),
                  AudioManager::ConvertNiPoint3ToX3DAUDIO_VECTOR(
                      RE::PlayerCharacter::GetSingleton()->GetPosition()),
                  headingAngle);
    };
    _dap_phase("before_LoadWAV");
    if (am.LoadWAV(reinterpret_cast<BYTE*>(buffer), localContentLength)) {
        _dap_phase("after_LoadWAV_ok");
        am.setMuffledPlayback(runtimeMuffleFilter);
        // Always set base playback volume. AudioManager ramps from 0 during Update(),
        // even when spatial positioning is disabled.
        if (std::abs(runtimeLineVolumeMultiplier - 1.0f) > 0.001f || enable3DAudioPlayback) {
            logger::info("[SpeakManager] Applying line volume multiplier: {}", runtimeLineVolumeMultiplier);
        }
        am.setVolume(scopedVolumeRestore.originalVolume * 100.0f * runtimeLineVolumeMultiplier);
        scopedVolumeRestore.active = true;
        DXinitOK = true;
        updatePlaybackSpatialPosition();
        _dap_phase("before_Play");
        if (!am.Play()) DXinitOK = false;
        _dap_phase("after_Play");
    } else {
        _dap_phase("after_LoadWAV_failed");
    }

    auto endTime = std::chrono::steady_clock::now()+ std::chrono::duration<double>(1);

    std::vector<TextSegment> textSegments;
    std::vector<SilenceSegment> silences;
    auto avoidClick = std::chrono::steady_clock::now() + std::chrono::duration<double>(preclip);
    // if (!PlaySoundA(buffer, NULL, SND_MEMORY | SND_ASYNC | SND_SYSTEM)) {
    if (!DXinitOK) {
        logger::info("Could not play buffer:  {}, we should now make something here", GetLastError());
        auto secondsToWait = static_cast<int>(std::ceil(static_cast<double>(text.length()) / 14.0));
        duration = secondsToWait;
        if (forcedDuration > 0) {
            duration = forcedDuration;
        }
        if (speaker == "Player") 
            duration = 2;

        logger::info("[SpeakerManager] faked duration {}",duration);
        textSegments = segmentText(text, duration - postclip);
        endTime = std::chrono::steady_clock::now() + std::chrono::duration<double>(duration);
        silences.push_back(SilenceSegment(0,0));
        logger::info("Buffer playing: buffer size:  {} {}, postclip {},preclip {}, text segments {}, imod {}",
                     localContentLength, duration, postclip, preclip, textSegments.size(), intensityModifier);

    } else {
        // Calculate the end time based on the duration
        endTime = std::chrono::steady_clock::now() + std::chrono::duration<double>(duration) -
                       std::chrono::duration<double>(postclip);

        

        if (phonetic != "") {
            logger::info("Found phonetic text input, using that for lip sync: {}", phonetic);
            text = phonetic;
        }
        
        textSegments = segmentText(text, duration - postclip);
        silences = detectSilences(reinterpret_cast<BYTE*>(buffer), localContentLength, preclip);

        logger::info("Buffer playing: buffer size:  {} {}, postclip {},preclip {}, text segments {}",
                     localContentLength, duration, postclip, preclip, textSegments.size());

    }
        // size:92 duration: 5,45 calculated as standard intensity
    const float referenceModifier = 92.0f / 5.45f;
    float intensityModifierDyn = (text.size() / duration) / referenceModifier;

    auto startTime = std::chrono::steady_clock::now();

    int lastViseme = 7;
    float intensity = 0;
    float intensityDecal = 0;

    auto fgen = speakerActorPointer->GetFaceGenAnimationData();
    auto lastTime = std::chrono::steady_clock::now();
    auto lastVisemeTaskQueued = startTime - kVisemeTaskMinInterval;
    auto lastVisemeTaskGuardLog = startTime - kVisemeTaskGuardLogInterval;
    auto visemeTaskInFlightSince = startTime;
    auto visemeTaskInFlight = std::make_shared<std::atomic<bool>>(false);
    bool visemeTaskGuardDisabled = false;
    int skippedVisemeTasks = 0;

    auto game = RE::UI::GetSingleton();

    bool recovery = false;

    const float lipIntensityBaseline = std::max(0.0f, SpeakManager::getInstance().getAnimIntensity());
    auto intensityModifier = lipIntensityBaseline * intensityModifierDyn;
    auto animationDelayMicroSecs = SpeakManager::getInstance().getResolution();

    

    bool lastLost = false;

    auto endTimeWithBlankSegment = endTime + std::chrono::duration<double>(0.15);
    auto lastRuntimeSpatialRefresh = std::chrono::steady_clock::now();
    auto lastRuntimeSpatialHeartbeatLog = std::chrono::steady_clock::now();
    std::string lastRuntimeSpatialLogSignature;
    bool hasLastRuntimeSpatialLogSignature = false;
    bool appliedMuffleFilter = runtimeMuffleFilter;
    float appliedLineVolumeMultiplier = runtimeLineVolumeMultiplier;

    speakerActorPointer->IncRefCount();  // Increment reference count to prevent actor from being unloaded

    //speakerActorPointer->GetActorRuntimeData().voiceTimer =
    bool unstablerun = false;

    _dap_phase("before_playback_wait_loop");
    _dap_phase("pre_watchdog_setup");

    // [LOOP-WATCHDOG] thread: per-iter setPhase advances an atomic; if it stays put
    // >2s a separate thread warns with the stuck phase name.
    auto loopWatchdogStart = std::chrono::steady_clock::now();
    std::atomic<const char*> loopPhase{"loop_init"};
    std::atomic<int64_t> loopPhaseStartMs{0};
    std::atomic<int64_t> loopIterCount{0};
    std::atomic<bool> loopRunning{true};
    std::mutex loopWakeMtx;
    std::condition_variable loopWakeCv;
    auto phaseNowMs = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - loopWatchdogStart).count();
    };
    auto setPhase = [&](const char* name) {
        loopPhase.store(name, std::memory_order_relaxed);
        loopPhaseStartMs.store(phaseNowMs(), std::memory_order_relaxed);
    };
    setPhase("loop_init");
    std::string speakerCopy = speaker;  // for safe access from watchdog thread
    _dap_phase("pre_watchdog_thread_spawn");
    std::thread loopWatchdog([&loopRunning, &loopPhase, &loopPhaseStartMs, &loopIterCount,
                              &loopWatchdogStart, &loopWakeMtx, &loopWakeCv, speakerCopy]() {
        logger::info("[LOOP-WATCHDOG STARTED speaker={}]", speakerCopy);
        const char* lastReportedPhase = nullptr;
        int64_t lastReportedStartMs = -1;
        while (loopRunning.load(std::memory_order_relaxed)) {
            // wait_for instead of sleep_for so cleanup notify_all wakes us instantly.
            std::unique_lock<std::mutex> lk(loopWakeMtx);
            loopWakeCv.wait_for(lk, std::chrono::seconds(3),
                                [&] { return !loopRunning.load(std::memory_order_relaxed); });
            lk.unlock();
            if (!loopRunning.load(std::memory_order_relaxed)) break;
            auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - loopWatchdogStart).count();
            const char* phase = loopPhase.load(std::memory_order_relaxed);
            int64_t startMs = loopPhaseStartMs.load(std::memory_order_relaxed);
            int64_t stuckMs = nowMs - startMs;
            if (stuckMs >= 2000) {
                bool sameAsLast = (phase == lastReportedPhase && startMs == lastReportedStartMs);
                logger::warn(
                    "[LOOP-WATCHDOG STUCK phase={} stuck_ms={} iter={} speaker={} repeat={}]",
                    phase ? phase : "<null>", stuckMs,
                    loopIterCount.load(std::memory_order_relaxed),
                    speakerCopy, sameAsLast ? 1 : 0);
                lastReportedPhase = phase;
                lastReportedStartMs = startMs;
            }
        }
    });

    // RAII guard: signals watchdog (notify wakes it instantly, no 0-3s join lag) and joins.
    struct WatchdogGuard {
        std::atomic<bool>& running;
        std::condition_variable& cv;
        std::thread& th;
        ~WatchdogGuard() {
            running.store(false, std::memory_order_relaxed);
            cv.notify_all();
            if (th.joinable()) th.join();
        }
    } _watchdogGuard{loopRunning, loopWakeCv, loopWatchdog};
    _dap_phase("post_watchdog_thread_spawn");

    while (std::chrono::steady_clock::now() < endTimeWithBlankSegment) {
        loopIterCount.fetch_add(1, std::memory_order_relaxed);
        setPhase("iter_top");
        // Your loop code here
        lastLost = false;
        setPhase("get_facegen_anim_data");
        fgen = speakerActorPointer->GetFaceGenAnimationData();
        if (!fgen) {

            logger::info("[SPEAKERMANAGER] NPC got lost while speaking?");
            setPhase("is_3d_loaded");
            if (!speakerActorPointer->Is3DLoaded()) {
                logger::info("[SPEAKERMANAGER] Actor has no 3d loaded, exiting this process");
                lastLost = true;
                break;
            }
            unstablerun = true;
            // lastLost = true;
            // break;
        }

        setPhase("game_paused_check");
        if (game->GameIsPaused() ) {
            if (RE::UI::GetSingleton()->IsItemMenuOpen()) {
                // Don't pause audio during item menu interactions
                // continue;
            } else if (PauseDialogueWhenMenuOpen) { // Pause audio based on MCM preference
                logger::info("Pausing audio...game is paused");
                setPhase("am_pause");
                am.Pause();
                recovery = true;

                // Wait for game to unpause
                setPhase("paused_wait_inner_loop");
                while (game->GameIsPaused()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (speakerActorPointer->IsDead() ||
                        SpeakManager::getInstance().shouldAbortPlaybackForSpeaker(speaker)) {
                        hasBeenAborted = true;
                        break;
                    }
                }

                if (!hasBeenAborted) {
                    logger::info("Resuming audio...game unpaused");
                    setPhase("am_resume_after_pause");
                    am.Resume();
                }
                continue;
            }
        } else if (recovery) {
            // Only resume if we were previously paused
            logger::info("Resuming audio after recovery");
            setPhase("am_resume_recovery");
            am.Resume();
            recovery = false;
        }

        setPhase("is_dead_check");
        if (speakerActorPointer->IsDead()) {
            logger::debug("Actor died {}:{}", speaker, text);
            hasBeenAborted = true;
            break;
        }
        if (SpeakManager::getInstance().shouldAbortPlaybackForSpeaker(speaker)) {
            logger::debug("Aborted {}:{}", speaker, text);
            hasBeenAborted = true;
            break;
        }

        if (DXinitOK && enable3DAudioPlayback) {
            const auto now = std::chrono::steady_clock::now();
            // 1500ms (was 500ms): each refresh can run a navmesh Dijkstra; avoid playback hitches in dense interiors.
            if (now - lastRuntimeSpatialRefresh >= std::chrono::milliseconds(1500)) {
                lastRuntimeSpatialRefresh = now;
                setPhase("evaluate_spatial_audio");
                const PlaybackSpatialAudioState runtimeSpatialState =
                    EvaluatePlaybackSpatialAudioForPlayer(speakerActorPointer, playbackListenerActor);

                const float updatedLineVolumeMultiplier =
                    std::max(0.0f, baseLineVolumeMultiplier * runtimeSpatialState.multiplier);
                const bool updatedMuffleFilter = runtimeSpatialState.muffled;

                bool changed = false;
                if (std::abs(updatedLineVolumeMultiplier - appliedLineVolumeMultiplier) > 0.03f) {
                    setPhase("am_setVolume_runtime");
                    am.setVolume(scopedVolumeRestore.originalVolume * 100.0f * updatedLineVolumeMultiplier);
                    scopedVolumeRestore.active = true;
                    appliedLineVolumeMultiplier = updatedLineVolumeMultiplier;
                    changed = true;
                }

                if (updatedMuffleFilter != appliedMuffleFilter) {
                    setPhase("am_setMuffledPlayback");
                    am.setMuffledPlayback(updatedMuffleFilter);
                    appliedMuffleFilter = updatedMuffleFilter;
                    changed = true;
                }

                const float appliedAbsoluteVolume = scopedVolumeRestore.originalVolume * appliedLineVolumeMultiplier;
                const std::string runtimeSpatialLogSignature = std::format(
                    "{}|{}|{:.1f}|{:.3f}|{:.3f}|{:.2f}|{}|{}|{}|{}|{}|{}|{:.3f}|{:.3f}|{}|{:.1f}|{:.3f}|{:.3f}|{}",
                    runtimeSpatialState.reason, runtimeSpatialState.canCommunicate ? 1 : 0,
                    runtimeSpatialState.airDistance, runtimeSpatialState.rawSpatialVolume,
                    runtimeSpatialState.spatialVolume, runtimeSpatialState.pathRatio,
                    runtimeSpatialState.navmeshPathUsed ? 1 : 0, runtimeSpatialState.navmeshPathFound ? 1 : 0,
                    runtimeSpatialState.openDoorCount, runtimeSpatialState.closedDoorCount,
                    runtimeSpatialState.losQueryOk ? 1 : 0, runtimeSpatialState.hasLineOfSight ? 1 : 0,
                    runtimeSpatialState.losPenalty, runtimeSpatialState.navmeshPenalty,
                    runtimeSpatialState.interiorSpace ? "interior" : "exterior", runtimeSpatialState.dropoffPercent,
                    appliedLineVolumeMultiplier, appliedAbsoluteVolume, appliedMuffleFilter ? 1 : 0);

                const bool stateChangedForLog =
                    !hasLastRuntimeSpatialLogSignature || runtimeSpatialLogSignature != lastRuntimeSpatialLogSignature;
                const bool heartbeatLog =
                    (now - lastRuntimeSpatialHeartbeatLog >= std::chrono::seconds(5));
                if (changed || stateChangedForLog || heartbeatLog) {
                    lastRuntimeSpatialHeartbeatLog = now;
                    lastRuntimeSpatialLogSignature = runtimeSpatialLogSignature;
                    hasLastRuntimeSpatialLogSignature = true;
                    logger::info(
                        "[SpeakManager] SpatialAudioDBG tick {} -> player: reason={} can={} dist={:.1f} rawVol={:.3f} tunedVol={:.3f} pathRatio={:.2f} pathUsed={} pathFound={} openDoors={} closedDoors={} losQueryOk={} los={} losPenalty={:.3f} navPenalty={:.3f} dropoffEnv={} dropoffPct={:.1f} lineMult={:.3f} absVol={:.3f} muffled={} changed={}",
                        speaker, runtimeSpatialState.reason, runtimeSpatialState.canCommunicate ? 1 : 0,
                        runtimeSpatialState.airDistance, runtimeSpatialState.rawSpatialVolume,
                        runtimeSpatialState.spatialVolume, runtimeSpatialState.pathRatio,
                        runtimeSpatialState.navmeshPathUsed ? 1 : 0, runtimeSpatialState.navmeshPathFound ? 1 : 0,
                        runtimeSpatialState.openDoorCount, runtimeSpatialState.closedDoorCount,
                        runtimeSpatialState.losQueryOk ? 1 : 0, runtimeSpatialState.hasLineOfSight ? 1 : 0,
                        runtimeSpatialState.losPenalty, runtimeSpatialState.navmeshPenalty,
                        runtimeSpatialState.interiorSpace ? "interior" : "exterior", runtimeSpatialState.dropoffPercent,
                        appliedLineVolumeMultiplier, appliedAbsoluteVolume, appliedMuffleFilter ? 1 : 0,
                        changed ? 1 : 0);
                }
            }
        }

        auto currentTime = std::chrono::steady_clock::now() - startTime;
        double elapsedSeconds = std::chrono::duration<double>(currentTime).count();
        
        //elapsedSeconds = am.GetElapsedTimeSeconds();  // New. More accurate elapsed time from audio manager, in case of
        //  pauses or other issues. Use only when real audio is being played, otherwise it can cause issues with timing
        //   of text segments. DXinitOK= true when real audio is being played, false when faking with timers.

        auto lastRun = std::chrono::steady_clock::now() - lastTime;
        double elapsedSeconds2 = std::chrono::duration<double>(lastRun).count();

        // logger::info("Elapsed: {} ", elapsedSeconds2);
        lastTime = std::chrono::steady_clock::now();

        // Find the text segment that should be playing at the current time
        if (!isNarrator) {
            TextSegment currentSegment;
            double totalDuration = 0.0;
            for (const TextSegment& segment : textSegments) {
                totalDuration += segment.duration;
                if (elapsedSeconds <= totalDuration) {
                    currentSegment = segment;
                    break;
                }
            }

            if (!currentSegment.text.empty()) {
                int visemeCode = std::atoi(currentSegment.text.c_str());

                if (isSilentAt(silences, elapsedSeconds)) {
                    /*
                    if (lastViseme == visemeCode) {
                        logger::info("Stoping mouth movement, silence detected at {} second", elapsedSeconds);
                    }
                    */
                    visemeCode = -1;
                }

                float intensityStep = 0.02 * (elapsedSeconds2 / 0.0019) * intensityModifier;
                float intensityStepDecal = std::min(intensityStep * 1.25f, 0.12f);

                //if (visemeCode == -1) visemeCode = 7;

                const float visemeMaxIntensity =
                    std::min(GetVisemeMaxIntensity(visemeCode) * lipIntensityBaseline, kVisemeAbsoluteMaxIntensity);
                const int candidateLastViseme = visemeCode;
                float candidateIntensity = intensity;
                if (lastViseme == visemeCode) {
                    candidateIntensity = candidateIntensity + intensityStep;
                } else {
                    /*
                    logger::info("At time {} segment: {} (Duration:{} seconds)", elapsedSeconds,
                        currentSegment.sourcetext, currentSegment.duration); 

                    logger::info("Viseme code {}, label:{}, last intensity (last viseme ended at) {}", visemeCode, getVISEMEName(visemeCode),
                                    intensity);
                    */
                    const float attackFloor = 0.10f * lipIntensityBaseline;
                    candidateIntensity = visemeCode >= 0 ? std::min(std::max(intensityStep, attackFloor), visemeMaxIntensity) : 0.0f;
                }

                if (candidateIntensity > visemeMaxIntensity) candidateIntensity = visemeMaxIntensity;

                auto commitVisemeCandidate = [&]() {
                    lastViseme = candidateLastViseme;
                    intensity = candidateIntensity;
                };

                if (fgen && !visemeTaskGuardDisabled) {
                    // Viseme frames are disposable; never let lip sync stack game-thread work in VR.
                    const auto visemeTaskNow = std::chrono::steady_clock::now();
                    if (visemeTaskNow - lastVisemeTaskQueued >= kVisemeTaskMinInterval) {
                        bool expected = false;
                        if (!visemeTaskInFlight->compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                            ++skippedVisemeTasks;
                            const auto stuckFor = visemeTaskNow - visemeTaskInFlightSince;
                            if (stuckFor >= kVisemeTaskStaleDisableAfter) {
                                visemeTaskGuardDisabled = true;
                                logger::warn(
                                    "[SpeakManager] Viseme freeze guard disabled lip updates for {}: previous task stuck {}ms, skipped {} frames",
                                    speaker,
                                    std::chrono::duration_cast<std::chrono::milliseconds>(stuckFor).count(),
                                    skippedVisemeTasks);
                            } else if (visemeTaskNow - lastVisemeTaskGuardLog >= kVisemeTaskGuardLogInterval) {
                                lastVisemeTaskGuardLog = visemeTaskNow;
                                logger::debug(
                                    "[SpeakManager] Viseme freeze guard skipped {} queued frames for {}",
                                    skippedVisemeTasks, speaker);
                            }
                            setPhase("viseme_task_skipped_inflight");
                        } else {
                            auto* taskInterface = SKSE::GetTaskInterface();
                            if (taskInterface) {
                                commitVisemeCandidate();
                                lastVisemeTaskQueued = visemeTaskNow;
                                visemeTaskInFlightSince = visemeTaskNow;

                                setPhase("write_voice_timer");
                                speakerActorPointer->GetActorRuntimeData().voiceTimer = 10.0;

                                setPhase("queue_viseme_task");
                                auto actorHandle = speakerActorPointer->GetHandle();
                                auto inFlight = visemeTaskInFlight;
                                const int queuedLastViseme = lastViseme;
                                const float queuedIntensity = intensity;
                                taskInterface->AddTask(
                                    [actorHandle, queuedLastViseme, visemeCode, queuedIntensity, intensityStepDecal, inFlight]() {
                                        struct VisemeTaskReset {
                                            std::shared_ptr<std::atomic<bool>> flag;
                                            ~VisemeTaskReset() { flag->store(false, std::memory_order_release); }
                                        } reset{inFlight};

                                        auto* actor = actorHandle.get().get();
                                        if (!actor || !actor->Is3DLoaded()) {
                                            return;
                                        }

                                        auto deferredFgen = actor->GetFaceGenAnimationData();
                                        if (!deferredFgen) {
                                            return;
                                        }

                                        RE::BSSpinLockGuard locker(deferredFgen->lock);
                                        ApplyVisemeFrame(deferredFgen, queuedLastViseme, visemeCode, queuedIntensity,
                                                         intensityStepDecal);
                                    });
                                setPhase("viseme_task_queued");
                            } else {
                                visemeTaskInFlight->store(false, std::memory_order_release);
                                logger::warn("[SpeakManager] Task interface unavailable for viseme update");
                            }
                        }
                    } else {
                        commitVisemeCandidate();
                    }
                } else {
                    // logger::warn("[SpeakManager] Failed to get FaceGen animation data for animation update");
                }
            }
        }

        setPhase("avoid_click_check");
        if (std::chrono::steady_clock::now() > avoidClick) {
            if (DXinitOK) {  // Only if audio being reproduced,
                setPhase("spatial_audio_position_update");
                updatePlaybackSpatialPosition();
            }
        }

        setPhase("iter_sleep");
        std::this_thread::sleep_for(std::chrono::microseconds(animationDelayMicroSecs));
    }
    setPhase("loop_exit");
    loopRunning.store(false, std::memory_order_relaxed);
    loopWakeCv.notify_all();
    if (loopWatchdog.joinable()) {
        loopWatchdog.join();
    }
    _dap_phase("after_playback_wait_loop");
    QueueMouthReset(speakerActorPointer->GetHandle());
    speakerActorPointer->GetActorRuntimeData().voiceTimer = 0.0;
    speakerActorPointer->DecRefCount();  // Increment reference count to prevent actor from being unloaded

    _dap_phase("before_am_Stop");
    am.Stop();
    _dap_phase("after_am_Stop");
    if (lastLost) {
        // To avoid rechat of a lost speech
        SpeakManager::getInstance().deleteQueue();
    }

    if (currentActor)
        logger::info("End talking sentence. {}", currentActor->getCurrentAnimation());

    // Free memory
    delete[] buffer;
    _dap_phase("exit_clean");

    if (hasBeenAborted) return 2;
    if (forcedDuration > 0)
        return 5;  // Currently only happens when called by MusicManager
                   // We wil use use this return value to not trigger rechat when music manager is calling,
                   // This is a bit of a hack, but it works for now. In the future, we may want to refactor
                   // this to be more elegant.
    return 0;

}


namespace SM {
    std::string ltrim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\n\r\f\v");
        return (start == std::string::npos) ? "" : s.substr(start);
    }

    // Trim trailing whitespace
    std::string rtrim(const std::string& s) {
        size_t end = s.find_last_not_of(" \t\n\r\f\v");
        return (end == std::string::npos) ? "" : s.substr(0, end + 1);
    }

    // Trim leading and trailing whitespace
    std::string trim(const std::string& s) { return rtrim(ltrim(s)); }

}

SpeakManager& SpeakManager::getInstance() {
    if (!speakManagerInstance) {
        speakManagerInstance = new SpeakManager();
    }
    return *speakManagerInstance;
}

std::chrono::high_resolution_clock::time_point SpeakManager::getLastUsedTime() {
    // logger::debug("[SpeakManager] Attempting to acquire mutex for getLastUsedTime");
    std::lock_guard<std::mutex> lock(mtx);
    // logger::debug("[SpeakManager] Mutex acquired for getLastUsedTime");
    auto result = lastUsedTime;
    // logger::debug("[SpeakManager] Releasing mutex for getLastUsedTime");
    return result;
}

void  SpeakManager::setLastUsedTime() {
    // logger::debug("[SpeakManager] Attempting to acquire mutex for setLastUsedTime");
    std::lock_guard<std::mutex> lock(mtx);
    // logger::debug("[SpeakManager] Mutex acquired for setLastUsedTime");
    lastUsedTime = std::chrono::high_resolution_clock::now();
    // logger::debug("[SpeakManager] Releasing mutex for setLastUsedTime");
}

void SpeakManager::insertInQueue(const ScriptLine& scriptLine) {
    // logger::debug("[SpeakManager] Attempting to acquire mutex for insertInQueue");
    std::lock_guard<std::mutex> lock(mtx);
    AIAgentManager& aiam = AIAgentManager::getInstance();
    // logger::debug("[SpeakManager] Mutex acquired for insertInQueue");
    // Trim whitespace from actor name before queueing
    ScriptLine trimmedLine = scriptLine;
    if (!trimmedLine.actor.empty()) {
        trimmedLine.actor.erase(0, trimmedLine.actor.find_first_not_of(" \t\n\r"));
        trimmedLine.actor.erase(trimmedLine.actor.find_last_not_of(" \t\n\r") + 1);
    }
    logger::debug("[SpeakManager] Queueing new line - Actor: {}, Text: '{}', Expression: '{}', Action: '{}', Animation: '{}', Phonetic: '{}'", 
                  trimmedLine.actor, trimmedLine.subtitle, trimmedLine.expression, trimmedLine.action, trimmedLine.animation, trimmedLine.phonetic);
    if (IsPlayerActorAlias(trimmedLine.actor, aiam)) {
        currentRechatChainId.clear();
        rechatInFlight = false;
        rechatInFlightSpeaker.clear();
        pendingRechatRetry = PendingRechatRetry{};
        lastRechatter.clear();
    }
    scriptQueue.push(trimmedLine);
    logger::info("[SpeakManager] Queue size after insertion: {}", scriptQueue.size());
    // logger::debug("[SpeakManager] Releasing mutex for insertInQueue");
}

ScriptLine SpeakManager::getFirstItem() {
    // logger::debug("[SpeakManager] Attempting to acquire mutex for getFirstItem");
    std::lock_guard<std::mutex> lock(mtx);
    // logger::debug("[SpeakManager] Mutex acquired for getFirstItem");

    // Drain Player lines stuck at the queue head. Player lines can be legitimately
    // queued for Player TTS (processPlayer handles those). Only drain if the line
    // has been at the head for 5+ seconds, meaning processPlayer didn't claim it.
    // This MUST run here because process() is gated by isProcessing and may never
    // be called when the flag is stuck.
    if (!scriptQueue.empty()) {
        static std::chrono::high_resolution_clock::time_point playerLineFirstSeen{};
        static std::string playerLineText{};

        const auto& front = scriptQueue.front();
        AIAgentManager& aiam = AIAgentManager::getInstance();
        bool isPlayerLine = IsPlayerActorAlias(front.actor, aiam);

        if (isPlayerLine) {
            auto now = std::chrono::high_resolution_clock::now();
            if (front.action == "__player_menu_tts") {
                // Menu Player TTS has its own request timeout. The generic drain is keyed only by text
                // and can falsely drop repeated dialogue choices before playback starts.
                playerLineFirstSeen = now;
                playerLineText.clear();
            } else if (isProcessing) {
                // The active Player line remains at the queue head until playback finishes.
                // Do not treat long Player Voice playback as a stuck queued line.
                playerLineFirstSeen = now;
                playerLineText = front.subtitle;
            } else if (playerLineText != front.subtitle) {
                playerLineFirstSeen = now;
                playerLineText = front.subtitle;
            } else {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - playerLineFirstSeen).count();
                if (elapsed >= 5) {
                    logger::warn("[SpeakManager] Draining stuck Player line after {}s: '{}'", elapsed, front.subtitle);
                    scriptQueue.pop();
                    playerLineText.clear();
                    // Also reset isProcessing in case it's stuck
                    isProcessing = false;
                    // Return empty so caller retries next cycle
                    return ScriptLine("", "", "", "", "", "");
                }
            }
        } else {
            playerLineText.clear();
        }
    }

    if (!scriptQueue.empty()) {
        auto result = scriptQueue.front();
        logger::debug("[SpeakManager] Releasing mutex for getFirstItem - found item for {}:{} ", result.actor,result.subtitle);
        return result;
    } else {
        //logger::debug("[SpeakManager] Releasing mutex for getFirstItem - queue empty");
        return ScriptLine("", "", "", "", "", "");
    }
}

ScriptLine SpeakManager::getLastItem() {
    // logger::debug("[SpeakManager] Attempting to acquire mutex for getFirstItem");
    std::lock_guard<std::mutex> lock(mtx);
    // logger::debug("[SpeakManager] Mutex acquired for getFirstItem");

    if (!scriptQueue.empty()) {
        auto result = scriptQueue.back();
        // logger::debug("[SpeakManager] Releasing mutex for getFirstItem - found item");
        return result;
    } else {
        // logger::debug("[SpeakManager] Releasing mutex for getFirstItem - queue empty");
        return ScriptLine("", "", "", "", "", "");
    }
}
void SpeakManager::dequeueFirstItem() {
    // logger::debug("[SpeakManager] Attempting to acquire mutex for dequeueFirstItem");
    std::lock_guard<std::mutex> lock(mtx);
    // logger::debug("[SpeakManager] Mutex acquired for dequeueFirstItem");

    if (!scriptQueue.empty()) {
        ScriptLine gone = scriptQueue.front();
        scriptQueue.pop();
        // logger::debug("[SpeakManager] Dequeued line - Actor: {}, Text: '{}'", gone.actor, gone.subtitle);
        // logger::info("[SpeakManager] Queue size after removal: {}", scriptQueue.size());
    } else {
        // logger::debug("[SpeakManager] Attempted to dequeue from empty queue");
    }
    // logger::debug("[SpeakManager] Releasing mutex for dequeueFirstItem");
}

bool SpeakManager::dropMatchingHeadItem(const ScriptLine& expectedLine) {
    std::lock_guard<std::mutex> lock(mtx);

    if (scriptQueue.empty()) {
        return false;
    }

    const auto& front = scriptQueue.front();
    const bool matches =
        front.actor == expectedLine.actor &&
        front.subtitle == expectedLine.subtitle &&
        front.action == expectedLine.action &&
        front.animation == expectedLine.animation &&
        front.expression == expectedLine.expression &&
        front.phonetic == expectedLine.phonetic;

    if (!matches) {
        return false;
    }

    scriptQueue.pop();
    return true;
}

bool SpeakManager::shouldAbortPlaybackForSpeaker(const std::string& speakerName) {
    std::lock_guard<std::mutex> lock(mtx);

    if (!interrupt) {
        return false;
    }

    if (forceInterruptCurrentPlayback) {
        interrupt = false;
        forceInterruptCurrentPlayback = false;
        return true;
    }

    if (scriptQueue.empty()) {
        interrupt = false;
        forceInterruptCurrentPlayback = false;
        return true;
    }

    const auto& front = scriptQueue.front();
    const std::string normalizedSpeaker = TrimCopy(speakerName);
    const std::string normalizedFrontActor = TrimCopy(front.actor);

    const bool sameSpeaker = !normalizedSpeaker.empty() && !normalizedFrontActor.empty() &&
                             EqualsIgnoreCase(normalizedSpeaker, normalizedFrontActor);

    if (sameSpeaker) {
        interrupt = false;
        forceInterruptCurrentPlayback = false;
        return false;
    }

    interrupt = false;
    forceInterruptCurrentPlayback = false;
    return true;
}

void SpeakManager::abortPendingUtterances(const std::string& reason, bool includeCurrentPlayback) {
    std::vector<std::string> utteranceIds;
    {
        std::lock_guard<std::mutex> lock(mtx);

        if (includeCurrentPlayback && !currentPlaybackUtteranceConfirmed &&
            !currentPlaybackUtteranceId.empty() && !IsPlayerActorAlias(currentPlaybackActor, AIAgentManager::getInstance())) {
            utteranceIds.push_back(currentPlaybackUtteranceId);
        }

        std::queue<ScriptLine> queueCopy = scriptQueue;
        while (!queueCopy.empty()) {
            const auto& currentItem = queueCopy.front();
            if (!currentItem.utteranceId.empty() &&
                !IsPlayerActorAlias(currentItem.actor, AIAgentManager::getInstance()) &&
                std::find(utteranceIds.begin(), utteranceIds.end(), currentItem.utteranceId) == utteranceIds.end()) {
                utteranceIds.push_back(currentItem.utteranceId);
            }
            queueCopy.pop();
        }
    }

    if (utteranceIds.empty()) {
        return;
    }

    json abortData;
    abortData["utterance_ids"] = utteranceIds;
    abortData["reason"] = reason;
    HTTPManager::log(std::format("_speech_abort|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), abortData.dump()));
}

void SpeakManager::deleteQueue(bool isActionCommand) {
    // logger::debug("[SpeakManager] Attempting to acquire mutex for deleteQueue");
    std::lock_guard<std::mutex> lock(mtx);
    // logger::debug("[SpeakManager] Mutex acquired for deleteQueue");
    
    if (isActionCommand && PreserveQueueDuringAction) {
        // logger::info("[SpeakManager] Queue deletion skipped - PreserveQueueDuringAction is enabled");
        // logger::debug("[SpeakManager] Releasing mutex for deleteQueue - early return");
        return;
    }

    // logger::info("[SpeakManager] Deleting queue - Current size: {}", scriptQueue.size());

    // Temporary queue to hold items we want to keep
    std::queue<ScriptLine> tempQueue;

    while (!scriptQueue.empty()) {
        // Check if the current item should be deleted
        const auto& currentItem = scriptQueue.front();
        if (IsPlayerActorAlias(currentItem.actor, AIAgentManager::getInstance())) {
            // Skip deleting and keep the item in the temporary queue
            tempQueue.push(currentItem);
        } else {
            // Log and delete items that aren't from "Player"
            HTTPManager::log(std::format("delete_event|{}|{}|{}: {}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         currentItem.actor, currentItem.subtitle));
        }
        // Remove the item from the original queue
        scriptQueue.pop();
    }

    // Swap back the kept items into the original queue
    scriptQueue = std::move(tempQueue);
    audienceSnapshotKey.clear();
    audienceSnapshotCompanions.clear();
    audienceSnapshotReady = false;
    pendingPlayerSubtitleActive = false;
    pendingPlayerSubtitleText.clear();
    // If we uncomment this, rechat will happen more often. but now it's hnday because if you talk a
    // lot with only one NPC, while another NPC present, you can focus on single conversation,
    // as second NPC will only try to rechat once after being interrupted
    // 
    //setLastRechatter("");// 

    // logger::debug("[SpeakManager] Releasing mutex for deleteQueue");
}

void SpeakManager::deleteQueuedPlayerLines() {
    std::lock_guard<std::mutex> lock(mtx);

    std::queue<ScriptLine> tempQueue;
    while (!scriptQueue.empty()) {
        const auto currentItem = scriptQueue.front();
        scriptQueue.pop();
        if (IsPlayerActorAlias(currentItem.actor, AIAgentManager::getInstance())) {
            continue;
        }
        tempQueue.push(currentItem);
    }

    scriptQueue = std::move(tempQueue);
    pendingPlayerSubtitleActive = false;
    pendingPlayerSubtitleText.clear();
}

bool SpeakManager::hasItems() {
    logger::debug("[SpeakManager] Attempting to acquire mutex for hasItems");
    std::lock_guard<std::mutex> lock(mtx);
    logger::debug("[SpeakManager] Mutex acquired for hasItems");
    bool result = !scriptQueue.empty();
    logger::debug("[SpeakManager] Releasing mutex for hasItems: {}", result);
    return result;
}

int SpeakManager::countItems() {
    logger::debug("[SpeakManager] Attempting to acquire mutex for countItems");
    std::lock_guard<std::mutex> lock(mtx);
    logger::debug("[SpeakManager] Mutex acquired for countItems");
    int n= scriptQueue.size();
    logger::debug("[SpeakManager] Releasing mutex for hasItems: {}", n);
    return n;
}


bool SpeakManager::getProcessing() {
    // logger::debug("[SpeakManager] Attempting to acquire mutex for getProcessing");
    std::unique_lock<std::mutex> lock(mtx);
    // logger::debug("[SpeakManager] Mutex acquired for getProcessing");
    bool result = isProcessing;
    // logger::debug("[SpeakManager] Releasing mutex for getProcessing: {}", result);
    return result;
}

void SpeakManager::setProcessing(bool processing) {
    // logger::debug("[SpeakManager] Attempting to acquire mutex for setProcessing");
    std::unique_lock<std::mutex> lock(mtx);
    // logger::debug("[SpeakManager] Mutex acquired for setProcessing");
    isProcessing = processing;
    logger::debug("[SpeakManager] Set processing state to: {}", processing);
    // logger::debug("[SpeakManager] Releasing mutex for setProcessing");
}

void SpeakManager::setPlayerPlaybackCompletedCallback(std::function<void(const ScriptLine&, int)> callback) {
    std::lock_guard<std::mutex> lock(mtx);
    playerPlaybackCompletedCallback = std::move(callback);
}

void SpeakManager::clearPlayerPlaybackCompletedCallback() {
    std::lock_guard<std::mutex> lock(mtx);
    playerPlaybackCompletedCallback = nullptr;
}

void SpeakManager::recoverFromProcessingFailure(const std::string& actorName) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    const std::string playerName = (player && player->GetName()) ? player->GetName() : "";

    bool droppedHead = false;
    ScriptLine droppedLine("", "", "", "", "", "");

    {
        std::lock_guard<std::mutex> lock(mtx);
        interrupt = false;
        forceInterruptCurrentPlayback = false;
        isProcessing = false;
        audienceSnapshotKey.clear();
        audienceSnapshotCompanions.clear();
        audienceSnapshotReady = false;
        playerPlaybackCompletedCallback = nullptr;

        if (!scriptQueue.empty()) {
            const auto& front = scriptQueue.front();
            const bool failedIsPlayer = actorName == "Player" || (!playerName.empty() && actorName == playerName);
            const bool headIsPlayer = front.actor == "Player" || (!playerName.empty() && front.actor == playerName);
            const bool failedIsNarrator = actorName == NARRATOR_NAME;
            const bool headIsNarrator = front.actor == NARRATOR_NAME;

            if (front.actor == actorName || (failedIsPlayer && headIsPlayer) || (failedIsNarrator && headIsNarrator)) {
                droppedLine = front;
                scriptQueue.pop();
                droppedHead = true;
            }
        }
    }

    if (droppedHead) {
        logger::warn("[SpeakManager] Recovered from processing failure. Dropped stuck head item for {}: '{}'",
                     droppedLine.actor, droppedLine.subtitle);
    } else {
        logger::warn("[SpeakManager] Recovered from processing failure for actor '{}'. No matching queue head to drop.",
                     actorName);
    }

    clearVisibleSubtitles();

    if (player && (actorName == NARRATOR_NAME || actorName == "Player" || (!playerName.empty() && actorName == playerName))) {
        AIAgentManager& aiam = AIAgentManager::getInstance();
        auto originalName = aiam.getPlayerName();
        if (originalName == "Prisoner") {
            originalName = playerName;
            aiam.setPlayerName(originalName);
        }
        player->SetDisplayName(originalName.c_str(), true);
        logger::info("[SpeakManager] Restored player display name to '{}' during failure recovery", originalName);
    }
}


void SpeakManager::setPreclip(float val) {
    preClip = val / 1000; 
}

void SpeakManager::setPostclip(float val) { 
    postClip = val / 1000;
}

void SpeakManager::resetRechatChainState()
{
    std::lock_guard<std::mutex> lock(mtx);
    currentRechatChainId.clear();
    rechatChainClosed = false;
    rechatInFlight = false;
    rechatInFlightSpeaker.clear();
    pendingRechatRetry = PendingRechatRetry{};
}

bool SpeakManager::isRechatChainClosed()
{
    std::lock_guard<std::mutex> lock(mtx);
    return rechatChainClosed;
}

std::string SpeakManager::ensureRechatChainId(const std::string& speaker, const std::string& listenerHint,
                                              const std::string& explicitTarget)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (!currentRechatChainId.empty()) {
        return currentRechatChainId;
    }

    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    const std::string seed = speaker + "\x1f" + listenerHint + "\x1f" + explicitTarget + "\x1f" + std::to_string(micros);
    const auto chainHash = static_cast<unsigned long long>(std::hash<std::string>{}(seed));
    currentRechatChainId = std::format("r{:016x}", chainHash);
    return currentRechatChainId;
}

void SpeakManager::setPlaybackDropoffInside(float percent)
{
    const float clampedValue = ClampPlaybackDropoffPercent(percent);
    g_playbackDropoffInsidePercent.store(clampedValue);
    logger::info("[SpeakManager] Set interior playback dropoff aggressiveness to {:.1f}%", clampedValue);
}

void SpeakManager::setPlaybackDropoffOutside(float percent)
{
    const float clampedValue = ClampPlaybackDropoffPercent(percent);
    g_playbackDropoffOutsidePercent.store(clampedValue);
    logger::info("[SpeakManager] Set exterior playback dropoff aggressiveness to {:.1f}%", clampedValue);
}

float SpeakManager::getPlaybackDropoffInside()
{
    return g_playbackDropoffInsidePercent.load();
}

float SpeakManager::getPlaybackDropoffOutside()
{
    return g_playbackDropoffOutsidePercent.load();
}

bool SpeakManager::beginRechatAttempt(const std::string& speaker)
{
    if (speaker.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mtx);
    rechatInFlight = true;
    rechatInFlightSpeaker = speaker;
    return true;
}

void SpeakManager::queueRechatRetry(const std::string& speaker, const std::string& listenerHint,
                                    const std::string& explicitTarget, const std::string& debugLauncherLine,
                                    int rechatDepth)
{
    if (speaker.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mtx);
    if (!rechatInFlight || rechatInFlightSpeaker != speaker) {
        return;
    }

    pendingRechatRetry.active = true;
    pendingRechatRetry.speaker = speaker;
    pendingRechatRetry.listenerHint = listenerHint;
    pendingRechatRetry.explicitTarget = explicitTarget;
    pendingRechatRetry.debugLauncherLine = debugLauncherLine;
    pendingRechatRetry.rechatDepth = rechatDepth;
}

void SpeakManager::completeRechatAttempt(const std::string& speaker, bool success)
{
    PendingRechatRetry retry;
    bool shouldRetry = false;

    {
        std::lock_guard<std::mutex> lock(mtx);
        if (!rechatInFlight || rechatInFlightSpeaker != speaker) {
            return;
        }

        rechatInFlight = false;
        rechatInFlightSpeaker.clear();

        if (success) {
            rechatChainClosed = false;
            lastRechatter = speaker;
            pendingRechatRetry = PendingRechatRetry{};
            return;
        }

        if (pendingRechatRetry.active && pendingRechatRetry.speaker == speaker) {
            retry = pendingRechatRetry;
            shouldRetry = true;
        }
        if (!shouldRetry) {
            rechatChainClosed = true;
        }
        pendingRechatRetry = PendingRechatRetry{};
    }

    if (!shouldRetry) {
        return;
    }

    if (rechat(retry.speaker, retry.listenerHint, retry.rechatDepth, retry.debugLauncherLine,
               retry.explicitTarget) > 0) {
        beginRechatAttempt(retry.speaker);
    }
}




int SpeakManager::rechat(std::string speaker, std::string targetedNpc, int rechatDepth, std::string debugLauncherLine,
                         std::string explicitRechatTarget) {

    // Rechat code moved here. To keep better time consistency we issue the rechat event once speaker
    // starts to say its lasts sentence.
    // Pros. Better sync with enviroment. 
    // Cons. Feels slower if using hihg latency models.

    // RECHAT
    // PROBLEM IF WE CHANGE SPEAKER SERVER SIDE
    auto tid = ThreadPool::getInstance().getCurrentTaskId();

    logger::info("[RECHAT] spoke: {} , listener: {}, explicit target: {}, depth  {},taskid {} ", speaker, targetedNpc,
                 explicitRechatTarget, rechatDepth, tid);
    logger::debug("[RECHAT] origin line: {} ", debugLauncherLine);
    auto player = RE::PlayerCharacter::GetSingleton();
    if (player->IsSneaking()) {
        logger::info("[RECHAT] Rechat avoided because stealth: {}", speaker);
        return 0;
    }

    
    if (std::chrono::high_resolution_clock::now() < rechatCooldown ) {
        logger::info("[RECHAT] Rechat avoided because cooldown: {}", speaker);
        return 0;
    }
    

    AIAgentManager& aiam = AIAgentManager::getInstance();
    auto agentLastSpeaker = aiam.getAgentByName(speaker);
    if (!agentLastSpeaker) {
        logger::debug("[RECHAT] AgentLastSpeaker is null for agent: {}", speaker);
        return 0;
    }

    auto speakerActor = agentLastSpeaker->getActor();
    if (!speakerActor) {
        logger::debug("[RECHAT] Speaker actor is null for agent: {}", speaker);
        return 0;
    }
    
    if (agentLastSpeaker) {
        if (agentLastSpeaker->isNarrator()) {
            // Check if this was a random narration event vs regular narrator dialogue
            extern std::string lastEventType;
            if (lastEventType == "narration") {
                logger::info("[RECHAT] Continuing rechat after random narration from {}", speaker);
                // Continue rechat normally, don't return
            } else {
                logger::info("[RECHAT] Rechat avoided as {} spoke {}", NARRATOR_NAME, speaker);
                return 0;
            }
        }
    }

    SPGResponse& spgResponse = SPGResponse::getInstance();
    bool commandInQueue = spgResponse.getSize("command") > 0;

    int n = ThreadPool::getInstance().runningTasksByType("HTTPStream");
    if (n > 0 && GlobalRechatPolicyAsap == 0) {
        logger::info("[RECHAT] Rechat avoid because another stream is active HTTPStream ");
        return 0;
    }

    n = ThreadPool::getInstance().runningTasksByType("HTTPStreamRechat");
    if (n > 0 && GlobalRechatPolicyAsap==0) {
        logger::info("[RECHAT] Rechat avoid because another stream is active HTTPStreamRechat");
        return 0;
    }

    if (!commandInQueue) {
        if (RE::MenuTopicManager::GetSingleton()->unkB1) {
            logger::debug("[RECHAT] Avoiding rechat event because player is in dialogue");
            return 0;
        }

        std::vector<std::string> audienceSnapshot;
        {
            std::lock_guard<std::mutex> lock(mtx);
            audienceSnapshot = audienceSnapshotCompanions;
        }

        json rechatPayload = json::object();
        const std::string rechatChainId = ensureRechatChainId(speaker, targetedNpc, explicitRechatTarget);
        rechatPayload["speaker"] = speaker;
        rechatPayload["listener_hint"] = targetedNpc;
        rechatPayload["rechat_target_hint"] = explicitRechatTarget;
        rechatPayload["origin_line"] = debugLauncherLine;
        rechatPayload["rechat_depth"] = rechatDepth;
        rechatPayload["audience"] = audienceSnapshot;
        rechatPayload["chain_id"] = rechatChainId;

        std::vector<std::string> chainMembers = audienceSnapshot;
        if (!speaker.empty() &&
            std::find(chainMembers.begin(), chainMembers.end(), speaker) == chainMembers.end()) {
            chainMembers.push_back(speaker);
        }
        if (!targetedNpc.empty() &&
            std::find(chainMembers.begin(), chainMembers.end(), targetedNpc) == chainMembers.end()) {
            chainMembers.push_back(targetedNpc);
        }
        if (!explicitRechatTarget.empty() &&
            std::find(chainMembers.begin(), chainMembers.end(), explicitRechatTarget) == chainMembers.end()) {
            chainMembers.push_back(explicitRechatTarget);
        }
        rechatPayload["chain_members"] = chainMembers;

        HTTPManager::stream(
            std::format("{}|{}|{}|{}", "rechat", getCurrentTimeMillis(), GetGameTimeStamp(), rechatPayload.dump()),
            speakerActor,
            rechatDepth + 1
        );
        return 1;
    } else {
        ResponseItem pending = spgResponse.getFirstItem("command");
        logger::info("Rechat avoided as command pending: {}", pending.text);
    }

    return 0;

}

void SpeakManager::process(AIAgent *agent) {

    auto tid = std::this_thread::get_id();
    logger::debug("[SPEAKERMANAGER {}] Starting process for agent: {}",
                  tid, agent ? agent->getActorName() : "null");

    // Add null check to prevent crash
    if (!agent) {
        logger::error("[SPEAKERMANAGER {}] Received null agent pointer",tid);
        setProcessing(false);
        return;
    }

    bool hasTalked = false;
    RE::Actor* npc = agent->getActor();
    AIAgentManager& aiam = AIAgentManager::getInstance();

    if (!npc) {
        logger::error("[SPEAKERMANAGER {}] Actor not instantiated for agent: {}",tid, agent->getActorName());
        return;
    }

    if (isProcessing) {
        logger::debug("[SPEAKERMANAGER {}] Manager is busy processing another request",tid);
        return;
    }

    if (!agent->isNarrator() && (!npc->GetActorRuntimeData().currentProcess || !npc->Is3DLoaded())) {
        logger::info("[SPEAKERMANAGER {}] Agent {} is not currently loaded for dialogue. Skipping.", tid,
                     agent->getActorName());
        dequeueFirstItem();
        setProcessing(false);
        return;
    }

    if (npc->IsDead()) {
        logger::warn("[SPEAKERMANAGER {}] Actor {} is dead. Deleting agent and queue.",tid, agent->getActorName());
        aiam.deleteAgentByName(agent->getActorName());
        deleteQueue();
        return;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    float distance = npc->GetPosition().GetDistance(player->GetPosition());
    logger::debug("[SpeakManager] Distance to player: {} units (min required: {})", distance, MIN_DISTANCE);

    if (distance > MIN_DISTANCE) {
        logger::info("[SPEAKERMANAGER {}] {} is too far ({} units). Discarding speech.", tid,npc->GetDisplayFullName(), distance);
        dequeueFirstItem();
        setProcessing(false);
        return;
    }

    if (distance > 3000) {
        int nfac = agent->getFarAwayCounter();
        if ( nfac < 3) {
            agent->increaseFarAwayCounter();
            logger::info("{} is {} units away (too far) (retry {})", npc->GetDisplayFullName(), distance, nfac);
            return;
        } else {
            logger::info("{} is {} units away (forced by nfac: {})", npc->GetDisplayFullName(), distance, nfac);
            agent->resetFarAwayCounter();
        }
    }

    if (!isProcessing && !scriptQueue.empty()) {
        setProcessing(true);

        logger::info("[SPEAKERMANAGER {}] SpeakManager is processing now for {}",
                     tid,agent->getActorName());
        
        ScriptLine scriptLine = getFirstItem();
        {
            std::lock_guard<std::mutex> lock(mtx);
            currentPlaybackUtteranceId = scriptLine.utteranceId;
            currentPlaybackActor = scriptLine.actor;
            currentPlaybackUtteranceConfirmed = false;
        }

        // Reset bored
        controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();

        const auto clearCurrentPlayback = [&]() {
            std::lock_guard<std::mutex> lock(mtx);
            currentPlaybackUtteranceId.clear();
            currentPlaybackActor.clear();
            currentPlaybackUtteranceConfirmed = false;
        };

        if (!agent->isNarrator()) {
            const std::string listenerHint = TrimCopy(scriptLine.action);
            auto explicitListenerAgent = !listenerHint.empty() ? aiam.getAgentByName(listenerHint) : nullptr;
            auto* explicitListenerActor = explicitListenerAgent ? explicitListenerAgent->getActor() : nullptr;
            const bool listenerIsPlayer =
                (explicitListenerActor && explicitListenerActor == player) ||
                IsDirectlyAddressingPlayer(listenerHint, aiam);

            if (player && npc && (listenerHint.empty() || listenerIsPlayer)) {
                uint32_t overrideFormId = 0;
                std::string overrideName;
                const bool isChatboxOverrideTarget =
                    PrismaUIBridge::GetChatboxTargetOverride(overrideFormId, overrideName) &&
                    ((overrideFormId != 0 && npc->GetFormID() == overrideFormId) ||
                     (!overrideName.empty() && agent->getActorName() == overrideName));

                if (!isChatboxOverrideTarget) {
                    // Air-distance-only gate; full SpatialAwareness::Evaluate from this worker thread SEH-crashed at 0x58 (LOS/navmesh/door scan touch game-thread refs).
                    const float airDistance = npc->GetPosition().GetDistance(player->GetPosition());
                    const auto spatialSettings = GetPlayerSpeechSpatialSettings(player, HERIKA_MAX_VISION_RANGE);
                    const auto* playerCell = player->GetParentCell();
                    const bool playerInterior = playerCell && playerCell->IsInteriorCell();
                    const float hearingMax = playerInterior ? spatialSettings.interiorMaxDistance :
                                                              spatialSettings.exteriorMaxDistance;
                    if (std::isfinite(hearingMax) && hearingMax > 0.0f && airDistance > hearingMax) {
                        logger::info(
                            "[SpeakManager] Skipping NPC->Player playback: {} -> player (target='{}', air_dist={:.1f} > hearing_max={:.1f})",
                            agent->getActorName(), listenerHint, airDistance, hearingMax);
                        dequeueFirstItem();
                        releasePendingPlayerSubtitle();
                        clearVisibleSubtitles();
                        clearCurrentPlayback();
                        setProcessing(false);
                        return;
                    }
                }
            }

            if (explicitListenerActor && explicitListenerActor != player && explicitListenerActor != npc) {
                const SpatialAwareness::Result spatialGate =
                    SpatialAwareness::Evaluate(agent->getActor(), explicitListenerActor);
                if (!spatialGate.canCommunicate) {
                    logger::info(
                        "[SpeakManager] Skipping NPC->NPC playback: {} -> {} (reason={}, dist={:.1f}, pathRatio={:.2f}, closedDoors={})",
                        agent->getActorName(), explicitListenerAgent->getActorName(), spatialGate.reason,
                        spatialGate.airDistance, spatialGate.pathRatio, spatialGate.closedDoorCount);
                    dequeueFirstItem();
                    releasePendingPlayerSubtitle();
                    clearVisibleSubtitles();
                    clearCurrentPlayback();
                    setProcessing(false);
                    return;
                }
            }
        }

        int res = 0;

        if (!SM::trim(scriptLine.expression).empty() && !agent->isNarrator()) {
            int expression = findExpression(SM::trim(scriptLine.expression));
            if (expression > -1) {
                auto fgen = npc->GetFaceGenAnimationData();
                if (fgen) {
                    logger::debug("[SPEAKERMANAGER {}] Attempting to acquire FaceGen lock for expression override",tid);
                    RE::BSSpinLockGuard locker(fgen->lock);
                    logger::debug("[SPEAKERMANAGER {}] FaceGen lock acquired for expression override",tid);
                    logger::info("Expression set to {}", SM::trim(scriptLine.expression));
                    fgen->ClearExpressionOverride();
                    fgen->SetExpressionOverride(expression, 0.9f);
                    fgen->exprOverride = true;
                    logger::debug("[SPEAKERMANAGER {}] FaceGen lock will be released",tid);
                } else {
                    logger::warn("[SPEAKERMANAGER {}] Failed to get FaceGen animation data for expression override",tid);
                }
            } else {
                logger::info("Failed to get expression: {}", SM::trim(scriptLine.expression));
            }
            
        }


        if (!SM::trim(scriptLine.subtitle).empty()) {
            RE::SubtitleInfo toSay;

            if (scriptLine.actor != agent->getActorName()) {  // Character change
                // If the mismatched item is a "Player" line, dequeue it — no NPC agent
                // will ever match "Player", so it blocks the queue head forever.
                if (IsPlayerActorAlias(scriptLine.actor, aiam)) {
                    logger::warn("[SPEAKERMANAGER {}] Dequeuing stuck Player line: '{}'", tid, scriptLine.subtitle);
                    dequeueFirstItem();
                }
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    currentPlaybackUtteranceId.clear();
                    currentPlaybackActor.clear();
                    currentPlaybackUtteranceConfirmed = false;
                }
                setProcessing(false);
                endDialogue(npc, "");
                return;
            }
            if (agent->isExternalLocked()) {
                logger::info("[SPEAKERMANAGER {}] {} is locked, cannot speak ", tid, agent->getActorName());
                deleteQueue(false);
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    currentPlaybackUtteranceId.clear();
                    currentPlaybackActor.clear();
                    currentPlaybackUtteranceConfirmed = false;
                }
                setProcessing(false);
                endDialogue(npc, "");

                return;
            }

            if (agent) {
                agent->setTalking(true);

                // npcAgent->getActor()->Decapitate();
                if (!agent->isNarrator()) {
                    npc->GetActorBase()->voiceType = NullVoiceType->As<RE::BGSVoiceType>();
                }
            }

            auto* sm = RE::SubtitleManager::GetSingleton();
            toSay.forceDisplay = true;
            toSay.targetDistance = 10;
            if (!agent->isNarrator()) {
                toSay.speaker = npc->GetHandle();
            } else {
                RE::PlayerCharacter::GetSingleton()->SetDisplayName(NARRATOR_NAME, true);
                toSay.speaker = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
            }

            releasePendingPlayerSubtitle();

            toSay.subtitle = scriptLine.subtitle;
            toSay.pad04 = 0xabcd;
            sm->KillSubtitles();
            sm->subtitles.push_back(toSay);

            hasTalked = true;

            logger::info("[SPEAKERMANAGER {}] Parsing scriptline actor:{} listener:{} text:{}", tid, scriptLine.actor,
                         scriptLine.action, scriptLine.subtitle);

            std::string speechListener = "";

            if (!scriptLine.action.empty()) {
                auto currentActor = aiam.getAgentByName(scriptLine.action);  // get listener

                if (currentActor && !agent->isNarrator()) {
                    auto position = currentActor->getActor()->GetPosition();

                    std::string animation = scriptLine.animation;

                    int animation_n = 0;
                    /* if (!animation.empty())
                        animation_n = std::stoi(animation, nullptr, 16);

                    if (!npcAgent->isAvailableforAnimation()) animation_n = 0;
                    */

                    int movehead = 1;
                    if (npc->GetActorRuntimeData().currentProcess->GetHeadtrackTarget())
                        if (npc->GetActorRuntimeData().currentProcess->GetHeadtrackTarget().get()->GetFormID() ==
                            currentActor->getActor()->GetFormID()) {
                            movehead = 0;
                        }

                    logger::info("{} must look to {}, animation '{}', move head {}", npc->GetDisplayFullName(),
                                 currentActor->getActorName(), animation, movehead);

                    speechListener.assign(currentActor->getActorName());

                    auto localActor = currentActor->getActorByFormId();
                    if (localActor) {
                        logger::info("FakeDialogueWith '{}' '{}' '{}' '{}'", npc->GetDisplayFullName(),
                                     currentActor->getActorName(), animation_n, movehead);

                        auto localNpc = npc;
                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                        auto args = RE::MakeFunctionArguments(std::move(localNpc), std::move(localActor),
                                                              std::move(animation_n), std::move(movehead));

                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                            "AIAgentAIMind", "FakeDialogueWith", args, callback);
                    } else {
                        logger::warn("FakeDialogueWith failed, localActor is null for {}",
                                     currentActor->getActorName());
                    }

                    logger::info("Dialogue Preparation end");

                    if (!agent->isNarrator()) {
                        agent->setClean(false);
                        agent->setRestored(false);
                    }

                } else {
                    if (!agent->isNarrator()) {
                        auto position = RE::PlayerCharacter::GetSingleton()->GetPosition();
                        int movehead = 1;

                        if (npc->GetActorRuntimeData().currentProcess->GetHeadtrackTarget())
                            if (npc->GetActorRuntimeData().currentProcess->GetHeadtrackTarget().get()->GetFormID() ==
                                RE::PlayerCharacter::GetSingleton()->GetFormID()) {
                                movehead = 0;
                            }

                        std::string animation = scriptLine.animation;
                        int animation_n = 0;
                        /* if (!animation.empty())
                            animation_n = std::stoi(animation, nullptr, 16);

                        if (!npcAgent->isAvailableforAnimation()) animation_n = 0;
                        */

                        /*
                        // Check here if is talking to player. agent will be empty
                        if (!agent->isNarrator()) {
                            HTTPManager::log(std::format("infoaction|{}|{}|{} could not hear {}'s speech",
                                                         getCurrentTimeMillis(), GetGameTimeStamp(), scriptLine.action,
                                                         npc->GetDisplayFullName()));
                        }
                        */
                        logger::info("{} must look to (default) {},animation {}", npc->GetDisplayFullName(),
                                     RE::PlayerCharacter::GetSingleton()->GetDisplayFullName(), animation, movehead);

                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                        auto args =
                            RE::MakeFunctionArguments(std::move(npc), std::move(animation_n), std::move(movehead));

                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                            "AIAgentAIMind", "FakeDialogue", args, callback);

                        /*
                        using HeadTrackType = RE::HighProcessData::HEAD_TRACK_TYPE;
                        auto process = npc->GetActorRuntimeData().currentProcess;
                        if (const auto highProcess = process->high) {
                                highProcess->SetHeadtrackTarget(HeadTrackType::kDialogue, player->AsReference());
                        }
                        */
                    }

                    if (!agent->isNarrator()) {
                        agent->setClean(false);
                        agent->setRestored(false);
                    }
                }
            } else {
                // BVogus
                int animation_n = 0;
                int movehead = 0;
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();

                auto args = RE::MakeFunctionArguments(std::move(npc), std::move(animation_n), std::move(movehead));

                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "FakeDialogue", args, callback);
            }

            if (!SM::trim(scriptLine.animation).empty()) {
                if (agent->isAvailableforAnimation() && !agent->isNarrator()) {
                    if (commandAnimation(SM::trim(scriptLine.animation), npc)) agent->setAnimationBusy(true);
                    ;
                }
            }

            // Reset current speech. Too agressive.
            // agent->getActor()->PauseCurrentDialogue();
            // agent->getActor()->GetActorBase()->voiceType = NullVoiceType->As<RE::BGSVoiceType>();
            // agent->getActor()->SetSpeakingDone(true);

            // Start playback

            SPGResponse& spgResponse = SPGResponse::getInstance();
            bool earlyRechat = false;
            std::string phoneticTrimmed = SM::trim(scriptLine.phonetic);
            bool unfinished = spgResponse.isUnfinished();
            if (GlobalRechatPolicyAsap == 0) {
                if (countItems() == 1 && !unfinished) {
                    // Last item in queue and SGPQueue is finished
                    std::string rechatListenerHint = ResolveScriptLineListenerHint(scriptLine);
                    std::string rechatTargetHint = ResolveScriptLineRechatTargetHint(scriptLine);

                    /* But, maybe listener is different in the last line. Can happen when scriptlines are queued,
                    and they come from different generation, like for example, return a function call */

                    auto lastLine = getLastItem();

                    if (!lastLine.action.empty()) {
                        rechatListenerHint = ResolveScriptLineListenerHint(lastLine);
                    }
                    if (!SM::trim(lastLine.rechatTargetHint).empty()) {
                        rechatTargetHint = ResolveScriptLineRechatTargetHint(lastLine);
                    }
                    if (scriptLine.duration > 0) {
                        //Musicmanager uses forced duration
                        logger::info("[EARLY RECHAT {}] Disabled by duration", tid, scriptLine.duration);
                        unfinished = true;
                    } else {
                        const bool sameSpeakerAsLastRechatter = (agent->getActorName() == getLastRechatter());
                        const bool sameSpeakerRechatInFlight = isRechatInFlightFor(agent->getActorName());
                        if (!sameSpeakerAsLastRechatter && !sameSpeakerRechatInFlight) {
                            logger::info("[EARLY RECHAT {}] LAUNCH Response queue has 1 items and is finished.", tid);
                            if (rechat(agent->getActorName(), rechatListenerHint, 0, scriptLine.subtitle,
                                       rechatTargetHint) > 0) {
                                earlyRechat = true;
                                beginRechatAttempt(agent->getActorName());
                            }
                        } else if (sameSpeakerRechatInFlight) {
                            logger::info("[EARLY RECHAT {}] AVOIDED because rechat is already in flight for {}.", tid,
                                         agent->getActorName());
                        } else {
                            logger::info("[EARLY RECHAT {}] AVOIDED because lastRechatter is same.", tid);
                        }
                    }
                }
            }
            // Playback init
            float playbackVolumeBoost = scriptLine.volumeBoost;
            bool applyMuffleFilter = false;

            // Dynamic attenuation/muffle is handled during playback in DownloadAndPlay so
            // door/LOS/navmesh changes can update while the line is still playing.

            const bool whisperModeActive = PrismaUIBridge::GetCurrentChatboxMode() == "WHISPER";
            const bool directedToPlayer = IsDirectlyAddressingPlayer(scriptLine.action, aiam);
            if (whisperModeActive && directedToPlayer && !agent->isNarrator()) {
                playbackVolumeBoost = 0.25f;
                logger::info(
                    "[SpeakManager] Whisper mode: forcing 25% NPC playback volume for {} (direct response to player '{}')",
                    agent->getActorName(), scriptLine.action);
            }

            res = DownloadAndPlay(scriptLine.subtitle, preClip, postClip, agent->getActorName(), phoneticTrimmed,
                                  playbackVolumeBoost, scriptLine.duration, applyMuffleFilter);

            const bool playbackAborted = (res == 2);
            if (playbackAborted) {
                dropMatchingHeadItem(scriptLine);
            } else {
                dequeueFirstItem();
            }

            if (res == 5) {  // Audio played, but returned this value to not trigger rechat
                ;
            } else {
                // New policy
                bool playerInDialog = false;
                if (RE::MenuTopicManager::GetSingleton()->unkB1) {
                    playerInDialog = true;
                }

                logger::info("[RECHAT {}] Rechat evaluation", tid);
                if (playerInDialog) {
                    logger::info("[RECHAT {}] Avoiding rechat, player is in dialog", tid);
                } else if (GlobalRechatPolicyAsap == 0 && res != 2 && earlyRechat == false) {
                    bool unfinished = spgResponse.isUnfinished();

                    if (countItems() == 1 && !unfinished) {
                        // Only one item pending. Lets launch rechat event here

                        std::string rechatListenerHint = ResolveScriptLineListenerHint(scriptLine);
                        std::string rechatTargetHint = ResolveScriptLineRechatTargetHint(scriptLine);

                        /* But, maybe listener is different in the last line. Can happen when scriptlines are queued,
                        and they come from different generation, like for example, return a function call */

                        auto lastLine = getLastItem();

                        if (!lastLine.action.empty()) {
                            rechatListenerHint = ResolveScriptLineListenerHint(lastLine);
                        }
                        if (!SM::trim(lastLine.rechatTargetHint).empty()) {
                            rechatTargetHint = ResolveScriptLineRechatTargetHint(lastLine);
                        }

                        const bool sameSpeakerAsLastRechatter = (agent->getActorName() == getLastRechatter());
                        const bool sameSpeakerRechatInFlight = isRechatInFlightFor(agent->getActorName());
                        if (isRechatChainClosed()) {
                        } else if (!sameSpeakerAsLastRechatter && !sameSpeakerRechatInFlight) {
                            logger::info("[RECHAT {}] LAUNCH Response queue has 1 items and is finished.", tid);
                            if (rechat(agent->getActorName(), rechatListenerHint, 0, scriptLine.subtitle,
                                       rechatTargetHint) > 0) {
                                beginRechatAttempt(agent->getActorName());
                            }
                        } else if (sameSpeakerRechatInFlight) {
                            logger::info("[RECHAT {}] Deferred retry for {} until the in-flight rechat finishes.", tid,
                                         agent->getActorName());
                            queueRechatRetry(agent->getActorName(), rechatListenerHint, rechatTargetHint,
                                             scriptLine.subtitle, 0);
                        } else {
                            logger::info("[RECHAT {}] AVOIDED because lastRechatter is same.", tid);
                        }

                    } else if (countItems() == 0 && !unfinished &&
                               getLastRechatter() != agent->getActorName() &&
                               !isRechatInFlightFor(agent->getActorName())) {  // One liners
                        // Last item item pending. Lets launch rechat event here
                        if (isRechatChainClosed()) {
                            setLastRechatter("");
                            resetRechatChainState();
                        } else {
                            logger::info("[RECHAT {}] LAUNCH Response queue has 0 items and is finished. ", tid);
                            if (rechat(agent->getActorName(), ResolveScriptLineListenerHint(scriptLine), 0,
                                       scriptLine.subtitle, ResolveScriptLineRechatTargetHint(scriptLine)) > 0) {
                                beginRechatAttempt(agent->getActorName());
                            }
                        }
                    } else if (countItems() == 0 && !unfinished &&
                               isRechatInFlightFor(agent->getActorName())) {
                        logger::info("[RECHAT {}] Deferred retry for {} until the in-flight rechat finishes.", tid,
                                     agent->getActorName());
                        queueRechatRetry(agent->getActorName(), ResolveScriptLineListenerHint(scriptLine),
                                         ResolveScriptLineRechatTargetHint(scriptLine), scriptLine.subtitle, 0);
                    } else {
                        const int queueItems = countItems();
                        const bool keepChainState = unfinished || queueItems > 0 ||
                                                    isRechatInFlightFor(agent->getActorName());
                        logger::info("[RECHAT {}] NO RECHAT! Items in queue {}, unfinished {}, last rechatter {}", tid,
                                     queueItems, unfinished, getLastRechatter());
                        if (!keepChainState) {
                            setLastRechatter("");
                            resetRechatChainState();
                        }
                    }
                } else {
                    if (GlobalRechatPolicyAsap == 0) {
                        const int queueItems = countItems();
                        const bool keepChainState = earlyRechat || queueItems > 0 ||
                                                    isRechatInFlightFor(agent->getActorName());
                        logger::info("[RECHAT {}] NO RECHAT! Last DownloadAndPlay return value was {},earlyRechat {} ",
                                     tid, res, earlyRechat ? 1 : 0);
                        if (!keepChainState) {
                            setLastRechatter("");
                            resetRechatChainState();
                        }
                    } else {
                        logger::info("[RECHAT {}] NO RECHAT! Using ASAP policy", tid, res);
                    }
                }
            }
            auto npc = agent->getActorName();
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            std::string textCopy(scriptLine.subtitle);

            auto args = RE::MakeFunctionArguments(std::move(npc), std::move(textCopy));

            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", "SendExternalEventChat", args, callback);

            // Log
            // Moved to be logged on server
            /* HTTPManager::log(std::format("chat|{}|{}|(Context location: {}){}: {}", getCurrentTimeMillis(),
                                                        GetGameTimeStamp(), GetPlayerLocation(),
                                                        npc->GetDisplayFullName(), toSay.subtitle.c_str()));*/

            try {
                json sData;
                const std::string speakerName = agent->getActorName();
                const bool speakerIsNarrator = agent->isNarrator() || speakerName == NARRATOR_NAME;
                sData["speaker"] = speakerName;
                sData["location"] = GetPlayerLocation();
                sData["speech"] = scriptLine.subtitle;
                sData["utterance_id"] = scriptLine.utteranceId;
                const std::string resolvedListenerName =
                    speechListener.empty() ? RE::PlayerCharacter::GetSingleton()->GetName() : speechListener;
                sData["listener"] = resolvedListenerName;
                std::vector<std::string> audibleCompanions;
                json spatialAudibility = json::array();

                // Calculate distance and v1-lite spatial context from speaker to listener.
                float distance = 0.0f;
                bool hasSpatialContext = false;
                SpatialAwareness::Result spatialResult{};
                auto player = RE::PlayerCharacter::GetSingleton();
                RE::Actor* listenerActor = nullptr;
                const auto iequals = [](const std::string& left, const std::string& right) {
                    if (left.size() != right.size()) {
                        return false;
                    }

                    for (std::size_t i = 0; i < left.size(); ++i) {
                        if (std::tolower(static_cast<unsigned char>(left[i])) !=
                            std::tolower(static_cast<unsigned char>(right[i]))) {
                            return false;
                        }
                    }
                    return true;
                };
                const auto normalizeName = [](std::string value) {
                    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    return value;
                };
                const auto resolveActorByName = [&](const std::string& name) -> RE::Actor* {
                    if (name.empty()) {
                        return nullptr;
                    }

                    auto directAgent = aiam.getAgentByName(name);
                    if (directAgent && directAgent->getActor()) {
                        return directAgent->getActor();
                    }

                    for (const auto& namedAgent : aiam.getAgents()) {
                        if (!namedAgent || !namedAgent->getActor()) {
                            continue;
                        }
                        if (iequals(namedAgent->getActorName(), name)) {
                            return namedAgent->getActor();
                        }
                    }

                    return nullptr;
                };
                const std::string playerName = player && player->GetName() ? player->GetName() : "";
                const std::string playerDisplayName = player && player->GetDisplayFullName() ? player->GetDisplayFullName() : "";
                const std::string configuredPlayerName = aiam.getPlayerName();

                if (!speechListener.empty()) {
                    listenerActor = resolveActorByName(speechListener);
                    if (!listenerActor &&
                        (iequals(speechListener, playerName) || iequals(speechListener, playerDisplayName) ||
                         (!configuredPlayerName.empty() && iequals(speechListener, configuredPlayerName)))) {
                        listenerActor = player;
                    }
                } else {
                    listenerActor = player;
                }

                if (!speakerIsNarrator && agent->getActor() && listenerActor) {
                    spatialResult = SpatialAwareness::Evaluate(agent->getActor(), listenerActor);
                    hasSpatialContext = true;
                    distance = spatialResult.airDistance;
                }

                if (!speakerIsNarrator) {
                    // Authoritative audience scope is evaluated from the speaking actor.
                    RE::Actor* audibilitySource = agent->getActor();
                    if (!audibilitySource) {
                        audibilitySource = listenerActor ? listenerActor : RE::PlayerCharacter::GetSingleton();
                    }

                    // Keep audience stable for multi-line NPC responses.
                    const std::string audienceSnapshotKey =
                        normalizeName(speakerName) + "->" + normalizeName(resolvedListenerName);
                    bool reusedAudienceSnapshot = false;
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        if (audienceSnapshotReady && audienceSnapshotKey == this->audienceSnapshotKey &&
                            !audienceSnapshotCompanions.empty()) {
                            audibleCompanions = audienceSnapshotCompanions;
                            reusedAudienceSnapshot = true;
                        }
                    }

                    if (!reusedAudienceSnapshot) {
                        // Audience scope is speech audibility, not auto-activate population.
                        // Auto-activate can keep broader scene agents alive, but NPC speech fanout
                        // should use the MCM spatial hearing distances before running Evaluate().
                        const auto spatialSettings = SpatialAwareness::GetSettings();
                        float audienceMaxDistance = spatialSettings.exteriorMaxDistance;
                        if (audibilitySource) {
                            auto* sourceCell = audibilitySource->GetParentCell();
                            if (sourceCell && sourceCell->IsInteriorCell()) {
                                audienceMaxDistance = spatialSettings.interiorMaxDistance;
                            }
                        }
                        if (spatialSettings.maxAirDistance > 0.0f) {
                            audienceMaxDistance = std::min(audienceMaxDistance, spatialSettings.maxAirDistance);
                        }

                        struct AudienceCandidate {
                            std::shared_ptr<AIAgent> agent;
                            RE::Actor* actor = nullptr;
                            float distance = 0.0f;
                        };

                        std::vector<AudienceCandidate> audienceCandidates;
                        audienceCandidates.reserve(8);

                        // Build companions by cheap distance first; the MCM hearing radius is the hard fanout guard.
                        for (const auto& candidateAgent : aiam.getAgents()) {
                            if (!candidateAgent) {
                                continue;
                            }

                            const std::string candidateName = candidateAgent->getActorName();
                            if (candidateName.empty() || candidateName == NARRATOR_NAME) {
                                continue;
                            }

                            auto* candidateActor = candidateAgent->getActor();
                            if (!candidateActor || candidateActor->IsDead()) {
                                continue;
                            }
                            if (audibilitySource && candidateActor->GetFormID() == audibilitySource->GetFormID()) {
                                continue;
                            }

                            float candidateDistance = 0.0f;
                            if (audibilitySource) {
                                candidateDistance = audibilitySource->GetPosition().GetDistance(candidateActor->GetPosition());
                                if (candidateDistance > audienceMaxDistance) {
                                    continue;
                                }
                            }

                            audienceCandidates.push_back(AudienceCandidate{candidateAgent, candidateActor, candidateDistance});
                        }

                        std::sort(audienceCandidates.begin(), audienceCandidates.end(),
                                  [](const auto& lhs, const auto& rhs) {
                                      return lhs.distance < rhs.distance;
                                  });

                        std::size_t audienceEvaluations = 0;
                        const auto audienceEvaluateStartedAt = std::chrono::steady_clock::now();
                        for (const auto& candidate : audienceCandidates) {
                            ++audienceEvaluations;

                            const auto& candidateAgent = candidate.agent;
                            auto* candidateActor = candidate.actor;
                            const std::string candidateName = candidateAgent->getActorName();

                            SpatialAwareness::Result candidateSpatial =
                                SpatialAwareness::Evaluate(audibilitySource, candidateActor);

                            json candidateDebug;
                            candidateDebug["name"] = candidateName;
                            candidateDebug["can_communicate"] = candidateSpatial.canCommunicate;
                            candidateDebug["volume"] = candidateSpatial.volume;
                            candidateDebug["reason"] = candidateSpatial.reason;
                            candidateDebug["distance"] = candidateSpatial.airDistance;
                            spatialAudibility.push_back(candidateDebug);

                            if (candidateSpatial.canCommunicate &&
                                std::find(audibleCompanions.begin(), audibleCompanions.end(), candidateName) ==
                                    audibleCompanions.end()) {
                                audibleCompanions.push_back(candidateName);
                            }
                        }

                        if (!audienceCandidates.empty()) {
                            const auto audienceEvaluateMs =
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - audienceEvaluateStartedAt).count();
                            logger::debug(
                                "[SpeakManager] NPC audience snapshot evaluated {}/{} in {:.2f}ms, audible={}",
                                audienceEvaluations, audienceCandidates.size(), audienceEvaluateMs,
                                audibleCompanions.size());
                        }

                        {
                            std::lock_guard<std::mutex> lock(mtx);
                            this->audienceSnapshotKey = audienceSnapshotKey;
                            audienceSnapshotCompanions = audibleCompanions;
                            audienceSnapshotReady = !audienceSnapshotCompanions.empty();
                        }
                    }

                    if (!speakerName.empty() &&
                        std::find(audibleCompanions.begin(), audibleCompanions.end(), speakerName) ==
                            audibleCompanions.end()) {
                        audibleCompanions.push_back(speakerName);
                    }

                    // Preserve the intended listener only when spatial still allows it.
                    // Otherwise far/blocked NPCs leak back into the rechat chain even
                    // after the audible audience snapshot filtered them out.
                    const bool listenerIsPlayer =
                        listenerActor && player && listenerActor->GetFormID() == player->GetFormID();
                    const bool listenerSpatiallyReachable =
                        listenerIsPlayer || (hasSpatialContext && spatialResult.canCommunicate);
                    if (!speechListener.empty()) {
                        if (listenerSpatiallyReachable &&
                            std::find(audibleCompanions.begin(), audibleCompanions.end(), speechListener) ==
                                audibleCompanions.end()) {
                            audibleCompanions.push_back(speechListener);
                        } else if (!listenerSpatiallyReachable) {
                            logger::info(
                                "[SpeakManager] Not preserving rechat listener '{}' for {}: spatial_reason={} distance={:.1f}",
                                speechListener, agent->getActorName(),
                                hasSpatialContext ? spatialResult.reason : "no_spatial_context",
                                hasSpatialContext ? spatialResult.airDistance : 0.0f);
                        }
                    }
                }

                sData["companions"] = audibleCompanions;
                sData["distance"] = distance;
                sData["spatial_can_communicate"] = hasSpatialContext ? spatialResult.canCommunicate : false;
                sData["spatial_volume"] = hasSpatialContext ? spatialResult.volume : 0.0f;
                sData["spatial_reason"] = speakerIsNarrator ? "narrator" :
                    (hasSpatialContext ? spatialResult.reason : "no_listener_context");
                sData["spatial_audibility"] = spatialAudibility;

                {
                    std::lock_guard<std::mutex> lock(mtx);
                    if (currentPlaybackUtteranceId == scriptLine.utteranceId) {
                        currentPlaybackUtteranceConfirmed = !scriptLine.utteranceId.empty();
                    }
                }

                HTTPManager::log(
                    std::format("_speech|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), sData.dump()));
            } catch (nlohmann::json_abi_v3_11_2::detail::type_error* exception) {
                logger::info("Error sending speech. Review encoding");
            }
            
        }
        

        {
            std::lock_guard<std::mutex> lock(mtx);
            if (currentPlaybackUtteranceId == scriptLine.utteranceId) {
                currentPlaybackUtteranceId.clear();
                currentPlaybackActor.clear();
                currentPlaybackUtteranceConfirmed = false;
            }
        }

        setProcessing(false);
        if (hasTalked) {
            ExtendPostSpeechMaintenanceSuppress(std::chrono::seconds(15));
        }

        // Narrator cleanup MUST run before checking for more queue items.
        // If the queue has items from other actors, the recursive process() call
        // hits the actor mismatch at line 1789 and returns early — never reaching
        // this cleanup code. That leaves the Player permanently named "The Narrator"
        // and corrupts the actor state, freezing the game.
        if (agent->isNarrator() && hasTalked) {
            auto originalName = aiam.getPlayerName();
            if (originalName == "Prisoner") {
                originalName = RE::PlayerCharacter::GetSingleton()->GetName();
                aiam.setPlayerName(originalName);
            }
            RE::PlayerCharacter::GetSingleton()->SetDisplayName(originalName.c_str(), true);
            clearVisibleSubtitles();
            logger::info("Narrator cleanup: restored player name to '{}', subtitles cleared", originalName);
        }

        if (hasItems()) {  // More items in queue, so keep processing.
            if (res != 2) {
                process(agent);
            }
        } else {
            {
                std::lock_guard<std::mutex> lock(mtx);
                audienceSnapshotKey.clear();
                audienceSnapshotCompanions.clear();
                audienceSnapshotReady = false;
            }

            if (hasTalked) {

                // End of dialogue here.
                setLastUsedTime();
                if (!agent->isNarrator())
                    endDialogue(npc, SM::trim(scriptLine.subtitle) );
                // Narrator cleanup already handled above
            }

        }
    }
}


void SpeakManager::processPlayer() {
    bool hasTalked = false;
    int res = 0;
    std::function<void(const ScriptLine&, int)> playerPlaybackCompletedCallbackCopy;
    if (isProcessing) {
        logger::info("SpeakManager is busy");
        return;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    
    if (!isProcessing && !scriptQueue.empty()) {
        setProcessing(true);
        isAborted();// Reset flag
        logger::info("SpeakManager is processing now for Player");
        ScriptLine scriptLine = scriptQueue.front();

        const std::string trimmedSubtitle = SM::trim(scriptLine.subtitle);
        const std::string playbackPhonetic = SM::trim(scriptLine.phonetic);
        const bool isMenuPlayerTtsLine = scriptLine.action == "__player_menu_tts";
        const bool isTextOnlyPlayerLine = scriptLine.action == "__player_text_only";

        if (!trimmedSubtitle.empty() || !playbackPhonetic.empty()) {
            hasTalked = true;

            if (!isMenuPlayerTtsLine && !trimmedSubtitle.empty()) {
                setPendingPlayerSubtitle(trimmedSubtitle);
                refreshPendingPlayerSubtitle(true);
            }

            if (isTextOnlyPlayerLine) {
                res = HoldTextOnlyPlayerSubtitle(*this, scriptLine);
            } else {
                res = DownloadAndPlay(trimmedSubtitle, preClip, postClip, "Player", playbackPhonetic, 1.0f);
            }
        }

        dequeueFirstItem();
        setProcessing(false);
        if (isTextOnlyPlayerLine) {
            // Text-only STT captions are a short echo of what the player said.
            // If no NPC/Narrator subtitle replaces them, release the pending state so
            // clearVisibleSubtitles() can remove the echo instead of refreshing it forever.
            releasePendingPlayerSubtitle();
            clearVisibleSubtitles();
        }
        if (hasTalked) {
            ExtendPostSpeechMaintenanceSuppress(std::chrono::seconds(15));
        }

        AIAgentManager& aiam = AIAgentManager::getInstance();
        auto originalName = aiam.getPlayerName();
        if (originalName == "Prisoner") {
            originalName = RE::PlayerCharacter::GetSingleton()->GetName();
            aiam.setPlayerName(originalName);
        }

        player->SetDisplayName(originalName.c_str(), true);

        auto fgen = player->GetFaceGenAnimationData();

        if (fgen && res!=3) {  // Player actually talked

            RE::BSSpinLockGuard locker(fgen->lock);

            for (int i = 0; i <= 15; i++) {
                if (i == 7)
                    fgen->phenomeKeyFrame.SetValue(i, 0.20);  // Default mouth position?
                else
                    fgen->phenomeKeyFrame.SetValue(i, 0);
            }
            //
            // fgen->ClearExpressionOverride();
            // fgen->Reset(0.0f, true, true, true, false);
            //

           // player->GetActorRuntimeData().currentProcess->Update3DModel(player);
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            playerPlaybackCompletedCallbackCopy = std::move(playerPlaybackCompletedCallback);
            playerPlaybackCompletedCallback = nullptr;
        }

        if (playerPlaybackCompletedCallbackCopy) {
            playerPlaybackCompletedCallbackCopy(scriptLine, res);
        }

    }
}

void SpeakManager::setPendingPlayerSubtitle(const std::string& subtitleText) {
    const auto trimmedSubtitle = SM::trim(subtitleText);
    if (trimmedSubtitle.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mtx);
    pendingPlayerSubtitleText = trimmedSubtitle;
    pendingPlayerSubtitleActive = true;
    pendingPlayerSubtitleLastRefresh = std::chrono::high_resolution_clock::time_point{};
}

void SpeakManager::releasePendingPlayerSubtitle() {
    std::lock_guard<std::mutex> lock(mtx);
    pendingPlayerSubtitleActive = false;
    pendingPlayerSubtitleText.clear();
    pendingPlayerSubtitleLastRefresh = std::chrono::high_resolution_clock::time_point{};
}

void SpeakManager::refreshPendingPlayerSubtitle(bool force) {
    std::string subtitleText;
    auto now = std::chrono::high_resolution_clock::now();

    {
        std::lock_guard<std::mutex> lock(mtx);
        if (!pendingPlayerSubtitleActive || pendingPlayerSubtitleText.empty()) {
            return;
        }

        if (!force && pendingPlayerSubtitleLastRefresh.time_since_epoch().count() != 0) {
            const auto elapsed = now - pendingPlayerSubtitleLastRefresh;
            if (elapsed < std::chrono::milliseconds(250)) {
                return;
            }
        }

        subtitleText = pendingPlayerSubtitleText;
        pendingPlayerSubtitleLastRefresh = now;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* subtitleManager = RE::SubtitleManager::GetSingleton();
    if (!player || !subtitleManager) {
        return;
    }

    PushForcedPlayerSubtitle(subtitleManager, player, subtitleText);
}

void SpeakManager::clearVisibleSubtitles() {
    refreshPendingPlayerSubtitle(true);

    {
        std::lock_guard<std::mutex> lock(mtx);
        if (pendingPlayerSubtitleActive) {
            return;
        }
    }

    auto* subtitleManager = RE::SubtitleManager::GetSingleton();
    if (!subtitleManager) {
        return;
    }

    subtitleManager->KillSubtitles();
    subtitleManager->subtitles.clear();
}

void SpeakManager::endDialogue(RE::Actor* npc, std::string lastline) {
    logger::info("Face reset for {}", npc->GetDisplayFullName());

    auto fgen = npc->GetFaceGenAnimationData();
    if (fgen) {
        logger::debug("[SpeakManager] Attempting to acquire FaceGen lock for dialogue end");
        RE::BSSpinLockGuard locker(fgen->lock);
        logger::debug("[SpeakManager] FaceGen lock acquired for dialogue end");

        for (int i = 0; i <= 15; i++) {
            if (i == 7)
                fgen->phenomeKeyFrame.SetValue(i, 0.20);  // Default mouth position?
            else
                fgen->phenomeKeyFrame.SetValue(i, 0);
        }
        logger::debug("[SpeakManager] FaceGen lock will be released");
    } else {
        logger::warn("[SpeakManager] Failed to get FaceGen animation data for dialogue end");
    }

    npc->AllowPCDialogue(true);
    //npc->SetSpeakingDone(true); //1.0.13
    clearVisibleSubtitles();

    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
    auto args = RE::MakeFunctionArguments(std::move(npc));

    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "EndDialogue",
                                                                               args, callback);

    AIAgentManager& aiam = AIAgentManager::getInstance();
    auto npcAgent = aiam.getAgentByName(npc->GetDisplayFullName());
    if (npcAgent) {
        npcAgent->setAnimationBusy(false);
        npcAgent->setTalking(false);
        npcAgent->setClean(false);
        npcAgent->setRestored(false);
        npcAgent->SetLastTimeTalk();
        logger::info("Check if needs animation cleaning{} {}", npcAgent->getActorName(),npcAgent->getCurrentAnimation());

        if ((npcAgent->getCurrentAnimation() == "IdleApplaudSarcastic") ||
            (npcAgent->getCurrentAnimation() == "IdleCiceroAgitated") ||
            (npcAgent->getCurrentAnimation() == "SpectatorCheer")) {
            
            commandAnimation("IdleStop", npcAgent->getActor());
        }
    }

    // If player talked
    /*
    if (RE::PlayerCharacter::GetSingleton()->GetFormID() == npc->GetFormID()) {
        HTTPManager::stream(std::format("{}|{}|{}|{}:{}", "chatsimfollow", getCurrentTimeMillis(), GetGameTimeStamp(),
                                        RE::PlayerCharacter::GetSingleton()->GetName(), lastline));
    }
    */
    logger::info("Speaker Manager Stopped");
}

/*
bool SpeakManager::downloadFakeNoteOld(std::string name) {
    
        HINTERNET hInternet = InternetOpen(L"CHIM AIagent, image downloader", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (hInternet == NULL) {
            logger::info("Failed to open internet session:  {}", GetLastError());

    return false;
}


        auto server = Conf::getInstance().getServer();
        auto port = Conf::getInstance().getPort();
        // Open the HTTP connection
        HINTERNET hConnection = InternetConnect(hInternet, StringToWideString(server), std::stoi(port), NULL, NULL,
                                                INTERNET_SERVICE_HTTP, 0, 0);

        if (hConnection == NULL) {
            logger::info("Failed to open HTTP connection:  {}", GetLastError());
            InternetCloseHandle(hInternet);
            return 1;
        }

        std::string path = Conf::getInstance().getPath();
        std::filesystem::path fullPath = path;
        std::filesystem::path dirName = fullPath.parent_path();
        path.assign(fullPath.parent_path().string());
        std::string hashedName = md5(trim(name));
        std::string fullPathFile = path.append("/soundcache/" + hashedName + ".png").c_str();

        // Create the request
        HINTERNET hRequest = HttpOpenRequest(hConnection, L"GET", StringToWideString(fullPathFile), NULL, NULL, NULL,
                                             INTERNET_FLAG_RELOAD, 0);
        if (hRequest == NULL) {
            logger::info("Failed to create HTTP request:  {}", GetLastError());
            InternetCloseHandle(hConnection);
            InternetCloseHandle(hInternet);
            return 1;
        }

        // Send the request
        if (!HttpSendRequest(hRequest, NULL, 0, NULL, 0)) {
            logger::info("Failed to send HTTP request:  {}", GetLastError());
            InternetCloseHandle(hRequest);
            InternetCloseHandle(hConnection);
            InternetCloseHandle(hInternet);
            return 1;
        }

        // Read the response
        DWORD contentLength = 0;
        DWORD bufferSize = sizeof(DWORD);

        if (!HttpQueryInfo(hRequest, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &contentLength, &bufferSize,
                           NULL)) {
            logger::info("Failed to get content length:  {}", GetLastError());

            InternetCloseHandle(hRequest);
            InternetCloseHandle(hConnection);
            InternetCloseHandle(hInternet);
            return 1;
        }

        char* buffer = new char[contentLength];
        DWORD bytesRead = 0;
        while (InternetReadFile(hRequest, buffer + bytesRead, contentLength - bytesRead, &bytesRead) && bytesRead != 0)
            ;

        std::string filePath =
            "Data/textures/AIAgent/Books/" + hashedName+".png ";

        // Open the file in binary mode and write the buffer contents to it
        std::ofstream outFile(filePath, std::ios::binary);
        if (outFile.is_open()) {
            outFile.write(buffer, contentLength);
            outFile.close();
        } else {
            // Error handling if the file could not be opened
            logger::info("Error: Could not open the file for writing:  {}", GetLastError());

        }

        // Clean up
        delete[] buffer;

        // Clean up
        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnection);
        InternetCloseHandle(hInternet);
 
        return true;
}

*/

bool SpeakManager::downloadFakeNote(std::string name) {
    // Open a WinHTTP session
    HINTERNET hSession =
        WinHttpOpen(L"CHIM AIagent, image downloader", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!hSession) {
        logger::info("Failed to open WinHTTP session: {}", GetLastError());
        return false;
    }

    // Get server and port from configuration
    auto server = Conf::getInstance().getServer();
    auto port = std::stoi(Conf::getInstance().getPort());
    std::wstring wideServer = StringToWideString(server);

    // Connect to the server
    HINTERNET hConnect = WinHttpConnect(hSession, wideServer.c_str(), port, 0);
    if (!hConnect) {
        logger::info("Failed to connect to server: {}", GetLastError());
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Construct file path and HTTP request path
    std::string path = Conf::getInstance().getPath();
    std::filesystem::path fullPath = path;
    std::filesystem::path dirName = fullPath.parent_path();
    path.assign(fullPath.parent_path().string());
    std::string hashedName = md5low(trim(name),false);
    std::string relativePath = "/data/books/" + hashedName + ".png";
    
    logger::info("Downloading fake note from path: {} {}", path, relativePath);
    path.append(relativePath);
    // Convert path to wide string
    std::wstring widePath = StringToWideString(path);

    // Create the HTTP request
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", widePath.c_str(), NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_REFRESH);
    if (!hRequest) {
        logger::info("Failed to create HTTP request: {}", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Send the request
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        logger::info("Failed to send HTTP request: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Wait for response
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        logger::info("Failed to receive HTTP response: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Query content length
    DWORD contentLength = 0;
    DWORD lengthSize = sizeof(contentLength);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, NULL, &contentLength,
                             &lengthSize, NULL)) {
        logger::info("Failed to query content length: {}", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Allocate buffer for the file content
    char* buffer = new char[contentLength];
    DWORD totalBytesRead = 0;
    DWORD bytesRead = 0;

    // Read the data
    while (WinHttpReadData(hRequest, buffer + totalBytesRead, contentLength - totalBytesRead, &bytesRead) &&
           bytesRead > 0) {
        totalBytesRead += bytesRead;
    }

    if (totalBytesRead != contentLength) {
        logger::info("Incomplete file download: expected {}, got {}", contentLength, totalBytesRead);
        delete[] buffer;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Save the file locally
    std::string filePath = "Data/textures/AIAgent/Books/" + hashedName + ".png";
    std::ofstream outFile(filePath, std::ios::binary);
    if (outFile.is_open()) {
        outFile.write(buffer, contentLength);
        outFile.close();
        logger::info("File saved to {}", filePath);
    } else {
        logger::info("Error: Could not open the file for writing: {}", GetLastError());
        delete[] buffer;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Clean up
    delete[] buffer;
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return true;
}
