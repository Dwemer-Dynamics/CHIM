#include "PlaythroughSession.h"
#include "ChimInteraction.h"
#include "SPGResponse.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "RE/Skyrim.h"
#include "Misc.h"
#include "HTTPManager.h"
#include "ActorTargetIdentifierUtils.h"
namespace logger = SKSE::log;

static const ResponseItem defaultNullResponseItem = {"", 0};
static const auto qTtl=3000000000000;


std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    size_t end = str.find_last_not_of(" \t\r\n");

    if (start == std::string::npos || end == std::string::npos) return "";  // Empty or whitespace-only string

    return str.substr(start, end - start + 1);
}

SPGResponse& SPGResponse::getInstance() {
    static SPGResponse instance;
    return instance;
}

void SPGResponse::decodeAndEnqueue(const std::string& data, bool rechatGenerated) {
    if (!PlaythroughSession::Allowed(PlaythroughSession::Context())) return;
    std::stringstream ss(data);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find("|") == std::string::npos) continue;

        // label|queue|payload[|identity]. A present identity field is validated strictly; an invalid or
        // unsupported one drops the line instead of falling back to the label.
        auto parsed = EventIdentityUtils::ParseResponseLine(line);
        if (!parsed.Valid()) {
            logger::warn("[RESPONSE_IDENTITY] Dropping {} output for '{}': {}", parsed.queue, parsed.label,
                         parsed.invalidReason);
            continue;
        }

        std::string tkey(parsed.label + parsed.queue);
        if (tkey.empty()) return;
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
        auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

        ResponseItem fitem = {parsed.payload, nanos, parsed.label, rechatGenerated};
        fitem.identity = std::move(parsed.identity);

        this->enqueue(parsed.queue, fitem);
    }
}

void SPGResponse::moveFirstToLast(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];

    if (!responseQueue.empty()) {
        ResponseItem firstItem = responseQueue.front();
        responseQueue.pop_front();
        responseQueue.push_back(firstItem);
        logger::info("Moved to last {},{}", key, m_responses[key].size());
    }
}

void SPGResponse::eraseOldItems(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& queue = m_responses[key];

    auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

    auto oldestTimestamp = nanos - qTtl;
    // Remove all items from the queue that are older than oldestTimestamp
    while (!queue.empty() && queue.front().timestamp < oldestTimestamp) {
        queue.pop_front();
        logger::info("Expired {},{}", key, m_responses[key].size());

    }
}

void SPGResponse::clearGameOutput() {
    std::lock_guard lock(m_mutex);
    for (auto& [key, queue] : m_responses) {
        if (ChimInteraction::IsGameOutput(key)) queue.clear();
    }
}

namespace {
    bool IsAgentActionQueue(const std::string& key) {
        return key == "command" || key == "confirmcommand" || key == "approvedcommand";
    }

    bool NeedsCapture(const std::string& key, const ResponseItem& item) {
        return (IsAgentActionQueue(key) && !item.actorCaptured) ||
               (key == "rolecommand" && !item.roleTargetsCaptured) ||
               (item.identity.present && !item.identityCaptured);
    }

    ResponseItem RefusedCapture(const std::string& key, ResponseItem item) {
        item.bindingRefused = true;
        if (IsAgentActionQueue(key)) item.actorCaptured = true;
        if (key == "rolecommand") item.roleTargetsCaptured = true;
        if (item.identity.present) item.identityCaptured = true;
        return item;
    }
}

BoundActionActor SPGResponse::BindActor(RE::Actor* actor, std::size_t argIndex, bool bareName) {
    BoundActionActor bound;
    bound.argIndex = argIndex;
    bound.bareName = bareName;
    if (!actor || actor->IsDeleted()) return bound;
    bound.actor.actorKey = BuildActorKey(actor);
    // A dynamic actor without a key cannot be told apart from the next actor in its slot.
    if (ActorIdentityUtils::IsDynamicFormId(actor->GetFormID()) && bound.actor.actorKey.empty()) return bound;
    bound.actor.formId = actor->GetFormID();
    bound.handle = actor->GetHandle();
    return bound;
}

