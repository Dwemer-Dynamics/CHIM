#pragma once

#include "BFA_API.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace FacialAnimationBridge {

// CHIM uses FaceGen expression indices; BFA expects dialogue emotion indices.
inline BFA_Emotion MakeMood(std::string_view expression)
{
    BFA_Emotion mood{};
    mood.version = BFA_API_VERSION;
    mood.size = sizeof(mood);
    mood.skyrim_value = 90;
    mood.gain = 1.0f;
    if (expression == "DialogueAnger" || expression == "MoodAnger" || expression == "CombatAnger") {
        mood.skyrim_type = 1;
    } else if (expression == "DialogueDisgusted" || expression == "MoodDisgusted") {
        mood.skyrim_type = 2;
    } else if (expression == "DialogueFear" || expression == "MoodFear") {
        mood.skyrim_type = 3;
    } else if (expression == "DialogueSad" || expression == "MoodSad") {
        mood.skyrim_type = 4;
    } else if (expression == "DialogueHappy" || expression == "MoodHappy") {
        mood.skyrim_type = 5;
    } else if (expression == "DialogueSurprise" || expression == "MoodSurprise") {
        mood.skyrim_type = 6;
    } else if (expression == "DialoguePuzzled" || expression == "MoodPuzzled") {
        mood.skyrim_type = 7;
    } else {
        mood.skyrim_value = 0;
        mood.gain = 0.0f;
    }
    return mood;
}

inline bool Accepted(BFA_Result result)
{
    return result == BFA_ResultOK || result == BFA_ResultPending ||
           result == BFA_ResultNoChange;
}
inline const char* ResultName(std::uint32_t code)
{
    switch (code) {
    case BFA_ResultOK: return "ok";
    case BFA_ResultPending: return "pending";
    case BFA_ResultNoChange: return "no-change";
    case BFA_ResultAlreadyStopped: return "already-stopped";
    case BFA_ResultInvalidArgument: return "invalid-argument";
    case BFA_ResultInvalidHandle: return "invalid-handle";
    case BFA_ResultInvalidState: return "invalid-state";
    case BFA_ResultNotFound: return "not-found";
    case BFA_ResultUnavailable: return "unavailable";
    case BFA_ResultDisabled: return "disabled";
    case BFA_ResultPlayerDisabled: return "player-disabled";
    case BFA_ResultRaceDisabled: return "race-disabled";
    case BFA_ResultInScene: return "in-scene";
    case BFA_ResultNoFaceGenHead: return "no-face-gen-head";
    case BFA_ResultHeadMapFailed: return "head-map-failed";
    case BFA_ResultModelNotLoaded: return "model-not-loaded";
    case BFA_ResultNoDevice: return "no-device";
    case BFA_ResultSuppressed: return "suppressed";
    case BFA_ResultUnsupportedRuntime: return "unsupported-runtime";
    case BFA_ResultAudioFailed: return "audio-failed";
    case BFA_ResultNotInDialogue: return "not-in-dialogue";
    case BFA_ResultInternalError: return "internal-error";
    case BFA_ResultInferenceFailed: return "inference-failed";
    case BFA_ResultCancelled: return "cancelled";
    default: return "unknown";
    }
}
inline bool IsCompatible(const BFA_Interface* api)
{
    return api && api->version == BFA_API_VERSION && api->size >= sizeof(BFA_Interface) &&
           api->Prepare && api->Start && api->Pause && api->Resume && api->Seek &&
           api->Stop && api->SetMood && api->Owner && api->GetStatus && api->ReleaseHandle;
}
#ifdef _WIN32
inline const BFA_Interface* Discover()
{
    const auto module = GetModuleHandleA("BetterFacialAnimations.dll");
    const auto query = module ? reinterpret_cast<BFA_QueryInterfaceFn>(
        GetProcAddress(module, "BFA_QueryInterface")) : nullptr;
    const auto* api = query ? query(BFA_API_VERSION) : nullptr;
    return IsCompatible(api) ? api : nullptr;
}
inline bool CanWriteActor(std::uint32_t actor)
{
    const auto* api = Discover();
    return !api || api->Owner(actor) == BFA_kOwnerEngine;
}
#endif

// Every queued facial task checks this at execution, not when queued. Owner is
// an instantaneous snapshot, not a cross-plugin write lease or callback gate.
struct FaceToken {
    const BFA_Interface* api = nullptr;
    std::uint32_t actor = 0;
    std::atomic<bool> active{true};
    mutable std::atomic<unsigned> fallbackChecks{0}, blockedChecks{0};
    bool CanWrite() const
    {
        const bool allowed = active.load() &&
            (!api || api->Owner(actor) == BFA_kOwnerEngine);
        if (allowed) ++fallbackChecks;
        else ++blockedChecks;
        return allowed;
    }
};

