#include "VRItemAwareness.h"

#include "HTTPManager.h"
#include "ItemIdentifierUtils.h"
#include "Misc.h"
#include "ThreadPool.h"

#include "RE/T/TESGrabReleaseEvent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Globals.h"

namespace
{
    struct HiggsMessage
    {
        static constexpr std::uint32_t kMessage_GetInterface = 0xF9279A57;

        void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
    };

    struct IHiggsInterface001
    {
        using PulledCallback = void (*)(bool isLeft, RE::TESObjectREFR* pulledRefr);
        using GrabbedCallback = void (*)(bool isLeft, RE::TESObjectREFR* grabbedRefr);
        using DroppedCallback = void (*)(bool isLeft, RE::TESObjectREFR* droppedRefr);
        using StashedCallback = void (*)(bool isLeft, RE::TESForm* stashedForm);
        using ConsumedCallback = void (*)(bool isLeft, RE::TESForm* consumedForm);

        virtual unsigned int GetBuildNumber() = 0;
        virtual void AddPulledCallback(PulledCallback callback) = 0;
        virtual void AddGrabbedCallback(GrabbedCallback callback) = 0;
        virtual void AddDroppedCallback(DroppedCallback callback) = 0;
        virtual void AddStashedCallback(StashedCallback callback) = 0;
        virtual void AddConsumedCallback(ConsumedCallback callback) = 0;
    };

    std::atomic_bool g_initialized{ false };
    std::atomic_bool g_flatPollingEnabled{ false };
    IHiggsInterface001* g_higgsInterface = nullptr;

    std::mutex g_debounceMutex;
    std::unordered_map<std::string, std::string> g_lastItemBySlot;
    std::unordered_map<std::string, std::string> g_lastActionBySlot;
    std::unordered_map<std::string, RE::FormID> g_lastRefIdBySlot;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_lastEventBySlot;

    std::mutex g_heldItemMutex;
    ItemIdentifierUtils::HeldItemTracker g_heldItems;

    std::mutex g_flatHeldMutex;
    RE::FormID g_flatHeldRefFormId = 0;
    std::string g_flatHeldItemName;
    std::chrono::steady_clock::time_point g_lastFlatPoll{};

    constexpr const char* kVrItemEventName = "ext_vr_item_raw";
    // Plain name per upstream review (tyler.maister 2026-07-09): DLL-emitted events are not ext_*.
    // Core lists "physics_raw" as a fast command (log-only); the SHARMAT server extension opts in
    // by renaming it to its internal ext_nsfw_physics_raw in preprocessing.
    constexpr const char* kNsfwPhysicsEventName = "physics_raw";
    constexpr auto kBodyContactPollInterval = std::chrono::milliseconds(33);
    constexpr auto kButtImpactCooldown = std::chrono::milliseconds(1200);
    constexpr float kButtContactEnterDistance = 18.0f;
    constexpr float kButtContactExitDistance = 27.0f;
    constexpr float kButtContactPreviousDistance = 22.0f;
    constexpr float kButtImpactMinSpeed = 40.0f;
    constexpr float kButtImpactMaxSpeed = 1200.0f;
    constexpr float kBodyContactMaxActorDistance = 260.0f;

    struct HandMotionState
    {
        bool initialized = false;
        RE::NiPoint3 position{};
        std::chrono::steady_clock::time_point sampledAt{};
    };

    struct HandMotionSnapshot
    {
        bool valid = false;
        bool hasPrevious = false;
        RE::NiPoint3 position{};
        RE::NiPoint3 previousPosition{};
        float speed = 0.0f;
    };

    std::mutex g_bodyContactMutex;
    std::array<HandMotionState, 2> g_handMotionStates{};
    std::unordered_map<std::string, bool> g_insideButtContactZone;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_lastButtImpactByKey;
    std::chrono::steady_clock::time_point g_lastBodyContactPoll{};

    std::string CleanRawField(std::string value)
    {
        replaceAll(value, "^", " ");
        replaceAll(value, "|", " ");
        replaceAll(value, "\r", " ");
        replaceAll(value, "\n", " ");
        replaceAll(value, "\t", " ");
        return value;
    }

    bool IsFinitePoint(const RE::NiPoint3& point)
    {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    }

    float Distance(const RE::NiPoint3& lhs, const RE::NiPoint3& rhs)
    {
        return lhs.GetDistance(rhs);
    }

