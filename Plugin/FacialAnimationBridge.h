#pragma once
#include "BFA_API.h"
#ifdef _WIN32
#include <Windows.h>
#endif
#include <atomic>
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
struct FaceToken {
    std::uint32_t actor = 0;
    std::atomic<bool> active{true}, animated{false}, terminal{false};
    std::atomic<std::uint32_t> event{UINT32_MAX}, result{BFA_kResultPending};
    mutable std::atomic<unsigned> fallbackChecks{0}, blockedChecks{0};
    bool CanWrite() const;
};
// Process-lifetime callback context; tokens are retained until cleanup completes and removed
// on terminal events (or rejected intake). Callbacks never call back into BFA.
struct Events {
    std::mutex mutex, registration;
    std::unordered_map<std::uint64_t, std::shared_ptr<FaceToken>> tokens;
    const BFA_Interface* api = nullptr;
    std::uint64_t listener = 0;
};
inline Events& State() { static auto* state = new Events; return *state; }
inline void OnEvent(const BFA_Event* ev, void*) {
    if (!ev || ev->version != BFA_API_VERSION || ev->size < sizeof(*ev)) return;
    auto& state = State(); std::lock_guard<std::mutex> guard(state.mutex);
    auto it = state.tokens.find(ev->utterance_id);
    if (it == state.tokens.end()) return;
    const auto token = it->second;
    if (!token) { state.tokens.erase(it); return; }
    if (token->terminal || ev->speaker_form_id != token->actor) return;
    if (ev->event > BFA_kExpired) return;
    token->event = ev->event;
    token->result = ev->event == BFA_kEnded ? BFA_kResultOK : ev->reason;
    token->animated = ev->event == BFA_kStarted;
    if (ev->event != BFA_kStarted) {
        token->terminal = true; state.tokens.erase(it);
    }
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
    const bool allowed = active && CanWriteActor(actor);
    if (allowed) ++fallbackChecks; else ++blockedChecks;
    return allowed;
}
#ifdef _WIN32
inline const BFA_Interface* Discover() {
    auto module = GetModuleHandleA("BetterFacialAnimations.dll");
    auto query = module ? reinterpret_cast<BFA_QueryInterfaceFn>(GetProcAddress(module, "BFA_QueryInterface")) : nullptr;
    auto api = query ? query(BFA_API_VERSION) : nullptr;
    return IsCompatible(api) ? api : nullptr;
}
#endif
class Line {
public:
    Line(const BFA_Interface* source, std::uint32_t actor) : token(std::make_shared<FaceToken>()) {
        token->actor = actor;
        api = Listen(source) ? source : nullptr;
        if (!api) return;
        utterance = api->NewUtteranceId();
        std::lock_guard<std::mutex> guard(State().mutex);
        State().tokens[utterance] = token;
    }
    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;
    ~Line() {
        Stop(BFA_kAborted);
        std::lock_guard<std::mutex> guard(State().mutex);
        if (utterance && !submitted) State().tokens.erase(utterance);
    }
    BFA_Result Prepare(const void* audio, std::uint32_t bytes, const char*, std::string_view expression) {
        if (!api) return last = BFA_kResultUnavailable;
        if (handle || !token->active) return last = BFA_kResultInvalidState;
        const auto mood = MakeMood(expression);
        BFA_Line line{BFA_API_VERSION, sizeof(BFA_Line), utterance, 0, token->actor, audio, bytes,
                      expression.empty() ? nullptr : &mood};
        handle = api->Prepare(&line, &last);
        submitted = handle != 0;
        return LastResult();
    }
    BFA_Result Start(double t) { return Ready() ? last = api->Start(handle, t) : LastResult(); }
    BFA_Result Pause() { return Ready() ? last = api->Pause(handle) : LastResult(); }
    BFA_Result Seek(double t) { return Ready() ? last = api->Seek(handle, t) : LastResult(); }
    BFA_Result Stop(std::uint32_t reason) {
        if (!token->active.exchange(false)) return LastResult();
        if (api && handle) {
            last = api->EndUtterance(utterance, reason);
            handle = 0;
        }
        return LastResult();
    }
    BFA_Result LastResult() const { return token->terminal ? token->result.load() : last; }
    std::uint64_t Handle() const { return handle; }
    std::shared_ptr<FaceToken> GetToken() const { return token; }
private:
    bool Ready() const { return api && handle && token->active && !token->terminal; }
    const BFA_Interface* api = nullptr;
    std::shared_ptr<FaceToken> token;
    bool submitted = false;
    std::uint64_t utterance = 0, handle = 0;
    BFA_Result last = BFA_kResultUnavailable;
};
} // namespace FacialAnimationBridge