class Line {
public:
    Line(const BFA_Interface* api, std::uint32_t actor) : token(std::make_shared<FaceToken>())
    {
        token->api = IsCompatible(api) ? api : nullptr;
        token->actor = actor;
    }
    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;
    ~Line() { Stop(BFA_kAborted); }

    BFA_Result Prepare(const void* audio, std::uint32_t bytes, const char* text,
                       std::string_view expression)
    {
        if (!token->api) return last = {BFA_ResultUnavailable};
        if (!token->active || handle) return last = {BFA_ResultInvalidState};
        BFA_Line line{};
        line.version = BFA_API_VERSION;
        line.size = sizeof(line);
        static std::atomic<std::uint64_t> nextUtterance{0};
        line.utterance_id = 0x4348494D00000000ull | ++nextUtterance;
        line.flags = BFA_kLastSegment; // each downloaded clip is independently delimited
        line.speaker_form_id = token->actor;
        line.audio.version = BFA_API_VERSION;
        line.audio.size = sizeof(line.audio);
        line.audio.format = BFA_kContainer;
        line.audio.data = audio;
        line.audio.bytes = bytes;
        line.emotion = MakeMood(expression);
        line.text = text;
        if (!expression.empty()) {
            const auto moodResult = token->api->SetMood(token->actor, &line.emotion, 0);
            if (!Accepted(moodResult)) return last = moodResult;
        }
        // An immediate failure may still return a handle: retain and inspect it.
        return Observe(token->api->Prepare(&line, &handle));
    }
    BFA_Result Start(double position)
    {
        if (!handle) return UnavailableHandle();
        if (Terminal()) return last = snapshot.playback == BFA_PlaybackEnded
            ? BFA_Result{BFA_ResultAlreadyStopped} : snapshot.result;
        return Observe(token->api->Start(handle, position));
    }
    BFA_Result Pause()
    {
        return handle ? Observe(token->api->Pause(handle)) : UnavailableHandle();
    }
    BFA_Result Resume(double position)
    {
        return handle ? Observe(token->api->Resume(handle, position)) : UnavailableHandle();
    }
    BFA_Result Seek(double position)
    {
        if (!handle) return UnavailableHandle();
        if (Terminal()) return last = snapshot.playback == BFA_PlaybackEnded
            ? BFA_Result{BFA_ResultAlreadyStopped} : snapshot.result;
        return Observe(token->api->Seek(handle, position));
    }
    BFA_Result Stop(std::uint32_t reason)
    {
        token->active.store(false);
        if (!handle) return last; // destructor must not issue a duplicate stop
        auto result = Observe(token->api->Stop(handle, reason));
        if (Terminal()) {
            // Copy terminal reason before releasing the record. Pending cleanup
            // retains it in BFA's bounded terminal store; never wait on game work.
            const auto released = token->api->ReleaseHandle(handle);
            if (!Accepted(released)) result = last = released;
        }
        handle = 0;
        return result;
    }
    bool HasStatus() const { return hasStatus; }
    const BFA_LineStatus& Snapshot() const { return snapshot; }
    BFA_Result LastResult() const { return last; }
    std::shared_ptr<FaceToken> GetToken() const { return token; }

private:
    bool Terminal() const
    {
        return hasStatus && snapshot.playback >= BFA_PlaybackEnded;
    }
    BFA_Result UnavailableHandle()
    {
        // Keep the original rejection visible on later clock calls.
        if (!Accepted(last)) return last;
        return last = {BFA_ResultInvalidHandle};
    }
    BFA_Result Observe(BFA_Result operation)
    {
        last = operation;
        if (!handle) return last;
        BFA_LineStatus status{};
        status.version = BFA_API_VERSION;
        status.size = sizeof(status);
        const auto queried = token->api->GetStatus(handle, &status);
        if (queried == BFA_ResultOK) {
            snapshot = status;
            hasStatus = true;
            // Surface asynchronous failure even if this clock update was accepted.
            if (status.playback == BFA_PlaybackFailed || status.playback == BFA_PlaybackDeclined)
                last = status.result;
        } else if (queried == BFA_ResultInvalidHandle) {
            handle = 0;
            last = queried;
        }
        return last;
    }
    std::shared_ptr<FaceToken> token;
    std::uint64_t handle = 0;
    BFA_Result last{BFA_ResultUnavailable};
    BFA_LineStatus snapshot{};
    bool hasStatus = false;
};

} // namespace FacialAnimationBridge