BoundActionActor SPGResponse::BindResponseEndpoint(const EventIdentityUtils::ResponseEndpoint& endpoint,
                                                   std::size_t argIndex) {
    // The narrator has no physical endpoint: its engine object is the player, so ref 14 never stands for it.
    if (!endpoint.IsPhysical() && !endpoint.IsPlayer()) return BindActor(nullptr, argIndex);
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(endpoint.refId);
    if (!actor || actor->IsDeleted() || actor->IsPlayerRef() != endpoint.IsPlayer()) {
        return BindActor(nullptr, argIndex);
    }
    // A dynamic slot is compared with the key it already holds, so a slot recycled after the server answered
    // is refused rather than minted a new identity that would then be accepted.
    if (ActorIdentityUtils::IsDynamicFormId(endpoint.refId) &&
        DynamicActorIdentity::KnownActorKey(endpoint.refId) != endpoint.id) {
        return BindActor(nullptr, argIndex);
    }
    auto bound = BindActor(actor, argIndex);
    if (bound.actor.actorKey != endpoint.id) return BindActor(nullptr, argIndex);
    return bound;
}

bool SPGResponse::StillMatchesResponseIdentity(const ResponseItem& item) {
    if (!item.identity.present) return true;
    if (!item.identityCaptured) return false;
    const auto& identity = item.identity;
    if ((identity.actor.IsPhysical() || identity.actor.IsPlayer()) && !StillSameBoundActor(item.identityActor)) {
        return false;
    }
    if ((identity.listener.IsPhysical() || identity.listener.IsPlayer()) &&
        !StillSameBoundActor(item.identityListener)) {
        return false;
    }
    if (item.identityTargets.size() != identity.targets.size()) return false;
    return std::all_of(item.identityTargets.begin(), item.identityTargets.end(),
                       [](const BoundActionActor& bound) { return StillSameBoundActor(bound); });
}

bool SPGResponse::StillSameBoundActor(const BoundActionActor& bound) {
    // Reads the handle captured at binding and the identity registry; no key is computed or assigned here.
    ActorIdentityUtils::BoundActorObservation now;
    if (const auto live = bound.handle.get()) {
        now.handleLive = true;
        now.handleFormId = live->GetFormID();
        now.deleted = live->IsDeleted();
        now.sameReference = RE::TESForm::LookupByID(now.handleFormId) == static_cast<RE::TESForm*>(live.get());
    }
    if (ActorIdentityUtils::IsDynamicFormId(bound.actor.formId)) {
        now.currentActorKey = DynamicActorIdentity::KnownActorKey(bound.actor.formId);
    }
    return ActorIdentityUtils::StillSameBoundActor(bound.actor, now);
}

bool SPGResponse::StillTargetsCapturedActor(const ResponseItem& item) {
    // Generation, not handshake readiness: a local action is not lost while the server reconnects.
    if (item.loadEpoch != PlaythroughSession::Generation()) {
        logger::info("[ACTION_IDENTITY] Dropping '{}' for {}; it was queued for another load", item.text, item.actor);
        return false;
    }
    if (item.bindingRefused) {
        logger::warn("[ACTION_IDENTITY] Dropping '{}' for {}; its actors could not be bound when it was queued",
                     item.text, item.actor);
        return false;
    }
    if (!StillMatchesResponseIdentity(item)) {
        logger::warn("[RESPONSE_IDENTITY] Dropping '{}' for {}; an actor its identity names changed since it "
                     "was queued", item.text, item.actor);
        return false;
    }
    if (item.actorCaptured) {
        // The agent is found by the bound FormID, then the physical actor behind it is checked: a cached profile
        // key alone would still match a stale entry whose FF slot now holds another actor.
        auto agent = item.actorFormId ? AIAgentManager::getInstance().getAgentByFormId(item.actorFormId) : nullptr;
        const auto parsed = ActorTargetIdentifierUtils::Parse(item.actor);
        // With an identity envelope the label is presentation only and is never looked up again.
        const bool namesAgent = agent && (item.identity.present
            ? item.identity.actor.IsPhysical() && item.identity.actor.refId == item.actorFormId
            : parsed.hasRefId
            ? parsed.refId == item.actorFormId
            : AIAgentManager::getInstance().getAgentByName(item.actor) == agent);
        if (!agent || agent->isNarrator() || !namesAgent || agent->getProfileKey() != item.actorProfileKey ||
            !StillSameBoundActor(item.agentActor)) {
            logger::warn("[ACTION_IDENTITY] Dropping '{}'; {} no longer resolves to the actor it was queued for",
                         item.text, item.actor);
            return false;
        }
    }
    if (item.roleTargetsCaptured && !RoleCommandTargetsStillBound(item)) {
        logger::warn("[ACTION_IDENTITY] Dropping role command '{}'; an actor it names changed since it was queued",
                     item.text);
        return false;
    }
    return true;
}

