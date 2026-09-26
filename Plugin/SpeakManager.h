#ifndef SPEAKMANAGER_H
#define SPEAKMANAGER_H

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "Globals.h"
#include "HeadVoiceVolumeUtils.h"

// Track last event type for narration detection
extern std::string lastEventType;

struct ScriptLine {
    std::string subtitle;
    std::string expression;
    std::string action;  // or listener
    std::string animation;
    std::string actor;
    std::string phonetic;  // text in the Latin alphabet to use with lip sync when using non-Latin languages
    std::string rechatTargetHint;
    std::string utteranceId;
    std::string directorSceneId;
    std::string ttsCacheKey;
    std::uint64_t directorGeneration = 0;
    std::uint64_t interactionGeneration = 0;
    int directorLine = 0;
    bool directorHasActions = false;
    bool rechatGenerated = false;
    float volumeBoost;  // Volume multiplier for shouting (1.0 = normal, 1.3 = 30% louder)
    float duration;     // Duration of the line in seconds, used for timing animations and lip sync

    ScriptLine(const std::string& sub, const std::string& exp, const std::string& act, const std::string& anim,
               const std::string& targetactor, const std::string& ph, float volBoost = 1.0f, float duration = -1,
               const std::string& rechatTarget = "", const std::string& utterance = "")
        : subtitle(sub),
          expression(exp),
          action(act),
          animation(anim),
          actor(targetactor),
          phonetic(ph),
          rechatTargetHint(rechatTarget),
          utteranceId(utterance),
          volumeBoost(volBoost),
          duration(duration) {}

    static ScriptLine parse(const std::string& input, const std::string& actor, char delimiter = '/') {
        logger::debug("Parsing input: {} for actor: {}", input, actor);
        size_t firstDelim = input.find(delimiter);
        if (firstDelim == std::string::npos) {
            logger::debug("First delimiter not found");
            return ScriptLine("", "", "", "", "", "", 1.0f);  // Handle the case when the delimiter is not found
        }

        std::string sub = input.substr(0, firstDelim);
        size_t secondDelim = input.find(delimiter, firstDelim + 1);
        if (secondDelim == std::string::npos) {
            logger::debug("Second delimiter not found");
            return ScriptLine(sub, "", "", "", actor, "",
                              1.0f);  // Handle the case when the second delimiter is not found
        }

        std::string exp = input.substr(firstDelim + 1, secondDelim - firstDelim - 1);
        size_t thirdDelim = input.find(delimiter, secondDelim + 1);
        if (thirdDelim == std::string::npos) {
            logger::debug("Third delimiter not found");
            return ScriptLine(sub, exp, "", "", actor, "",
                              1.0f);  // Handle the case when the third delimiter is not found
        }

        std::string act = input.substr(secondDelim + 1, thirdDelim - secondDelim - 1);
        size_t fourthDelim = input.find(delimiter, thirdDelim + 1);
        if (fourthDelim == std::string::npos) {
            logger::debug("Fourth delimiter not found");
            return ScriptLine(sub, exp, act, "", actor, "",
                              1.0f);  // Handle the case when the fourth delimiter is not found
        }

        std::string anim = input.substr(thirdDelim + 1, fourthDelim - thirdDelim - 1);
        size_t fifthDelim = input.find(delimiter, fourthDelim + 1);
        if (fifthDelim == std::string::npos) {
            // No volume boost field, just phonetic
            std::string phonetic = input.substr(fourthDelim + 1);
            return ScriptLine(sub, exp, act, anim, actor, phonetic, 1.0f);
        }

        std::string phonetic = input.substr(fourthDelim + 1, fifthDelim - fourthDelim - 1);
        size_t sixthDelim = input.find(delimiter, fifthDelim + 1);
        std::string volumeBoostStr;
        std::string rechatTargetHint;
        std::string utteranceId;
        if (sixthDelim == std::string::npos) {
            volumeBoostStr = input.substr(fifthDelim + 1);
        } else {
            volumeBoostStr = input.substr(fifthDelim + 1, sixthDelim - fifthDelim - 1);
            size_t seventhDelim = input.find(delimiter, sixthDelim + 1);
            if (seventhDelim == std::string::npos) {
                rechatTargetHint = input.substr(sixthDelim + 1);
            } else {
                rechatTargetHint = input.substr(sixthDelim + 1, seventhDelim - sixthDelim - 1);
                utteranceId = input.substr(seventhDelim + 1);
            }
        }

        float volumeBoost = 1.0f;
        try {
            if (!volumeBoostStr.empty()) {
                volumeBoost = std::stof(volumeBoostStr);
            }
        } catch (...) {
            logger::warn("Failed to parse volumeBoost: '{}', using default 1.0", volumeBoostStr);
            volumeBoost = 1.0f;
        }

        return ScriptLine(sub, exp, act, anim, actor, phonetic, volumeBoost, -1, rechatTargetHint, utteranceId);
    }
};