    RE::NiAVObject* ResolveHandNode(RE::VR_NODE_DATA* nodeData, bool isLeft)
    {
        if (!nodeData) {
            return nullptr;
        }

        if (isLeft) {
            if (nodeData->LeftWandNode) return nodeData->LeftWandNode.get();
            if (nodeData->LeftValveIndexControllerNode) return nodeData->LeftValveIndexControllerNode.get();
            if (nodeData->LeftWandShakeNode) return nodeData->LeftWandShakeNode.get();
            if (nodeData->NPCLHnd) return nodeData->NPCLHnd.get();
        } else {
            if (nodeData->RightWandNode) return nodeData->RightWandNode.get();
            if (nodeData->RightValveIndexControllerNode) return nodeData->RightValveIndexControllerNode.get();
            if (nodeData->RightWandShakeNode) return nodeData->RightWandShakeNode.get();
            if (nodeData->NPCRHnd) return nodeData->NPCRHnd.get();
        }

        return nullptr;
    }

    HandMotionSnapshot SampleHandMotion(int handIndex, const RE::NiPoint3& position,
                                        std::chrono::steady_clock::time_point now)
    {
        HandMotionSnapshot snapshot;
        if (handIndex < 0 || handIndex >= static_cast<int>(g_handMotionStates.size()) || !IsFinitePoint(position)) {
            return snapshot;
        }

        auto& state = g_handMotionStates[handIndex];
        snapshot.valid = true;
        snapshot.position = position;

        if (state.initialized) {
            const auto delta = now - state.sampledAt;
            const float deltaSeconds = std::chrono::duration<float>(delta).count();
            if (deltaSeconds > 0.001f && deltaSeconds < 1.0f && IsFinitePoint(state.position)) {
                snapshot.hasPrevious = true;
                snapshot.previousPosition = state.position;
                snapshot.speed = Distance(position, state.position) / deltaSeconds;
            }
        }

        state.initialized = true;
        state.position = position;
        state.sampledAt = now;
        return snapshot;
    }

    void CollectNodePositions(RE::NiAVObject* root, const std::vector<const char*>& names,
                              std::vector<RE::NiPoint3>& out)
    {
        if (!root) {
            return;
        }

        for (const auto* name : names) {
            auto* node = root->GetObjectByName(name);
            if (!node) {
                continue;
            }

            const auto position = node->world.translate;
            if (IsFinitePoint(position)) {
                out.push_back(position);
            }
        }
    }

    std::vector<RE::NiPoint3> GetButtContactPoints(RE::Actor* actor)
    {
        std::vector<RE::NiPoint3> points;
        if (!actor || !actor->Is3DLoaded()) {
            return points;
        }

        auto* root = actor->Get3D();
        if (!root) {
            return points;
        }

        static const std::vector<const char*> buttNodeNames = {
            "NPC L Butt [LButt]",
            "NPC R Butt [RButt]",
            "NPC L Butt",
            "NPC R Butt",
            "NPC Butt [Butt]",
            "NPC Butt"
        };

        CollectNodePositions(root, buttNodeNames, points);
        return points;
    }

    std::string MakeBodyContactKey(RE::FormID actorFormId, const std::string& hand)
    {
        return std::format("{:08X}:{}", actorFormId, hand);
    }

    bool ShouldEmitButtImpact(RE::FormID actorFormId, const std::string& hand,
                              float previousDistance, float currentDistance, float speed,
                              std::chrono::steady_clock::time_point now)
    {
        const std::string key = MakeBodyContactKey(actorFormId, hand);
        const bool wasInside = g_insideButtContactZone[key];

        if (currentDistance >= kButtContactExitDistance) {
            g_insideButtContactZone[key] = false;
            return false;
        }

        if (currentDistance > kButtContactEnterDistance) {
            return false;
        }

        g_insideButtContactZone[key] = true;

        const bool crossedIntoButtZone =
            !wasInside &&
            previousDistance >= kButtContactPreviousDistance &&
            currentDistance <= kButtContactEnterDistance;

        const bool speedLooksLikeSwat = speed >= kButtImpactMinSpeed && speed <= kButtImpactMaxSpeed;
        if (!crossedIntoButtZone || !speedLooksLikeSwat) {
            return false;
        }

        const auto lastIt = g_lastButtImpactByKey.find(key);
        if (lastIt != g_lastButtImpactByKey.end() && now - lastIt->second < kButtImpactCooldown) {
            return false;
        }

        g_lastButtImpactByKey[key] = now;
        return true;
    }