ResponseItem SPGResponse::CaptureActionTarget(const std::string& key, ResponseItem item) {
    // Keep the originating load and actor binding of a re-queued (confirmed/approved) action.
    if (item.loadEpoch == 0) item.loadEpoch = PlaythroughSession::Context();
    if (item.identity.present && !item.identityCaptured) {
        item.identityCaptured = true;
        const auto& identity = item.identity;
        bool bound = true;
        if (identity.actor.IsPhysical() || identity.actor.IsPlayer()) {
            item.identityActor = BindResponseEndpoint(identity.actor);
            bound = item.identityActor.actor.formId != 0;
        }
        if (bound && (identity.listener.IsPhysical() || identity.listener.IsPlayer())) {
            item.identityListener = BindResponseEndpoint(identity.listener);
            bound = item.identityListener.actor.formId != 0;
        }
        for (const auto& target : identity.targets) {
            if (!bound) break;
            item.identityTargets.push_back(BindResponseEndpoint(target.actor, target.arg));
            bound = item.identityTargets.back().actor.formId != 0;
        }
        if (bound && !identity.targets.empty() &&
            (!(IsAgentActionQueue(key) || key == "rolecommand") ||
             !ApplyResponseIdentityTargets(item, true, IsAgentActionQueue(key)))) {
            // Targets only belong to command arguments, and must agree with them. A bare label is rewritten to
            // the bound reference so the branch selects that actor, never whoever carries the label later.
            bound = false;
        }
        if (bound && identity.targets.empty() && IsAgentActionQueue(key) &&
            !ApplyResponseIdentityTargets(item, true, true)) {
            // An ordinary command whose actor argument carries no envelope target is refused, never relooked up.
            bound = false;
        }
        if (!bound) {
            logger::warn("[RESPONSE_IDENTITY] Refusing {} output '{}' for {}; an identity it names is not that "
                         "loaded actor now", key, item.text, item.actor);
            item.bindingRefused = true;
        }
    }
    if (IsAgentActionQueue(key) && !item.actorCaptured) {
        item.actorCaptured = true;
        // An envelope names the agent by its bound physical actor; only legacy output resolves its label.
        // A bare name only binds when unique; an ambiguous or missing actor binds to nothing and is dropped later.
        std::shared_ptr<AIAgent> envelopeAgent;
        if (item.identity.present && item.identityActor.actor.formId != 0 && item.identity.actor.IsPhysical()) {
            envelopeAgent = AIAgentManager::getInstance().getAgentByFormId(item.identityActor.actor.formId);
        }
        if (auto agent = item.identity.present ? envelopeAgent : AIAgentManager::getInstance().getAgentByName(item.actor);
            agent && !agent->isNarrator()) {
            const auto bound = BindActor(agent->getActor());
            // A dynamic agent's profile key is its dyn: key, so a stale entry over a recycled slot shows here.
            const bool sameActor = bound.actor.formId != 0 && bound.actor.formId == agent->GetFormId() &&
                (!ActorIdentityUtils::IsDynamicFormId(bound.actor.formId) ||
                 bound.actor.actorKey == agent->getProfileKey());
            if (sameActor) {
                item.actorFormId = bound.actor.formId;
                item.actorProfileKey = agent->getProfileKey();
                item.agentActor = bound;
            } else {
                logger::warn("[ACTION_IDENTITY] {} is a stale agent entry; '{}' binds to no actor", item.actor,
                             item.text);
            }
        }
    }
    if (key == "rolecommand" && !item.roleTargetsCaptured) {
        item.roleTargetsCaptured = true;
        CaptureRoleCommandTargets(item);
    }
    return item;
}

ResponseItem SPGResponse::CaptureActionTargetFromAnyThread(const std::string& key, ResponseItem item) {
    if (!NeedsCapture(key, item) || HTTPManager::OnGameThread()) return CaptureActionTarget(key, item);
    if (item.loadEpoch == 0) item.loadEpoch = PlaythroughSession::Context();
    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) return RefusedCapture(key, std::move(item));
    auto captured = std::make_shared<std::promise<ResponseItem>>();
    auto result = captured->get_future();
    tasks->AddTask([key, item, captured]() { captured->set_value(CaptureActionTarget(key, item)); });
    if (result.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        logger::warn("[ACTION_IDENTITY] Refusing '{}'; the game thread did not bind its actors in time", item.text);
        return RefusedCapture(key, std::move(item));
    }
    return result.get();
}

