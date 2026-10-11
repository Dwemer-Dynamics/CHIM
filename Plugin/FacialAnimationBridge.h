#pragma once

#include "BFA_SpeechAPI.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string_view>

namespace FacialAnimationBridge {

// CHIM expressions map directly to Audio2Face mood channels.
inline BFA_SpeechEmotion MakeMood(std::string_view expression)
{
    BFA_SpeechEmotion mood{};
    mood.version = BFA_SPEECH_API_VERSION;
    mood.size = sizeof(mood);
    if (expression == "DialogueAnger" || expression == "MoodAnger" || expression == "CombatAnger") {
        mood.channels[1] = 0.9f;
    } else if (expression == "DialogueDisgusted" || expression == "MoodDisgusted") {
        mood.channels[3] = 0.9f;
    } else if (expression == "DialogueFear" || expression == "MoodFear") {
        mood.channels[4] = 0.9f;
    } else if (expression == "DialogueSad" || expression == "MoodSad") {
        mood.channels[9] = 0.9f;
    } else if (expression == "DialogueHappy" || expression == "MoodHappy") {
        mood.channels[6] = 0.9f;
    } else if (expression == "DialogueSurprise" || expression == "MoodSurprise") {
        mood.channels[0] = 0.9f;
    } else if (expression == "DialoguePuzzled" || expression == "MoodPuzzled") {
        mood.channels[0] = 0.4f;
    }
    return mood;
}

inline bool IsCompatible(const BFA_SpeechInterface* api)
{
    return api && api->version >= BFA_SPEECH_API_VERSION &&
           api->size >= sizeof(BFA_SpeechInterface) && api->Prepare && api->Start &&
           api->Pause && api->Resume && api->Seek && api->Stop && api->EndUtterance &&
           api->SetMood && api->Owner && api->RegisterListener;
}

#ifdef _WIN32
inline const BFA_SpeechInterface* Discover()
{
    const auto module = GetModuleHandleA("BetterFacialAnimations.dll");
    const auto query = module ? reinterpret_cast<BFA_QuerySpeechInterfaceFn>(
        GetProcAddress(module, "BFA_QuerySpeechInterface")) : nullptr;
    const auto* api = query ? query(BFA_SPEECH_API_VERSION) : nullptr;
    return IsCompatible(api) ? api : nullptr;
}

inline bool CanWriteActor(std::uint32_t actor)
{
    const auto* api = Discover();
    return !api || api->Owner(actor) == BFA_SpeechOwnerEngine;
}
#endif

// Queued writes check this at execution, not when queued. The callback gates
// writes before returning; BFA waits for callback delivery before deforming.
struct FaceToken {
    const BFA_SpeechInterface* api = nullptr;
    std::uint32_t actor = 0;
    std::atomic<bool> active{true};
    std::atomic<bool> owned{false};
    mutable std::atomic<unsigned> fallbackChecks{0}, blockedChecks{0};

    bool CanWrite() const
    {
        const bool allowed = active.load() && !owned.load() &&
            (!api || api->Owner(actor) == BFA_SpeechOwnerEngine);
        if (allowed) ++fallbackChecks;
        else ++blockedChecks;
        return allowed;
    }
};

// Listener context lives with the plugin; no callback can outlive a stack Line.
// Register the correlation ID BEFORE Prepare, which can finish asynchronously
// before returning its handle. Don't hold the registry lock across API calls.
struct EventRegistry {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::weak_ptr<FaceToken>> clips;
    std::once_flag once;
    std::uint64_t listener = 0;

    static void Receive(const BFA_SpeechEvent* event, void* context)
    {
        auto& registry = *static_cast<EventRegistry*>(context);
        std::lock_guard lock(registry.mutex);
        const auto found = registry.clips.find(event->utterance_id);
        if (found == registry.clips.end()) return;
        const auto token = found->second.lock();
        if (!token || token->actor != event->speaker_form_id) return;
        if (event->event == BFA_SpeechStarted) token->owned.store(true);
        else if (event->event == BFA_SpeechEnded || event->event == BFA_SpeechFailed ||
                 event->event == BFA_SpeechDeclined || event->event == BFA_SpeechHandleExpired ||
                 (event->event == BFA_SpeechOwnerChanged && event->handle))
            token->owned.store(false);
    }
};
inline EventRegistry& Registry()
{
    static auto* registry = new EventRegistry;
    return *registry;
}

class Line {
public:
    Line(const BFA_SpeechInterface* api, std::uint32_t actor) : token(std::make_shared<FaceToken>())
    {
        token->api = IsCompatible(api) ? api : nullptr;
        token->actor = actor;
        if (token->api) {
            auto& registry = Registry();
            std::call_once(registry.once, [&] {
                token->api->RegisterListener(EventRegistry::Receive, &registry, &registry.listener);
            });
            if (!registry.listener) token->api = nullptr;
        }
    }
    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;
    ~Line() { Stop(BFA_SpeechInterrupted); }

    std::uint32_t Prepare(const void* audio, std::uint32_t bytes, const char*,
                          std::string_view expression)
    {
        auto* api = token->api;
        if (!api || !token->actor) return BFA_SpeechUnavailable;
        if (!expression.empty()) {
            const auto mood = MakeMood(expression);
            const auto result = api->SetMood(token->actor, &mood);
            if (result != BFA_SpeechOK) return result;
        }
        static std::atomic<std::uint64_t> nextUtterance{0};
        // CHIM's downloaded clips are independently delimited. A namespaced
        // counter avoids colliding with another provider's small reply IDs.
        utterance = 0x4348494D00000000ull | ++nextUtterance;
        {
            auto& registry = Registry();
            std::lock_guard lock(registry.mutex);
            registry.clips[utterance] = token;
        }
        BFA_SpeechLine line{};
        line.version = BFA_SPEECH_API_VERSION;
        line.size = sizeof(line);
        line.utterance_id = utterance;
        line.speaker_form_id = token->actor;
        line.audio = audio;
        line.audio_bytes = bytes;
        return api->Prepare(&line, &handle);
    }
    BFA_SpeechResult Start(double position)
    {
        return handle ? token->api->Start(handle, position) : BFA_SpeechResult(BFA_SpeechInvalidHandle);
    }
    BFA_SpeechResult Pause()
    {
        return handle ? token->api->Pause(handle) : BFA_SpeechResult(BFA_SpeechInvalidHandle);
    }
    BFA_SpeechResult Resume(double position)
    {
        return handle ? token->api->Resume(handle, position) : BFA_SpeechResult(BFA_SpeechInvalidHandle);
    }
    BFA_SpeechResult Seek(double position)
    {
        return handle ? token->api->Seek(handle, position) : BFA_SpeechResult(BFA_SpeechInvalidHandle);
    }
    void Stop(std::uint32_t reason)
    {
        token->active.store(false);
        if (token->api && utterance) {
            if (handle) token->api->Stop(handle, reason);
            token->api->EndUtterance(token->actor, utterance, reason);
            auto& registry = Registry();
            std::lock_guard lock(registry.mutex);
            registry.clips.erase(utterance);
            utterance = handle = 0;
        }
    }
    std::shared_ptr<FaceToken> GetToken() const { return token; }

private:
    std::shared_ptr<FaceToken> token;
    std::uint64_t handle = 0, utterance = 0;
};

}  // namespace FacialAnimationBridge