    void SendButtImpactEvent(std::string actorName, const std::string& hand, float speed)
    {
        actorName = CleanRawField(std::move(actorName));
        if (actorName.empty()) {
            return;
        }

        const std::string rawData = std::format("{}^Butt^spank^false^^{}^^{:.1f}", actorName, hand, speed);
        logger::info("[VRBodyContact] butt impact classified: actor='{}' hand={} speed={:.1f}", actorName, hand, speed);

        ThreadPool::getInstance().enqueue(
            "VRBodyContact",
            [rawData]() {
                HTTPManager::log(std::format("{}|{}|{}|{}",
                                             kNsfwPhysicsEventName,
                                             getCurrentTimeMillis(),
                                             GetGameTimeStamp(),
                                             rawData));
            },
            actorName + ":" + hand + ":butt_impact");
    }

    std::string ResolveFormName(RE::TESForm* form)
    {
        if (!form) {
            return "";
        }

        const char* name = form->GetName();
        return name ? std::string(name) : "";
    }

    std::string ResolveItemName(RE::TESObjectREFR* ref)
    {
        if (!ref) {
            return "";
        }

        std::string itemName = ref->GetDisplayFullName();
        if (itemName.empty()) {
            itemName = ResolveFormName(ref->GetBaseObject());
        }

        return CleanRawField(itemName.empty() ? "something" : itemName);
    }

    bool ShouldSkipReference(RE::TESObjectREFR* ref)
    {
        if (!ref) {
            return true;
        }

        if (ref->As<RE::Actor>()) {
            return true;
        }

        auto* baseForm = ref->GetBaseObject();
        if (!baseForm) {
            return false;
        }

        const auto formType = baseForm->GetFormType();
        return formType == RE::FormType::NPC || formType == RE::FormType::ActorCharacter;
    }

    bool ShouldDebounce(const std::string& slot, const std::string& action, const std::string& itemName,
                        RE::FormID refId)
    {
        const auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(g_debounceMutex);
        const bool duplicate =
            g_lastActionBySlot[slot] == action &&
            g_lastItemBySlot[slot] == itemName &&
            g_lastRefIdBySlot[slot] == refId &&
            now - g_lastEventBySlot[slot] < std::chrono::milliseconds(500);

        if (!duplicate) {
            g_lastActionBySlot[slot] = action;
            g_lastItemBySlot[slot] = itemName;
            g_lastRefIdBySlot[slot] = refId;
            g_lastEventBySlot[slot] = now;
        }

        return duplicate;
    }

    void SetHeldItemState(const std::string& slot, RE::FormID refId, std::string itemName)
    {
        std::lock_guard<std::mutex> lock(g_heldItemMutex);
        g_heldItems.Set(slot, refId, std::move(itemName));
    }

    ItemIdentifierUtils::HeldItemState GetHeldItemState(const std::string& slot)
    {
        std::lock_guard<std::mutex> lock(g_heldItemMutex);
        return g_heldItems.Get(slot);
    }

    void ClearHeldItemState(const std::string& slot)
    {
        std::lock_guard<std::mutex> lock(g_heldItemMutex);
        g_heldItems.Clear(slot);
    }

    void SendHeldItemEvent(const std::string& slot, const std::string& action, std::string itemName,
                           RE::FormID refId)
    {
        if (itemName.empty()) {
            itemName = "something";
        }

        itemName = CleanRawField(itemName);
        if (ShouldDebounce(slot, action, itemName, refId)) {
            return;
        }

        const std::string rawData = ItemIdentifierUtils::BuildHeldItemEvent(itemName, action, slot, refId);
        logger::info("[VRItemAwareness] {} {} ({}, RefID=0x{:08X})", action, itemName, slot, refId);

        ThreadPool::getInstance().enqueue(
            "VRItemAwareness",
            [rawData]() {
                HTTPManager::log(std::format("{}|{}|{}|{}",
                                             kVrItemEventName,
                                             getCurrentTimeMillis(),
                                             GetGameTimeStamp(),
                                             rawData));
            },
            slot + ":" + action);
    }

    void SetFlatHeldState(RE::FormID formId, std::string itemName)
    {
        std::lock_guard<std::mutex> lock(g_flatHeldMutex);
        g_flatHeldRefFormId = formId;
        g_flatHeldItemName = std::move(itemName);
    }