std::vector<ResponseItem> SPGResponse::CaptureActionTargetsFromAnyThread(const std::string& key,
                                                                         std::vector<ResponseItem> items) {
    // One game-thread task and one bounded wait for the whole batch, in order. On the game thread it binds inline,
    // so the caller never waits on the thread it is running on.
    for (auto& item : items) {
        if (item.loadEpoch == 0) item.loadEpoch = PlaythroughSession::Context();
    }
    const auto captureAll = [key](std::vector<ResponseItem> pending) {
        for (auto& item : pending) item = CaptureActionTarget(key, std::move(item));
        return pending;
    };
    const bool needed = std::any_of(items.begin(), items.end(),
                                    [&key](const ResponseItem& item) { return NeedsCapture(key, item); });
    if (!needed || HTTPManager::OnGameThread()) return captureAll(std::move(items));
    const auto refuseAll = [&key](std::vector<ResponseItem> pending) {
        for (auto& item : pending) item = RefusedCapture(key, std::move(item));
        return pending;
    };
    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) return refuseAll(std::move(items));
    auto captured = std::make_shared<std::promise<std::vector<ResponseItem>>>();
    auto result = captured->get_future();
    tasks->AddTask([items, captured, captureAll]() { captured->set_value(captureAll(items)); });
    if (result.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        logger::warn("[ACTION_IDENTITY] Refusing {} items; the game thread did not bind their actors in time",
                     items.size());
        return refuseAll(std::move(items));
    }
    return result.get();
}

namespace {
    // Off-thread items queued behind a game-thread capture. While any is pending, later items of the stream
    // (legacy lines included) take the same task path, so a line never overtakes one queued before it.
    std::atomic<int> g_pendingCaptures{0};
}

void SPGResponse::enqueue(const std::string& key, const ResponseItem& source) {
    if (!HTTPManager::OnGameThread() && (NeedsCapture(key, source) || g_pendingCaptures.load() > 0)) {
        // Engine records are read only on the game thread. The item keeps the load it arrived in, and items
        // keep their arrival order because tasks run in order; push drops any from an earlier load.
        ResponseItem pending = source;
        if (pending.loadEpoch == 0) pending.loadEpoch = PlaythroughSession::Context();
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            logger::warn("[ACTION_IDENTITY] Dropping '{}'; no game thread to bind its actors", pending.text);
            return;
        }
        ++g_pendingCaptures;
        tasks->AddTask([key, pending]() {
            SPGResponse::getInstance().push(key, CaptureActionTarget(key, pending));
            --g_pendingCaptures;
        });
        return;
    }
    push(key, CaptureActionTarget(key, source));
}

void SPGResponse::push(const std::string& key, const ResponseItem& item) {
    if (item.loadEpoch != PlaythroughSession::Generation()) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    if (ChimInteraction::IsGameOutput(key) && !ChimInteraction::Enabled()) return;
    m_responses[key].push_back(item);
    logger::info("Pushed {},{},{},{}", key, m_responses[key].size(),item.text,item.actor);
}

void SPGResponse::dequeue(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_responses[key].size() < 1) return;

    auto itemDeleted = m_responses[key].at(0);

    m_responses[key].pop_back();
    logger::info("Popped {},{},{}", key, m_responses[key].size(), itemDeleted.text);

    return;
}

void SPGResponse::dequeueFirst(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_responses[key].size() < 1) return;
    auto itemDeleted = m_responses[key].at(0);

    m_responses[key].erase(m_responses[key].begin());
    logger::info("Popped {},{},{}", key, m_responses[key].size(), itemDeleted.text);

    return;
}

std::deque<ResponseItem> SPGResponse::dequeue(const std::string& key, const std::string& text) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];
    std::deque<ResponseItem> filteredItems;

    for (auto it = responseQueue.begin(); it != responseQueue.end();) {
        if (it->text == text) {
            filteredItems.push_back(*it);
            it = responseQueue.erase(it);
        } else {
            ++it;
        }
    }

    return filteredItems;
}

ResponseItem SPGResponse::getLastItem(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];

    if (!responseQueue.empty()) {
        for (auto it = responseQueue.rbegin(); it != responseQueue.rend(); ++it) {
            auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
            auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            if (key != "HerikaAASPGDialogueHerika3Branch1Topic") { // One item queue, no expiry
                if ((nanos - it->timestamp) > qTtl) {
                    // Too old
                    continue;
                }
            }

            return *it;
        }
    }

    return ResponseItem{"", 0};
}


