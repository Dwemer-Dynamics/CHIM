#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ActorIdentityUtils.h"
#include "EventIdentityUtils.h"

// A physical actor an action names, bound on the game thread when the action is first queued.
struct BoundActionActor {
    ActorIdentityUtils::BoundActor actor;
    RE::ActorHandle handle;
    std::size_t argIndex = 0;
    bool bareName = false;  // Bound from a bare label, so the branch's own resolution must still pick it.
    bool unresolved = false;  // A bare label that named nobody at capture; it may never bind someone later.
};

struct ResponseItem {
    std::string text;
    long long timestamp;
    std::string actor;
    bool rechatGenerated = false;
    // Captured when an action is first queued and carried through confirmation/approval re-queues, so a later
    // load or a recycled FF slot holding a new actor cannot receive it.
    std::uint64_t loadEpoch = 0;
    bool actorCaptured = false;
    std::uint32_t actorFormId = 0;
    std::string actorProfileKey;
    // The physical actor behind the agent, so a stale agent entry over a recycled FF slot cannot pass.
    BoundActionActor agentActor;
    // Existing actors named by a rolecommand's arguments.
    bool roleTargetsCaptured = false;
    std::vector<BoundActionActor> roleTargets;
    // An explicit reference that did not resolve, or a capture that could not run on the game thread.
    bool bindingRefused = false;
    // Response identity v1 (the line's fourth field). When present it is the only routing authority: the label
    // is presentation, the listener is never filled from the payload, and every endpoint is bound on the game
    // thread before the item is queued and checked again before dispatch or playback.
    EventIdentityUtils::ResponseIdentity identity;
    bool identityCaptured = false;
    BoundActionActor identityActor;     // Physical or player actor; unbound for the typed narrator.
    BoundActionActor identityListener;  // Unbound when the listener is null or the narrator.
    std::vector<BoundActionActor> identityTargets;  // One per envelope target, argIndex = its argument.
    // ScriptProxy: JSON parameter names of roleTargets (argIndex indexes this list).
    std::vector<std::string> scriptProxyKeys;
};

// ScriptProxy actor bindings, handed to Papyrus as an opaque token inside the command JSON so the native json
// getters return only the bound physical actors.
namespace ScriptProxyBindings {
    // Bound actors of one dispatched command; returns the token to embed under ScriptProxyBindingKey.
    std::uint64_t Register(std::vector<std::pair<std::string, BoundActionActor>> actors);
    // For jsonGetActor/jsonGetReference: `requested` is the RefID the parameter names. Returns true when the
    // getter may hand that reference to Papyrus. A command without a token keeps legacy resolution; with one,
    // every bound actor must still be the same and an actor parameter must be one of them.
    bool Allows(const nlohmann::json& command, const std::string& key, std::uint32_t requested);
}

// Implemented next to parseRoleCommand. Game thread only: binds the existing actors a rolecommand names.
void CaptureRoleCommandTargets(ResponseItem& item);
// Implemented next to parseRoleCommand. Game thread only, after the identity envelope is bound: checks each
// envelope target against its argument text (a decorated or numeric reference must name the same RefID) and,
// when rewriteBareArgs, replaces a bare label with the exact identifier so the branch cannot pick a namesake.
// Returns false when the envelope and the arguments disagree.
// ordinaryCommand selects the agent "command" queue map (OrdinaryCommandActorArgs) instead of the rolecommand one.
bool ApplyResponseIdentityTargets(ResponseItem& item, bool rewriteBareArgs, bool ordinaryCommand);
// Any thread: the bound role targets are still the same actors and their branches still resolve to them.
bool RoleCommandTargetsStillBound(const ResponseItem& item);
// Dispatches a queued rolecommand with its captured bindings (ScriptProxy hands them to Papyrus).
void parseRoleCommand(const ResponseItem& item);

class SPGResponse {
public:
    static SPGResponse& getInstance();