    std::pair<RE::FormID, std::string> GetFlatHeldState()
    {
        std::lock_guard<std::mutex> lock(g_flatHeldMutex);
        return { g_flatHeldRefFormId, g_flatHeldItemName };
    }

    void ClearFlatHeldState()
    {
        SetFlatHeldState(0, "");
    }

    void SendFlatDrop();

    void SendFlatPickup(RE::TESObjectREFR* ref)
    {
        if (ShouldSkipReference(ref)) {
            SendFlatDrop();
            return;
        }

        const auto formId = ref->GetFormID();
        const std::string itemName = ResolveItemName(ref);
        auto [heldFormId, heldItemName] = GetFlatHeldState();

        if (heldFormId != 0 && heldFormId != formId) {
            SendHeldItemEvent("both", "drop", heldItemName, heldFormId);
        }

        if (heldFormId != formId) {
            SendHeldItemEvent("both", "pickup", itemName, formId);
            SetFlatHeldState(formId, itemName);
        }
    }

    void SendFlatDrop()
    {
        auto [heldFormId, heldItemName] = GetFlatHeldState();
        if (heldFormId == 0) {
            return;
        }

        SendHeldItemEvent("both", "drop", heldItemName, heldFormId);
        ClearFlatHeldState();
    }

    void OnHiggsGrabbed(bool isLeft, RE::TESObjectREFR* grabbedRef)
    {
        if (ShouldSkipReference(grabbedRef)) {
            return;
        }

        const std::string slot = isLeft ? "left" : "right";
        const auto refId = grabbedRef->GetFormID();
        const auto itemName = ResolveItemName(grabbedRef);
        SendHeldItemEvent(slot, "pickup", itemName, refId);
        SetHeldItemState(slot, refId, itemName);
    }

    void OnHiggsDropped(bool isLeft, RE::TESObjectREFR* droppedRef)
    {
        if (ShouldSkipReference(droppedRef)) {
            return;
        }

        const std::string slot = isLeft ? "left" : "right";
        auto state = GetHeldItemState(slot);
        const auto refId = droppedRef->GetFormID() != 0 ? droppedRef->GetFormID() : state.refId;
        auto itemName = ResolveItemName(droppedRef);
        if (itemName.empty() || itemName == "something") {
            itemName = state.name;
        }
        SendHeldItemEvent(slot, "drop", itemName, refId);
        ClearHeldItemState(slot);
    }

    void OnHiggsStashed(bool isLeft, RE::TESForm* stashedForm)
    {
        const std::string slot = isLeft ? "left" : "right";
        auto state = GetHeldItemState(slot);
        const auto itemName = !state.name.empty() ? state.name : ResolveFormName(stashedForm);
        // The stash callback only exposes the base form. Never mislabel that BaseID as a world RefID.
        const auto refId = state.refId;
        SendHeldItemEvent(slot, "drop", itemName, refId);
        ClearHeldItemState(slot);
    }

    void OnHiggsConsumed(bool isLeft, RE::TESForm* consumedForm)
    {
        const std::string slot = isLeft ? "left" : "right";
        auto state = GetHeldItemState(slot);
        const auto itemName = !state.name.empty() ? state.name : ResolveFormName(consumedForm);
        // The consume callback only exposes the base form. Reuse the RefID captured by the grab callback.
        const auto refId = state.refId;
        SendHeldItemEvent(slot, "drop", itemName, refId);
        ClearHeldItemState(slot);
    }