bool SPGResponse::findInQueue(const std::string& key,std::string needle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];

    if (!responseQueue.empty()) {
        for (auto it = responseQueue.rbegin(); it != responseQueue.rend(); ++it) {
            auto entry = *it;
            if (entry.text.contains(needle))
                return true;

          
        }
        return false;
    }

     return false;
}

int SPGResponse::getSize(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];

    return responseQueue.size();
}

void SPGResponse::markUnFinished(bool t) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    unfinished = t;
    
}

bool SPGResponse::isUnfinished() {
    std::lock_guard<std::mutex> lock(m_mutex);

    return unfinished;
}



ResponseItem SPGResponse::getFirstItem(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<ResponseItem>& responseQueue = m_responses[key];

    ResponseItem cursor;
    if (!responseQueue.empty()) {
        for (auto it = responseQueue.begin(); it != responseQueue.end(); ++it) {
            auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
            auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            if (key != "HerikaAASPGDialogueHerika3Branch1Topic") {  // One item queue, no expiry
                if ((nanos - it->timestamp) > qTtl) {
                    // Too old
                    continue;
                }
            }
           // logger::info("{}",it->text);
            
         cursor = *it;
         return cursor;

        }
    }

    return cursor;
}

void SPGResponse::clearQueue(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_responses.find(key);
    if (it != m_responses.end()) {
        it->second.clear();
    }
}

void SPGResponse::clearAllQueues() {
    std::lock_guard<std::mutex> lock(m_mutex);
    int counter;
    for (auto it = m_responses.begin(); it != m_responses.end(); ++it) {
        it->second.clear();
    }
    m_responses.clear();
    logger::info("All queues cleared");
}

namespace {
    struct ScriptProxyBindingEntry {
        std::uint64_t token = 0;
        std::uint64_t generation = 0;
        std::vector<std::pair<std::string, BoundActionActor>> actors;
        bool refused = false;  // Once any binding failed, every later getter call of the command is refused.
    };
    std::mutex g_scriptProxyMutex;
    std::deque<ScriptProxyBindingEntry> g_scriptProxyBindings;
    std::uint64_t g_scriptProxyNextToken = 1;
}

std::uint64_t ScriptProxyBindings::Register(std::vector<std::pair<std::string, BoundActionActor>> actors) {
    std::lock_guard lock(g_scriptProxyMutex);
    ScriptProxyBindingEntry entry;
    entry.token = g_scriptProxyNextToken++;
    entry.generation = PlaythroughSession::Generation();
    entry.actors = std::move(actors);
    g_scriptProxyBindings.push_back(std::move(entry));
    while (g_scriptProxyBindings.size() > 64) g_scriptProxyBindings.pop_front();
    return g_scriptProxyBindings.back().token;
}

bool ScriptProxyBindings::Allows(const nlohmann::json& command, const std::string& key, std::uint32_t requested) {
    const auto tokenField = command.find(std::string(EventIdentityUtils::ScriptProxyBindingKey));
    if (tokenField == command.end()) return true;  // Local UI and legacy calls: unchanged resolution.
    if (!tokenField->is_number_unsigned()) return false;
    const auto token = tokenField->get<std::uint64_t>();
    std::lock_guard lock(g_scriptProxyMutex);
    const auto entry = std::find_if(g_scriptProxyBindings.begin(), g_scriptProxyBindings.end(),
                                    [token](const auto& e) { return e.token == token; });
    if (entry == g_scriptProxyBindings.end() || entry->refused ||
        entry->generation != PlaythroughSession::Generation()) {
        return false;
    }
    for (const auto& [name, bound] : entry->actors) {
        if (!SPGResponse::StillSameBoundActor(bound)) {
            logger::warn("[SCRIPTPROXY_IDENTITY] Refusing command {}; parameter {} no longer names its bound actor",
                         token, name);
            entry->refused = true;
            return false;
        }
    }
    const auto bound = std::find_if(entry->actors.begin(), entry->actors.end(),
                                    [&key](const auto& b) { return b.first == key; });
    if (bound != entry->actors.end()) return requested == bound->second.actor.formId;
    // An unbound parameter may name a non-actor reference, never an actor it was not bound to.
    auto* form = requested ? RE::TESForm::LookupByID(requested) : nullptr;
    if (form && form->As<RE::Actor>()) {
        logger::warn("[SCRIPTPROXY_IDENTITY] Refusing command {}; parameter {} names an unbound actor {:08X}", token,
                     key, requested);
        entry->refused = true;
        return false;
    }
    return true;
}