class SpeakManager {
private:
    std::queue<ScriptLine> scriptQueue;
    std::mutex mtx;  // Mutex for thread safety
    bool isProcessing;
    std::chrono::high_resolution_clock::time_point lastUsedTime;
    float preClip = 0.1;
    float postClip = 0.1;

    int resolution = 500 * 1;  // 10 def value
    float animIntensity = 1.0f;
    std::atomic<float> headVoiceVolumeMultiplier{1.0f};

    bool interrupt = false;
    bool forceInterruptCurrentPlayback = false;
    std::string lastRechatter = "";
    bool rechatInFlight = false;
    std::string rechatInFlightSpeaker = "";
    std::string currentRechatChainId = "";
    bool rechatChainClosed = false;
    bool rechatChainHardCancelled = false;
    std::string currentPlaybackUtteranceId = "";
    std::string currentPlaybackActor = "";
    bool currentPlaybackUtteranceConfirmed = false;
    struct RecentAiSubtitle {
        RE::FormID speakerFormId;
        std::string text;
        std::chrono::steady_clock::time_point expiresAt;
    };
    std::mutex recentAiSubtitleMutex;
    std::deque<RecentAiSubtitle> recentAiSubtitles;
    struct PendingRechatRetry {
        bool active = false;
        std::string speaker = "";
        std::string listenerHint = "";
        std::string explicitTarget = "";
        std::string debugLauncherLine = "";
        int rechatDepth = 0;
    } pendingRechatRetry;
    std::string audienceSnapshotKey = "";
    std::vector<std::string> audienceSnapshotCompanions = {};
    bool audienceSnapshotReady = false;
    std::function<void(const ScriptLine&, int)> playerPlaybackCompletedCallback;
    std::string pendingPlayerSubtitleText;
    bool pendingPlayerSubtitleActive = false;
    std::chrono::high_resolution_clock::time_point pendingPlayerSubtitleLastRefresh{};
    std::chrono::high_resolution_clock::time_point lastVisemeApplied;
    std::string narratorDisplayName = NARRATOR_NAME;

    SpeakManager() : isProcessing(false) {}  // Private constructor for Singleton pattern

    std::chrono::high_resolution_clock::time_point rechatCooldown = std::chrono::high_resolution_clock::now();

   

public:
    // Get the singleton instance of SpeakManager
    static SpeakManager& getInstance();

    void setNarratorDisplayName(const std::string& displayName);
    std::string getNarratorDisplayName();

    void setHeadVoiceVolumePercent(float percent) {
        headVoiceVolumeMultiplier.store(HeadVoiceVolumeUtils::PercentToMultiplier(percent), std::memory_order_relaxed);
    }

    float getHeadVoiceVolumeMultiplier() const { return headVoiceVolumeMultiplier.load(std::memory_order_relaxed); }

    int getResolution() {
        std::lock_guard<std::mutex> lock(mtx);
        return resolution;
    }
    void setResolution(int i) {
        std::lock_guard<std::mutex> lock(mtx);
        resolution = i;
    }
    float getAnimIntensity() {
        std::lock_guard<std::mutex> lock(mtx);
        return animIntensity;
    }
    void setAnimIntensity(float i) {
        std::lock_guard<std::mutex> lock(mtx);
        animIntensity = i;
    }

    void setLastRechatter(std::string i) {
        std::lock_guard<std::mutex> lock(mtx);
        lastRechatter.assign(i);
    }

    std::string getLastRechatter() {
        std::lock_guard<std::mutex> lock(mtx);
        return lastRechatter;
    }