    void enqueue(const std::string& key, const ResponseItem& item);
    void dequeue(const std::string& key);
    void dequeueFirst(const std::string& key);
    std::deque<ResponseItem> dequeue(const std::string& key, const std::string& text);
    ResponseItem getLastItem(const std::string& key);
    int getSize(const std::string& key);
    ResponseItem getFirstItem(const std::string& key);
    void decodeAndEnqueue(const std::string& data, bool rechatGenerated = false);
    void eraseOldItems(const std::string& key);
    void clearQueue(const std::string& key);
    void moveFirstToLast(const std::string& key);
    void clearAllQueues();
    void clearGameOutput();

    // True while the item's load is current and, for an action bound to an agent, the same agent (FormID and
    // persistent profile key) still resolves from its actor identifier.
    static bool StillTargetsCapturedActor(const ResponseItem& item);
    // Game thread only. Fills the load and actor binding enqueue() records; a re-queued item keeps its binding.
    static ResponseItem CaptureActionTarget(const std::string& key, ResponseItem item);
    // For callers off the game thread that hold an action outside the queues: waits briefly for the
    // game-thread capture and returns a refused binding if it cannot run.
    static ResponseItem CaptureActionTargetFromAnyThread(const std::string& key, ResponseItem item);
    // The same for a whole ordered batch: one game-thread task and one bounded wait, never one per item.
    static std::vector<ResponseItem> CaptureActionTargetsFromAnyThread(const std::string& key,
                                                                       std::vector<ResponseItem> items);
    // Any thread, without computing identity: the bound handle still names the same reference and key.
    static bool StillSameBoundActor(const BoundActionActor& bound);
    // Game thread only: binds an envelope endpoint when its canonical key still names the reference it gives.
    // Returns an unbound actor when the reference is missing, deleted or now carries another key.
    static BoundActionActor BindResponseEndpoint(const EventIdentityUtils::ResponseEndpoint& endpoint,
                                                 std::size_t argIndex = 0);
    // Any thread: the envelope's endpoints still hold the actors bound when the item was queued.
    static bool StillMatchesResponseIdentity(const ResponseItem& item);
    // Game thread only.
    static BoundActionActor BindActor(RE::Actor* actor, std::size_t argIndex = 0, bool bareName = false);

    void markUnFinished(bool t);
    bool isUnfinished();


    bool findInQueue(const std::string& key, std::string needle);
    private:
    void push(const std::string& key, const ResponseItem& item);
    SPGResponse() {}
    SPGResponse(const SPGResponse&) = delete;
    SPGResponse& operator=(const SPGResponse&) = delete;

    std::mutex m_mutex;
    std::unordered_map<std::string, std::deque<ResponseItem>> m_responses;
    bool unfinished = false;
};

class SPGMessage {
public:
    static SPGMessage& getInstance() {
        static SPGMessage instance;
        return instance;
    }

    void enqueue(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(message);
    }

    void enqueue2(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue2.push(message);
    }

    void enqueue3(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue3.push(message);
    }

    std::string dequeue() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            return "";
        }
        std::string message = m_queue.front();
        m_queue.pop();
        return message;
    }

    std::string dequeue2() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue2.empty()) {
            return "";
        }
        std::string message = m_queue2.front();
        m_queue2.pop();
        return message;
    }

    std::string dequeue3() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue3.empty()) {
            return "";
        }
        std::string message = m_queue3.front();
        m_queue3.pop();
        return message;
    }

private:
    std::queue<std::string> m_queue;
    std::queue<std::string> m_queue2;
    std::queue<std::string> m_queue3;
    std::mutex m_mutex;

    SPGMessage() = default;
    ~SPGMessage() = default;
    SPGMessage(const SPGMessage&) = delete;
    SPGMessage& operator=(const SPGMessage&) = delete;
};

// Helper trim
std::string trim(const std::string& str);