    void TickBodyImpactAwareness()
    {
        if (!REL::Module::IsVR()) {
            return;
        }

        // No body-contact turns while the player is in a scripted conversation - same
        // quest-breaking hazard as gaze: the spoken reaction stomps the vanilla dialogue
        // state and the NPC can wedge "busy" mid-quest.
        if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(g_bodyContactMutex);
            if (now - g_lastBodyContactPoll < kBodyContactPollInterval) {
                return;
            }
            g_lastBodyContactPoll = now;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* nodeData = player ? player->GetVRNodeData() : nullptr;
        if (!player || !nodeData) {
            return;
        }

        struct HandCandidate
        {
            std::string hand;
            int index = 0;
            RE::NiAVObject* node = nullptr;
        };

        const std::array<HandCandidate, 2> hands = {{
            { "left", 0, ResolveHandNode(nodeData, true) },
            { "right", 1, ResolveHandNode(nodeData, false) }
        }};

        std::array<HandMotionSnapshot, 2> snapshots{};
        {
            std::lock_guard<std::mutex> lock(g_bodyContactMutex);
            for (const auto& hand : hands) {
                if (!hand.node) {
                    continue;
                }
                const auto position = hand.node->world.translate;
                snapshots[hand.index] = SampleHandMotion(hand.index, position, now);
            }
        }

        auto agents = AIAgentManager::getInstance().getAgents();
        for (const auto& agent : agents) {
            if (!agent) {
                continue;
            }

            auto* actor = agent->getActor();
            if (!actor || actor == player || !actor->Is3DLoaded()) {
                continue;
            }

            if (Distance(actor->GetPosition(), player->GetPosition()) > kBodyContactMaxActorDistance) {
                continue;
            }

            const auto buttPoints = GetButtContactPoints(actor);
            if (buttPoints.empty()) {
                continue;
            }

            for (const auto& hand : hands) {
                const auto& snapshot = snapshots[hand.index];
                if (!snapshot.valid || !snapshot.hasPrevious) {
                    continue;
                }

                float currentDistance = std::numeric_limits<float>::max();
                float previousDistance = std::numeric_limits<float>::max();
                for (const auto& point : buttPoints) {
                    currentDistance = std::min(currentDistance, Distance(snapshot.position, point));
                    previousDistance = std::min(previousDistance, Distance(snapshot.previousPosition, point));
                }

                bool emit = false;
                {
                    std::lock_guard<std::mutex> lock(g_bodyContactMutex);
                    emit = ShouldEmitButtImpact(actor->GetFormID(), hand.hand, previousDistance, currentDistance,
                                                snapshot.speed, now);
                }

                if (emit) {
                    SendButtImpactEvent(agent->getActorName(), hand.hand, snapshot.speed);
                }
            }
        }
    }

    IHiggsInterface001* GetHiggsInterface()
    {
        if (g_higgsInterface) {
            return g_higgsInterface;
        }

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            logger::warn("[VRItemAwareness] SKSE messaging interface unavailable");
            return nullptr;
        }

        HiggsMessage message;
        if (!messaging->Dispatch(HiggsMessage::kMessage_GetInterface,
                                 &message,
                                 static_cast<std::uint32_t>(sizeof(message)),
                                 "HIGGS")) {
            return nullptr;
        }

        if (!message.GetApiFunction) {
            return nullptr;
        }

        g_higgsInterface = static_cast<IHiggsInterface001*>(message.GetApiFunction(1));
        return g_higgsInterface;
    }
}

namespace VRItemAwareness
{
    void Initialize()
    {
        bool expected = false;
        if (!g_initialized.compare_exchange_strong(expected, true)) {
            return;
        }

        if (REL::Module::GetRuntime() != REL::Module::Runtime::VR) {
            g_flatPollingEnabled = true;
            logger::info("[VRItemAwareness] Non-VR runtime detected; flat held item awareness enabled");
            return;
        }

        auto* higgs = GetHiggsInterface();
        if (!higgs) {
            logger::info("[VRItemAwareness] HIGGS interface unavailable; VR held item awareness disabled");
            return;
        }

        higgs->AddGrabbedCallback(OnHiggsGrabbed);
        higgs->AddDroppedCallback(OnHiggsDropped);
        higgs->AddStashedCallback(OnHiggsStashed);
        higgs->AddConsumedCallback(OnHiggsConsumed);

        logger::info("[VRItemAwareness] HIGGS item awareness enabled, build {}", higgs->GetBuildNumber());
        logger::info("[VRBodyContact] VR butt impact classifier enabled");
    }

    void Tick()
    {
        TickBodyImpactAwareness();

        if (!g_flatPollingEnabled.load(std::memory_order_acquire)) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastFlatPoll < std::chrono::milliseconds(250)) {
            return;
        }
        g_lastFlatPoll = now;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            SendFlatDrop();
            return;
        }

        auto grabbedRef = player->GetGrabbedRef();
        if (grabbedRef) {
            SendFlatPickup(grabbedRef.get());
        } else {
            SendFlatDrop();
        }
    }

    void HandleFlatGrabReleaseEvent(const RE::TESGrabReleaseEvent* event)
    {
        if (!g_flatPollingEnabled.load(std::memory_order_acquire) || !event) {
            return;
        }

        if (event->grabbed) {
            SendFlatPickup(event->ref.get());
        } else {
            SendFlatDrop();
        }
    }
}
