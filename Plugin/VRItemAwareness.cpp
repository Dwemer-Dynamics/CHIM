#include "VRItemAwareness.h"

#include "HTTPManager.h"
#include "Misc.h"
#include "ThreadPool.h"

#include "RE/T/TESGrabReleaseEvent.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

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
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_lastEventBySlot;

    std::mutex g_flatHeldMutex;
    RE::FormID g_flatHeldRefFormId = 0;
    std::string g_flatHeldItemName;
    std::chrono::steady_clock::time_point g_lastFlatPoll{};

    std::string CleanRawField(std::string value)
    {
        replaceAll(value, "^", " ");
        replaceAll(value, "|", " ");
        replaceAll(value, "\r", " ");
        replaceAll(value, "\n", " ");
        replaceAll(value, "\t", " ");
        return value;
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

    bool ShouldDebounce(const std::string& slot, const std::string& action, const std::string& itemName)
    {
        const auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(g_debounceMutex);
        const bool duplicate =
            g_lastActionBySlot[slot] == action &&
            g_lastItemBySlot[slot] == itemName &&
            now - g_lastEventBySlot[slot] < std::chrono::milliseconds(500);

        if (!duplicate) {
            g_lastActionBySlot[slot] = action;
            g_lastItemBySlot[slot] = itemName;
            g_lastEventBySlot[slot] = now;
        }

        return duplicate;
    }

    void SendHeldItemEvent(const std::string& slot, const std::string& action, std::string itemName)
    {
        if (itemName.empty()) {
            itemName = "something";
        }

        itemName = CleanRawField(itemName);
        if (ShouldDebounce(slot, action, itemName)) {
            return;
        }

        const std::string rawData = std::format("{}^{}^{}", itemName, action, slot);

        ThreadPool::getInstance().enqueue(
            "VRItemAwareness",
            [rawData]() {
                HTTPManager::log(std::format("ext_held_item_raw|{}|{}|{}",
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
            SendHeldItemEvent("both", "drop", heldItemName);
        }

        if (heldFormId != formId) {
            SendHeldItemEvent("both", "pickup", itemName);
            SetFlatHeldState(formId, itemName);
        }
    }

    void SendFlatDrop()
    {
        auto [heldFormId, heldItemName] = GetFlatHeldState();
        if (heldFormId == 0) {
            return;
        }

        SendHeldItemEvent("both", "drop", heldItemName);
        ClearFlatHeldState();
    }

    void OnHiggsGrabbed(bool isLeft, RE::TESObjectREFR* grabbedRef)
    {
        if (ShouldSkipReference(grabbedRef)) {
            return;
        }

        SendHeldItemEvent(isLeft ? "left" : "right", "pickup", ResolveItemName(grabbedRef));
    }

    void OnHiggsDropped(bool isLeft, RE::TESObjectREFR* droppedRef)
    {
        if (ShouldSkipReference(droppedRef)) {
            return;
        }

        SendHeldItemEvent(isLeft ? "left" : "right", "drop", ResolveItemName(droppedRef));
    }

    void OnHiggsStashed(bool isLeft, RE::TESForm* stashedForm)
    {
        SendHeldItemEvent(isLeft ? "left" : "right", "drop", ResolveFormName(stashedForm));
    }

    void OnHiggsConsumed(bool isLeft, RE::TESForm* consumedForm)
    {
        SendHeldItemEvent(isLeft ? "left" : "right", "drop", ResolveFormName(consumedForm));
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
    }

    void Tick()
    {
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