    bool isRechatInFlightFor(const std::string& speaker) {
        std::lock_guard<std::mutex> lock(mtx);
        return rechatInFlight && rechatInFlightSpeaker == speaker;
    }

    void resetRechatChainState();
    void cancelRechatChain();
    void startRechatChainForPlayerInput();
    void startRechatChainForAutonomousEvent();
    bool isRechatChainClosed();
    std::string ensureRechatChainId(const std::string& speaker, const std::string& listenerHint,
                                    const std::string& explicitTarget);

    bool beginRechatAttempt(const std::string& speaker);
    void queueRechatRetry(const std::string& speaker, const std::string& listenerHint,
                          const std::string& explicitTarget, const std::string& debugLauncherLine, int rechatDepth);
    void completeRechatAttempt(const std::string& speaker, bool success);

    void abortPlay(bool forceCurrentPlayback = false) {
        std::lock_guard<std::mutex> lock(mtx);
        interrupt = true;
        if (forceCurrentPlayback) {
            forceInterruptCurrentPlayback = true;
        }
    }

    bool shouldAbortPlaybackForSpeaker(const std::string& speakerName);

    bool isAborted() {
        std::lock_guard<std::mutex> lock(mtx);
        bool is = interrupt;
        interrupt = false;
        forceInterruptCurrentPlayback = false;
        return is;
    }

    void stopRechatForNseconds(int n) {
        std::lock_guard<std::mutex> lock(mtx);
        rechatCooldown = std::chrono::high_resolution_clock::now() + std::chrono::seconds(n);
    }

    std::string getCurrentProcessingActorName() {
        std::lock_guard<std::mutex> lock(mtx);
        return currentPlaybackActor;
    }
    // Method to insert a ScriptLine into the queue
    void insertInQueue(const ScriptLine& scriptLine);

    // Method to get the first item in the queue
    ScriptLine getFirstItem();

    // Method to get the first item in the queue
    ScriptLine getLastItem();

    // Method to dequeue the first item from the queue
    void dequeueFirstItem();
    bool dropMatchingHeadItem(const ScriptLine& expectedLine);

    // Check if queue has items
    bool hasItems();
    int countItems();

    std::chrono::high_resolution_clock::time_point getLastUsedTime();
    void setLastUsedTime();

    // Method to process the queue
    void process(AIAgent* agent);
    void processPlayer();
    void setPendingPlayerSubtitle(const std::string& subtitleText);
    void releasePendingPlayerSubtitle();
    void refreshPendingPlayerSubtitle(bool force = false);
    void clearVisibleSubtitles();

    bool getProcessing();

    // Setter for isProcessing
    void setProcessing(bool processing);
    void endDialogue(RE::Actor* npc, std::string lastLine);

    void setPreclip(float val);
    void setPostclip(float val);
    void setPlaybackDropoffInside(float percent);
    void setPlaybackDropoffOutside(float percent);
    float getPlaybackDropoffInside();
    float getPlaybackDropoffOutside();

    void abortPendingUtterances(const std::string& reason, bool includeCurrentPlayback = true);
    void deleteQueue(bool isActionCommand = false);
    void deleteQueuedPlayerLines();
    void discardPendingInteraction();
    void setPlayerPlaybackCompletedCallback(std::function<void(const ScriptLine&, int)> callback);
    void clearPlayerPlaybackCompletedCallback();
    void recoverFromProcessingFailure(const std::string& actorName);
    void registerAiSubtitle(RE::FormID speakerFormId, const std::string& subtitleText);
    bool isRecentAiSubtitle(RE::FormID speakerFormId, const std::string& subtitleText);

    bool downloadFakeNote(std::string name);

    int rechat(std::string speaker, std::string targetedNpc, int rechatDepth, std::string debugLauncherLine,
               std::string explicitRechatTarget = "");

     void setLastVisemeApplied(std::chrono::high_resolution_clock::time_point timePoint) {
        std::lock_guard<std::mutex> lock(mtx);
        lastVisemeApplied = timePoint;
    }

    std::chrono::high_resolution_clock::time_point getLastVisemeApplied() {
        std::lock_guard<std::mutex> lock(mtx);
        return lastVisemeApplied;
    }
};

void ProcessVrVisemePumpOnGameThread(float deltaSeconds);

#endif
