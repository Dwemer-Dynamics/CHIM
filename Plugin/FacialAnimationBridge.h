#pragma once
#include "BFA_API.h"
#ifdef _WIN32
#include <Windows.h>
#endif
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
namespace FacialAnimationBridge {
inline bool Accepted(BFA_Result r) {
    return r == BFA_kResultOK || r == BFA_kResultPending || r == BFA_kResultNoChange;
}
inline const char* ResultName(std::uint32_t code)
{
    switch (code) {
    case BFA_kResultOK: return "ok";
    case BFA_kResultPending: return "pending";
    case BFA_kResultNoChange: return "no-change";
    case BFA_kResultInvalidArgument: return "invalid-argument";
    case BFA_kResultInvalidHandle: return "invalid-handle";
    case BFA_kResultInvalidState: return "invalid-state";
    case BFA_kResultNotFound: return "not-found";
    case BFA_kResultUnavailable: return "unavailable";
    case BFA_kResultDisabled: return "disabled";
    case BFA_kResultPlayerDisabled: return "player-disabled";
    case BFA_kResultRaceDisabled: return "race-disabled";
    case BFA_kResultInScene: return "in-scene";
    case BFA_kResultNoFaceGenHead: return "no-face-gen-head";
    case BFA_kResultHeadMapFailed: return "head-map-failed";
    case BFA_kResultModelNotLoaded: return "model-not-loaded";
    case BFA_kResultNoDevice: return "no-device";
    case BFA_kResultUnsupportedRuntime: return "unsupported-runtime";
    case BFA_kResultAudioFailed: return "audio-failed";
    case BFA_kResultNotInDialogue: return "not-in-dialogue";
    case BFA_kResultInternalError: return "internal-error";
    case BFA_kResultInferenceFailed: return "inference-failed";
    case BFA_kResultCancelled: return "cancelled";
    case BFA_kResultTakenByOtherLine: return "taken-by-other-line";
    case BFA_kResultExpired: return "expired";
    case BFA_kResultCapacity: return "capacity";
    default: return "unknown";
    }
}

inline BFA_Emotion MakeMood(std::string_view expression) {
    BFA_Emotion mood{};
    mood.version = BFA_API_VERSION; mood.size = sizeof mood;
    if (expression.find("Anger") != expression.npos) mood.channels[BFA_kAnger] = 1;
    else if (expression.find("Disgust") != expression.npos) mood.channels[BFA_kDisgust] = 1;
    else if (expression.find("Fear") != expression.npos) mood.channels[BFA_kFear] = 1;
    else if (expression.find("Sad") != expression.npos) mood.channels[BFA_kSadness] = 1;
    else if (expression.find("Happy") != expression.npos) mood.channels[BFA_kJoy] = 1;
    else if (expression.find("Surprise") != expression.npos) mood.channels[BFA_kAmazement] = 1;
    else if (expression.find("Puzzled") != expression.npos) mood.channels[BFA_kAmazement] = .4f;
    return mood;
}
inline bool IsCompatible(const BFA_Interface* api) {
    return api && api->version == BFA_API_VERSION && api->size >= sizeof(*api) &&
        api->NewUtteranceId && api->Prepare && api->Start && api->Pause && api->Seek &&
        api->Stop && api->EndUtterance && api->RegisterListener;
}
// One reply (a BFA utterance) per speaking actor: all of its sentences share
// it, so BFA keeps the face between them and hands it back once, after
// EndReply. The token is the write gate for the actor's face.
struct FaceToken {
    std::uint32_t actor = 0;
    std::uint64_t utterance = 0;
    std::uint32_t segments = 0;
    std::atomic<bool> ended{false}, animated{false};
    std::atomic<std::uint32_t> event{UINT32_MAX}, result{BFA_kResultPending};
    std::atomic<std::uint64_t> terminal_handle{0};  // the segment last told a terminal event
    std::chrono::steady_clock::time_point touched = std::chrono::steady_clock::now();
    mutable std::atomic<unsigned> fallbackChecks{0}, blockedChecks{0};
    bool CanWrite() const;
};
// Process-lifetime callback context. Tokens stay registered until BFA hands the
// face back (a terminal event after EndReply). Callbacks never call back into BFA.
struct Events {
    std::mutex mutex, registration;
    std::unordered_map<std::uint64_t, std::shared_ptr<FaceToken>> tokens;   // by utterance
    std::unordered_map<std::uint32_t, std::shared_ptr<FaceToken>> replies;  // open, by actor
    const BFA_Interface* api = nullptr;
    std::uint64_t listener = 0;
};
inline Events& State() { static auto* state = new Events; return *state; }
// An open reply with no new sentence for this long is over when the next one starts.
inline double& StaleSeconds() { static double seconds = 10; return seconds; }
inline double Age(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}
inline void OnEvent(const BFA_Event* ev, void*) {
    if (!ev || ev->version != BFA_API_VERSION || ev->size < sizeof(*ev)) return;
    auto& state = State(); std::lock_guard<std::mutex> guard(state.mutex);
    auto it = state.tokens.find(ev->utterance_id);
    if (it == state.tokens.end()) return;
    const auto token = it->second;
    if (!token) { state.tokens.erase(it); return; }
    if (ev->speaker_form_id != token->actor || ev->event > BFA_kExpired) return;
    if (ev->event == BFA_kStarted) {
        if (ev->handle == token->terminal_handle) return;  // late, for a finished segment
        token->event = ev->event; token->animated = true;
        return;
    }
    token->event = ev->event;
    token->result = ev->event == BFA_kEnded ? BFA_kResultOK : ev->reason;
    token->terminal_handle = ev->handle;
    token->animated = false;  // BFA handed the face back (or declined this sentence)
    if (token->ended) state.tokens.erase(it);
}
inline bool Listen(const BFA_Interface* api) {
    if (!IsCompatible(api)) return false;
    auto& state = State(); std::lock_guard<std::mutex> guard(state.registration);
    if (state.api == api && state.listener) return true;
    BFA_Result result;
    auto id = api->RegisterListener(OnEvent, nullptr, &result);
    if (!id || result != BFA_kResultOK) return false;
    state.api = api; state.listener = id; return true;
}
inline bool CanWriteActor(std::uint32_t actor) {
    auto& state = State(); std::lock_guard<std::mutex> guard(state.mutex);
    for (auto it = state.tokens.begin(); it != state.tokens.end(); ) {
        const auto token = it->second;
        if (!token) { it = state.tokens.erase(it); continue; }
        if (token->actor == actor && token->animated) return false;
        ++it;
    }
    return true;
}
inline bool FaceToken::CanWrite() const {
    const bool allowed = CanWriteActor(actor);
    if (allowed) ++fallbackChecks; else ++blockedChecks;
    return allowed;
}
// Closes the actor's open reply: BFA stops its sentences, fades the face back and
// then sends the terminal events that reopen the write gate.
inline void EndReply(std::uint32_t actor, std::uint32_t reason) {
    auto& state = State();
    std::shared_ptr<FaceToken> token;
    const BFA_Interface* api = nullptr;
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        auto it = state.replies.find(actor);
        if (it == state.replies.end()) return;
        token = it->second; state.replies.erase(it);
        token->ended = true; api = state.api;
    }
    const auto result = api ? api->EndUtterance(token->utterance, reason) : BFA_kResultUnavailable;
    if (!Accepted(result)) {  // nothing of it reached BFA: no events will come
        std::lock_guard<std::mutex> guard(state.mutex);
        state.tokens.erase(token->utterance);
        token->animated = false;
    }
}
#ifdef _WIN32
inline const BFA_Interface* Discover() {
    auto module = GetModuleHandleA("BetterFacialAnimations.dll");
    auto query = module ? reinterpret_cast<BFA_QueryInterfaceFn>(GetProcAddress(module, "BFA_QueryInterface")) : nullptr;
    auto api = query ? query(BFA_API_VERSION) : nullptr;
    return IsCompatible(api) ? api : nullptr;
}
#endif
// One sentence of the actor's current reply (opened by the first sentence).
class Line {
public:
    Line(const BFA_Interface* source, std::uint32_t actor) {
        api = Listen(source) ? source : nullptr;
        if (!api) {
            token = std::make_shared<FaceToken>(); token->actor = actor;
            return;
        }
        auto& state = State();
        bool stale = false;
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            auto it = state.replies.find(actor);
            stale = it != state.replies.end() && Age(it->second->touched) >= StaleSeconds();
        }
        if (stale) EndReply(actor, BFA_kCompleted);
        std::lock_guard<std::mutex> guard(state.mutex);
        auto& reply = state.replies[actor];
        if (!reply) {
            reply = std::make_shared<FaceToken>();
            reply->actor = actor;
            reply->utterance = api->NewUtteranceId();
            state.tokens[reply->utterance] = reply;
        }
        token = reply;
        segment = token->segments++;
        token->touched = std::chrono::steady_clock::now();
    }
    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;
    ~Line() { Stop(BFA_kAborted); }
    BFA_Result Prepare(const void* audio, std::uint32_t bytes, const char*, std::string_view expression) {
        if (!api) return last = BFA_kResultUnavailable;
        if (handle || stopped) return last = BFA_kResultInvalidState;
        const auto mood = MakeMood(expression);
        BFA_Line line{BFA_API_VERSION, sizeof(BFA_Line), token->utterance, segment, token->actor, audio, bytes,
                      expression.empty() ? nullptr : &mood};
        handle = api->Prepare(&line, &last);
        return LastResult();
    }
    BFA_Result Start(double t) { return Ready() ? last = api->Start(handle, t) : LastResult(); }
    BFA_Result Pause() { return Ready() ? last = api->Pause(handle) : LastResult(); }
    BFA_Result Seek(double t) { return Ready() ? last = api->Seek(handle, t) : LastResult(); }
    // Ends this sentence; the reply stays open for the next (EndReply closes it).
    BFA_Result Stop(std::uint32_t reason) {
        if (stopped) return LastResult();
        stopped = true;
        if (Ready(true)) last = api->Stop(handle, reason);
        token->touched = std::chrono::steady_clock::now();
        return LastResult();
    }
    BFA_Result LastResult() const {
        return handle && token->terminal_handle == handle ? token->result.load() : last;
    }
    std::uint64_t Handle() const { return handle; }
    std::shared_ptr<FaceToken> GetToken() const { return token; }
private:
    bool Ready(bool stopping = false) const {
        return api && handle && (stopping || !stopped) && !token->ended && token->terminal_handle != handle;
    }
    const BFA_Interface* api = nullptr;
    std::shared_ptr<FaceToken> token;
    std::uint32_t segment = 0;
    std::uint64_t handle = 0;
    bool stopped = false;
    BFA_Result last = BFA_kResultUnavailable;
};
} // namespace FacialAnimationBridge
