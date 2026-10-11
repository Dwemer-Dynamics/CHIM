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

inline bool IsCompatible(const BFA_Interface* api)
{
    return api && api->version >= BFA_API_VERSION &&
           api->size >= offsetof(BFA_Interface, SetMood) + sizeof(api->SetMood) &&
           api->IsReady && api->Owner && api->Prepare && api->Start && api->Pause &&
           api->Resume && api->Seek && api->Stop && api->SetMood;
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

// Queued game-thread writes retain this token, never the playback stack. Check
// ownership when the task executes: inference can finish after it was queued.
struct FaceToken {
    const BFA_Interface* api = nullptr;
    std::uint32_t actor = 0;
    std::atomic<bool> active{true};

    bool CanWrite() const
    {
        return active.load() && (!api || api->Owner(actor) == BFA_kOwnerEngine);
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

    std::uint32_t Prepare(const void* audio, std::uint32_t bytes, const char* text,
                          std::string_view expression)
    {
        auto* api = token->api;
        if (!api || !api->IsReady() || !token->actor) return BFA_kDisabled;
        BFA_Line line{};
        line.version = BFA_API_VERSION;
        line.size = sizeof(line);
        // Each DownloadAndPlay call owns a complete clip. Do not claim a reply
        // across CHIM's queue until its final-segment boundary is available here.
        static std::atomic<std::uint64_t> nextUtterance{0};
        line.utterance_id = ++nextUtterance;
        line.flags = BFA_kLastSegment;
        line.speaker_form_id = token->actor;
        line.audio.version = BFA_API_VERSION;
        line.audio.size = sizeof(line.audio);
        line.audio.format = BFA_kContainer;
        line.audio.data = audio;
        line.audio.bytes = bytes;
        line.emotion = MakeMood(expression);
        line.text = text;
        std::uint32_t reason = BFA_kAccepted;
        handle = api->Prepare(&line, &reason);
        // An omitted expression preserves the previous mood; MoodNeutral clears
        // it explicitly. BFA clears its world state on a save/load transition.
        if (handle && !expression.empty()) {
            api->SetMood(token->actor, &line.emotion, 0.2f);
        }
        return reason;
    }

    void Start(double position) { if (handle) token->api->Start(handle, position); }
    void Pause() { if (handle) token->api->Pause(handle); }
    void Resume(double position) { if (handle) token->api->Resume(handle, position); }
    void Seek(double position) { if (handle) token->api->Seek(handle, position); }
    void Stop(std::uint32_t reason)
    {
        token->active.store(false);
        if (handle) {
            token->api->Stop(handle, reason);
            handle = 0;
        }
    }
    std::shared_ptr<FaceToken> GetToken() const { return token; }

private:
    std::shared_ptr<FaceToken> token;
    std::uint64_t handle = 0;
};

}  // namespace FacialAnimationBridge
