#include "ItemInteraction.h"
#include "ChimInteraction.h"
#include "Globals.h"
#include "HTTPManager.h"
#include "Misc.h"
#include "PlaythroughSession.h"
#include "PrismaUIBridge.h"
#include "SpeakManager.h"
#include "ThreadPool.h"
#include <atomic>
#include <map>
#include <mutex>
#include <random>
#include <sstream>

namespace ItemInteraction
{
namespace
{
using json = nlohmann::json;
struct Choice
{
    RE::FormID form;
    RE::ExtraDataList *extra;
    int count;
    std::string name;
    std::string fingerprint;
};
struct Request
{
    std::string id;
    std::uint64_t epoch;
    std::uint64_t dialogueGeneration = 0;
    std::uint64_t cancellationGeneration = 0;
    std::chrono::steady_clock::time_point reactionExpires{};
    RE::ObjectRefHandle target;
    std::vector<Choice> choices;
    Choice selected{};
    bool hasItem = false;
    bool pickupIssued = false;
    RE::FormID pickupForm = 0;
    int pickupInventoryBefore = 0;
    int pickupWorldBefore = 0;
    std::chrono::steady_clock::time_point pickupVerifyUntil{};
    int quantity = 1;
    int step = -1;
    std::atomic<int> allowedStep{-1};
    bool submitted = false;
    bool inventoryMoved = false;
    std::chrono::steady_clock::time_point deadline{};
    bool approved = false;
    json snapshot;
    json allowed;
    json plan;
    json receipts = json::array();
};
std::shared_ptr<Request> current;
std::map<std::string, std::shared_ptr<Request>> pendingReactions;
std::mutex currentMutex;
std::atomic<std::uint64_t> reactionCancellationGeneration{0};
constexpr RE::FormID powerLocalId = 0x05A0F1;

std::string NewRequestId()
{
    std::random_device random;
    std::ostringstream id;
    for (int i = 0; i < 4; ++i)
        id << std::format("{:08x}", random());
    return id.str();
}

// Keep rejected input visible so the player can correct it without losing their description.
void ShowInputError(const std::string &message)
{
    PrismaUIBridge::UpdateItemInteraction({{"state", "error"}, {"status", message}, {"confirm", false}});
}

std::shared_ptr<Request> Current()
{
    std::lock_guard lock(currentMutex);
    return current;
}
bool Live(const std::shared_ptr<Request> &r)
{
    return r && r == Current() && PlaythroughSession::Allowed(r->epoch) && ChimInteraction::Enabled();
}
void RunStep(const std::shared_ptr<Request> &r);

// Detect changes or allocator reuse of an extra-list pointer while the model is resolving.
std::string Fingerprint(RE::TESBoundObject *object, RE::ExtraDataList *extra)
{
    json state = {{"base", object->GetFormID()}};
    if (extra)
    {
        auto name = extra->GetDisplayName(object);
        state["name"] = name ? name : "";
        if (auto e = extra->GetByType<RE::ExtraEnchantment>())
            state["enchantment"] = e->enchantment ? e->enchantment->GetFormID() : 0;
        if (auto h = extra->GetByType<RE::ExtraHealth>())
            state["health"] = h->health;
        if (auto c = extra->GetByType<RE::ExtraCharge>())
            state["charge"] = c->charge;
        if (auto u = extra->GetByType<RE::ExtraUniqueID>())
            state["unique"] = {u->baseID, u->uniqueID};
        if (auto soul = extra->GetByType<RE::ExtraSoul>())
            state["soul"] = static_cast<int>(soul->GetContainedSoul());
        if (auto poison = extra->GetByType<RE::ExtraPoison>())
            state["poison"] = {poison->poison ? poison->poison->GetFormID() : 0, poison->count};
    }
    return state.dump();
}

// Re-find the selected extra list without dereferencing a pointer retained across engine changes.
bool InventoryChoice(RE::TESObjectREFR *owner, const Choice &choice, int needed, RE::TESBoundObject *&object,
                     RE::ExtraDataList *&extra)
{
    object = RE::TESForm::LookupByID<RE::TESBoundObject>(choice.form);
    extra = nullptr;
    if (!owner || !object)
        return false;
    auto inventory = owner->GetInventory();
    auto found = inventory.find(object);
    if (found == inventory.end() || found->second.first < needed || !found->second.second)
        return false;
    int special = 0;
    if (auto lists = found->second.second->extraLists)
        for (auto list : *lists)
        {
            if (!list)
                continue;
            special += list->GetCount();
            if (choice.extra && list == choice.extra && list->GetCount() >= needed &&
                Fingerprint(object, list) == choice.fingerprint)
            {
                extra = list;
                return true;
            }
        }
    return !choice.extra && found->second.first - special >= needed;
}

json Effects(RE::MagicItem *item)
{
    json result = json::array();
    if (item)
        for (auto effect : item->effects)
        {
            if (!effect || !effect->baseEffect)
                continue;
            result.push_back({{"name", effect->baseEffect->GetName()},
                              {"magnitude", effect->effectItem.magnitude},
                              {"duration", effect->effectItem.duration},
                              {"area", effect->effectItem.area}});
            if (result.size() == 12)
                break;
        }
    return result;
}

// Receipts are retriable reports; mutations are never retried by this request.
void Finish(const std::shared_ptr<Request> &r)
{
    if (!Live(r))
        return;
    r->allowedStep = -2;
    PrismaUIBridge::HideItemInteraction();
    json payload = {{"op", "receipt"}, {"id", r->id}, {"gamets", GetGameTimeStamp()}, {"receipts", r->receipts}};
    ThreadPool::getInstance().enqueue("InteractReceipt", [r, payload] {
        auto result = HTTPManager::postGameDataJson("item_interaction.php", payload, 120000);
        if (!result.value("ok", false) && Live(r))
            result = HTTPManager::postGameDataJson("item_interaction.php", payload, 120000);
        SKSE::GetTaskInterface()->AddTask([r, result] {
            if (!Live(r))
                return;
            if (result.value("ok", false) && result.contains("narration"))
            {
                auto n = result["narration"];
                ScriptLine line(n.value("text", ""), "", "", "", NARRATOR_NAME, "", 1.0f, -1, "explicit_disable_rechat",
                                n.value("utterance_id", ""));
                line.ttsCacheKey = n.value("tts_cache_key", "");
                if (r->snapshot["target"].value("actor", false))
                {
                    std::lock_guard lock(currentMutex);
                    if (pendingReactions.size() >= 8)
                        pendingReactions.erase(pendingReactions.begin());
                    r->reactionExpires = std::chrono::steady_clock::now() + std::chrono::seconds(120);
                    pendingReactions[line.utteranceId] = r;
                }
                SpeakManager::getInstance().insertInQueue(line);
            }
            else
                RE::DebugNotification(
                    "[CHIM] Effects finished; outcome reporting failed. Do not repeat uncertain effects.");
            std::lock_guard lock(currentMutex);
            if (current == r)
                current.reset();
        });
    });
}

void RunStep(const std::shared_ptr<Request> &r)
{
    if (!Live(r))
        return;
    ++r->step;
    r->allowedStep = r->step;
    r->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    if (r->step >= static_cast<int>(r->plan["steps"].size()))
    {
        Finish(r);
        return;
    }
    auto target = r->target.get();
    const auto &step = r->plan["steps"][r->step];
    const auto effect = step.at("effect").get<std::string>();
    auto actor = target ? target->As<RE::Actor>() : nullptr;
    bool blocked = r->pickupIssued || !target || target->IsDisabled() || !target->Is3DLoaded() || target->IsDeleted();
    RE::TESBoundObject *held = nullptr;
    RE::ExtraDataList *heldExtra = nullptr;
    if (r->hasItem && !r->inventoryMoved &&
        !InventoryChoice(RE::PlayerCharacter::GetSingleton(), r->selected, 1, held, heldExtra))
        blocked = true;
    for (const auto &dependency : step.at("requires"))
    {
        if (r->receipts.at(dependency.get<int>()).at("status") != "succeeded")
            blocked = true;
    }
    if (step.value("alive", false) && (!actor || actor->IsDead()))
        blocked = true;
    if (blocked)
    {
        Complete(r->id, r->step, "skipped", "Target unavailable or prerequisite did not succeed.");
        return;
    }
    const float value = step.at("value").get<float>();
    if (effect == "observe")
    {
        Complete(r->id, r->step, "succeeded", "No physical change.");
        return;
    }
    if (effect == "pickup")
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        auto food = target->GetBaseObject()->As<RE::AlchemyItem>();
        if (!player || !food || !food->IsFood())
        {
            Complete(r->id, r->step, "failed", "The target is no longer eligible loose food.");
            return;
        }
        r->pickupInventoryBefore = player->GetInventoryCounts()[food];
        r->pickupWorldBefore = target->extraList.GetCount();
        r->pickupForm = food->GetFormID();
        r->pickupVerifyUntil = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        // Use the engine pickup path on the exact reference, retaining engine ownership/crime handling.
        r->pickupIssued = true;
        SKSE::log::info("[INTERACT] Pickup {} ref={:08X} base={:08X} inventoryBefore={} worldBefore={} offLimits={}",
                        r->id, target->GetFormID(), food->GetFormID(), r->pickupInventoryBefore, r->pickupWorldBefore,
                        target->IsOffLimits());
        player->PickUpObject(target.get(), 1);
        // Tick observes completion without ever repeating the mutation.
        return;
    }
    if (effect == "give" || effect == "store" || effect == "consume" || effect == "equip" || effect == "magic")
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        RE::TESBoundObject *object = nullptr;
        RE::ExtraDataList *extra = nullptr;
        const int count = (effect == "give" || effect == "store") ? static_cast<int>(value) : 1;
        if (!r->hasItem || count > r->quantity || !InventoryChoice(player, r->selected, count, object, extra))
        {
            Complete(r->id, r->step, "failed", "The exact selected inventory instance is no longer available.");
            return;
        }
        if (effect == "magic")
        {
            auto scroll = object->As<RE::ScrollItem>();
            if (!scroll || !actor)
            {
                Complete(r->id, r->step, "failed", "Selected spell is unavailable.");
                return;
            }
            if (player->GetInventory().at(object).second->IsQuestObject())
            {
                Complete(r->id, r->step, "failed", "Quest-bound scrolls cannot be consumed.");
                return;
            }
            const auto before = player->GetInventoryCounts()[object];
            player->RemoveItem(object, 1, RE::ITEM_REMOVE_REASON::kRemove, extra, nullptr);
            if (player->GetInventoryCounts()[object] != before - 1)
            {
                Complete(r->id, r->step, "unknown", "Scroll removal could not be confirmed; no spell was cast.");
                return;
            }
            r->inventoryMoved = true;
            auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            std::string id = r->id, operation = effect;
            int index = r->step;
            auto ref = target.get();
            bool approved = false;
            float amount = 0;
            RE::ScrollItem *spell = scroll;
            if (!vm ||
                !vm->DispatchStaticCall("CHIMItemInteraction", "Execute",
                                        RE::MakeFunctionArguments(std::move(id), std::move(index), std::move(ref),
                                                                  std::move(operation), std::move(amount),
                                                                  std::move(approved), std::move(spell)),
                                        callback))
                Complete(r->id, r->step, "failed", "Scroll consumed but the execution script was unavailable.");
            return;
        }
        if ((effect != "store" && !actor) ||
            (effect == "store" && target->GetBaseObject()->GetFormType() != RE::FormType::Container))
        {
            Complete(r->id, r->step, "failed", "Inventory destination is no longer eligible.");
            return;
        }
        if (player->GetInventory().at(object).second->IsQuestObject())
        {
            Complete(r->id, r->step, "failed", "The selected item became quest-bound; transfer was not attempted.");
            return;
        }
        const auto beforePlayer = player->GetInventoryCounts()[object];
        const auto beforeTarget = target->GetInventoryCounts()[object];
        player->RemoveItem(object, count, RE::ITEM_REMOVE_REASON::kStoreInContainer, extra, target.get());
        const auto afterPlayer = player->GetInventoryCounts()[object];
        const auto afterTarget = target->GetInventoryCounts()[object];
        if (beforePlayer - afterPlayer != count || afterTarget - beforeTarget != count)
        {
            Complete(r->id, r->step, "unknown", "Transfer was issued but both inventory counts did not confirm it.");
            return;
        }
        r->inventoryMoved = true;
        if (effect == "give" || effect == "store")
        {
            Complete(r->id, r->step, "succeeded", "Exact selected inventory was transferred.");
            return;
        }
        // Extra lists can merge on transfer. Refuse an ambiguous target instance instead of using another copy.
        RE::ExtraDataList *transferred = nullptr;
        auto targetInventory = target->GetInventory();
        auto entry = targetInventory.find(object);
        int lists = 0;
        if (entry != targetInventory.end() && entry->second.second && entry->second.second->extraLists)
        {
            for (auto list : *entry->second.second->extraLists)
            {
                if (list)
                {
                    transferred = list;
                    ++lists;
                }
            }
        }
        if (r->selected.extra && (beforeTarget != 0 || lists != 1))
        {
            Complete(r->id, r->step, "unknown",
                     "Item transferred, but the destination instance is ambiguous; no equip or consumption occurred.");
            return;
        }
        if (!r->selected.extra)
            transferred = nullptr;
        if (effect == "consume")
        {
            auto potion = object->As<RE::AlchemyItem>();
            const auto beforeConsumption = actor->GetInventoryCounts()[object];
            bool accepted = potion && actor->DrinkPotion(potion, transferred);
            const auto afterConsumption = actor->GetInventoryCounts()[object];
            bool consumed = accepted && afterConsumption == beforeConsumption - 1;
            Complete(r->id, r->step, consumed ? "succeeded" : "unknown",
                     consumed ? "Item transferred and consumption confirmed by inventory decrease."
                              : "Item transferred but consumption could not be confirmed.");
        }
        else
        {
            if (!transferred && beforeTarget != 0)
            {
                Complete(r->id, r->step, "unknown",
                         "Item transferred, but existing copies make exact equipment verification ambiguous.");
                return;
            }
            const auto expectedFingerprint = transferred ? Fingerprint(object, transferred) : std::string{};
            RE::ActorEquipManager::GetSingleton()->EquipObject(actor, object, transferred, 1, nullptr, false, false,
                                                               true, true);
            // Re-read extras: a matching base form already equipped is not proof this copy was equipped.
            bool equipped = false;
            auto afterInventory = actor->GetInventory();
            auto afterEntry = afterInventory.find(object);
            if (afterEntry != afterInventory.end() && afterEntry->second.second &&
                afterEntry->second.second->extraLists)
            {
                for (auto list : *afterEntry->second.second->extraLists)
                {
                    if (!list ||
                        (!list->HasType(RE::ExtraDataType::kWorn) && !list->HasType(RE::ExtraDataType::kWornLeft)))
                        continue;
                    if (transferred)
                        equipped = list == transferred && Fingerprint(object, list) == expectedFingerprint;
                    else
                        equipped = beforeTarget == 0;
                    if (equipped)
                        break;
                }
            }
            Complete(r->id, r->step, equipped ? "succeeded" : "unknown",
                     equipped ? "Item transferred and equipped."
                              : "Item transferred; equipment state could not be confirmed.");
        }
        return;
    }
    if (effect == "consume_world")
    {
        auto consumable = target->GetBaseObject()->As<RE::AlchemyItem>();
        if (r->hasItem || !consumable || consumable->IsPoison() || target->extraList.GetCount() != 1 ||
            target->extraList.HasType(RE::ExtraDataType::kEnchantment) ||
            target->extraList.HasType(RE::ExtraDataType::kPoison))
        {
            Complete(r->id, r->step, "failed", "The world target is no longer one eligible consumable.");
            return;
        }
        r->pickupIssued = true; // Later steps must not act on the reference moved into inventory.
    }
    auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
    std::string id = r->id, operation = effect;
    int index = r->step;
    auto ref = target.get();
    bool approved = r->approved;
    float amount = value;
    if (!vm ||
        !vm->DispatchStaticCall("CHIMItemInteraction", "Execute",
                                RE::MakeFunctionArguments(std::move(id), std::move(index), std::move(ref),
                                                          std::move(operation), std::move(amount), std::move(approved),
                                                          static_cast<RE::ScrollItem *>(nullptr)),
                                callback))
    {
        Complete(r->id, r->step, "failed", "The interaction script is unavailable.");
    }
}
} // namespace

bool CanExecute(const std::string &id, int step)
{
    auto r = Current();
    return Live(r) && r->id == id && r->allowedStep.load() == step;
}
void Complete(const std::string &id, int step, const std::string &status, const std::string &detail)
{
    SKSE::GetTaskInterface()->AddTask([id, step, status, detail] {
        auto r = Current();
        if (!CanExecute(id, step) || r->receipts.size() != static_cast<std::size_t>(step))
            return;
        // Papyrus strings can retain interned casing; canonicalize only the bounded wire enum.
        std::string outcome = status;
        for (auto &character : outcome)
            if (character >= 'A' && character <= 'Z')
                character += 'a' - 'A';
        bool valid = outcome == "succeeded" || outcome == "failed" || outcome == "skipped" || outcome == "unknown";
        if (!valid)
            outcome = "unknown";
        std::string diagnostic = status.substr(0, 24);
        for (auto &character : diagnostic)
            if (character < ' ' || character > '~')
                character = '?';
        SKSE::log::info("[INTERACT] Receipt {} step {} raw={} status={} valid={}", id, step, diagnostic, outcome,
                        valid);
        r->receipts.push_back(
            {{"status", outcome},
             {"detail", valid ? detail : "Execution returned an unrecognized outcome; result is uncertain."}});
        // Only accepted receipts reach this point; duplicate or stale callbacks never notify.
        const auto &completedStep = r->plan["steps"][step];
        const auto effect = completedStep.at("effect").get<std::string>();
        static const std::map<std::string, std::string> labels = {
            {"observe", "Examine"}, {"pickup", "Pick up"}, {"give", "Give"}, {"store", "Store"},
            {"consume", "Administer"}, {"equip", "Equip"}, {"injure", "Injure"}, {"kill", "Kill"},
            {"push", "Push"}, {"lock", "Lock"}, {"unlock", "Unlock"}, {"activate", "Activate"},
            {"open", "Open"}, {"close", "Close"}, {"destroy", "Damage"}, {"disable", "Disable"},
            {"resize", "Resize"}, {"magic", "Cast"}, {"combat", "Start combat with"},
            {"consume_world", "Consume"}};
        const auto label = labels.find(effect);
        std::string action = label != labels.end() ? label->second : "Interaction with";
        const auto targetName = r->snapshot["target"].value("name", "target");
        if (effect == "give" || effect == "store")
            action += std::format(" {} {} {} {}", completedStep.at("value").get<int>(), r->selected.name,
                                  effect == "store" ? "in" : "to", targetName);
        else if (effect == "consume" || effect == "equip" || effect == "magic")
            action += std::format(" {} on {}", r->selected.name, targetName);
        else
            action += " " + targetName;
        std::string result = outcome;
        if (outcome == "unknown")
            result = "could not be confirmed";
        else if (outcome == "skipped")
            result = "not completed";
        RE::DebugNotification(std::format("[CHIM] {}: {}.", action, result).c_str());
        RunStep(r);
    });
}
// Playback completion, not queue insertion or the generic speech log, releases one exact NPC reaction.
void NarrationComplete(const std::string &utteranceId, bool completed)
{
    if (!utteranceId.starts_with("interact-"))
        return;
    std::shared_ptr<Request> r;
    {
        std::lock_guard lock(currentMutex);
        auto found = pendingReactions.find(utteranceId);
        if (found == pendingReactions.end())
            return;
        r = found->second;
        pendingReactions.erase(found);
    }
    if (!completed)
        return;
    SKSE::GetTaskInterface()->AddTask([r] {
        if (!PlaythroughSession::Allowed(r->epoch) || !ChimInteraction::Enabled() ||
            r->cancellationGeneration != reactionCancellationGeneration.load() ||
            r->dialogueGeneration != PrismaUIBridge::GetDialogueStopGeneration())
            return;
        auto ref = r->target.get();
        auto actor = ref ? ref->As<RE::Actor>() : nullptr;
        if (!actor || actor->IsDead() || actor->IsDisabled() || actor->IsDeleted() || !actor->Is3DLoaded())
            return;
        const std::string expectedSpeaker = r->snapshot["target"].value("speaker", "");
        bool exactAgent = false;
        for (const auto &agent : AIAgentManager::getInstance().getAgents())
            if (agent && !agent->isNarrator() && agent->getActor() == actor && agent->getActorName() == expectedSpeaker)
                exactAgent = true;
        if (!exactAgent)
        {
            RE::DebugNotification("[CHIM] NPC response needs this exact actor active in CHIM.");
            return;
        }
        json context = {{"id", r->id}, {"target_ref", std::format("{:08X}", actor->GetFormID())}};
        HTTPManager::streamForActor(std::format("chatnf_interact_reaction|{}|{}|{}", getCurrentTimeMillis(),
                                                GetGameTimeStamp(), context.dump()),
                                    actor, PlayerConversationRoutingPolicy::RequestEligibility::ExplicitTarget);
    });
}

void Tick()
{
    static std::atomic<bool> queued = false;
    if (queued.exchange(true))
        return;
    SKSE::GetTaskInterface()->AddTask([] {
        queued = false;
        {
            std::lock_guard lock(currentMutex);
            for (auto it = pendingReactions.begin(); it != pendingReactions.end();)
            {
                const auto &pending = it->second;
                if (!PlaythroughSession::Allowed(pending->epoch) || !ChimInteraction::Enabled() ||
                    pending->dialogueGeneration != PrismaUIBridge::GetDialogueStopGeneration() ||
                    pending->cancellationGeneration != reactionCancellationGeneration.load() ||
                    std::chrono::steady_clock::now() >= pending->reactionExpires)
                    it = pendingReactions.erase(it);
                else
                    ++it;
            }
        }
        auto r = Current();
        if (!r)
            return;
        if (!Live(r))
        {
            Cancel();
            return;
        }
        if (r->pickupForm && CanExecute(r->id, r->step))
        {
            auto player = RE::PlayerCharacter::GetSingleton();
            auto food = RE::TESForm::LookupByID<RE::AlchemyItem>(r->pickupForm);
            auto target = r->target.get();
            const bool moved = !target || target->IsDeleted() || target->IsDisabled() || !target->Is3DLoaded() ||
                               target->extraList.GetCount() < r->pickupWorldBefore;
            const bool acquired = player && food && player->GetInventoryCounts()[food] == r->pickupInventoryBefore + 1;
            if ((acquired && moved) || std::chrono::steady_clock::now() >= r->pickupVerifyUntil)
            {
                SKSE::log::info("[INTERACT] Pickup result {} inventoryBefore={} inventoryAfter={} worldBefore={} "
                                "worldAfter={} refPresent={} deleted={} disabled={} loaded={} acquired={} moved={}",
                                r->id, r->pickupInventoryBefore,
                                player && food ? player->GetInventoryCounts()[food] : -1, r->pickupWorldBefore,
                                target ? target->extraList.GetCount() : -1, bool(target), target && target->IsDeleted(),
                                target && target->IsDisabled(), target && target->Is3DLoaded(), acquired, moved);
                r->pickupForm = 0;
                Complete(r->id, r->step, acquired && moved ? "succeeded" : "unknown",
                         acquired && moved
                             ? "One target food item entered the player's inventory."
                             : "Pickup was requested once; inventory and world-reference changes did not confirm it.");
            }
            return;
        }
        if (r->step >= 0 && r->step < static_cast<int>(r->plan["steps"].size()) &&
            std::chrono::steady_clock::now() > r->deadline)
        {
            r->receipts.push_back({{"status", "unknown"},
                                   {"detail", "Execution did not report within 15 seconds; it will not be repeated."}});
            while (r->receipts.size() < r->plan["steps"].size())
                r->receipts.push_back({{"status", "skipped"}, {"detail", "An earlier operation timed out."}});
            RE::DebugNotification("[CHIM] Interaction result could not be confirmed.");
            r->step = static_cast<int>(r->plan["steps"].size());
            Finish(r);
        }
    });
}
void Cancel()
{
    ++reactionCancellationGeneration;
    auto old = Current();
    if (old && old->submitted && old->step < 0 && PlaythroughSession::Allowed(old->epoch))
    {
        json payload = {{"op", "cancel"}, {"id", old->id}};
        ThreadPool::getInstance().enqueue(
            "InteractCancel", [payload] { HTTPManager::postGameDataJson("item_interaction.php", payload, 5000); });
    }
    {
        std::lock_guard lock(currentMutex);
        current.reset();
        pendingReactions.clear();
    }
    PrismaUIBridge::HideItemInteraction();
}
void Open()
{
    if (Current())
    {
        RE::DebugNotification("[CHIM] An interaction is already active.");
        return;
    }
    auto player = RE::PlayerCharacter::GetSingleton();
    auto crosshair = RE::CrosshairPickData::GetSingleton();
    auto target = crosshair ? crosshair->GetActiveTarget().get() : RE::NiPointer<RE::TESObjectREFR>{};
    if (!player || !target || !ChimInteraction::Enabled() ||
        !PlaythroughSession::Allowed(PlaythroughSession::Generation()) || !target->Is3DLoaded() ||
        target.get() == player || RE::UI::GetSingleton()->GameIsPaused() || PrismaUIBridge::IsAnyHotkeyPanelFocused() ||
        RE::UI::GetSingleton()->IsMenuOpen(RE::DialogueMenu::MENU_NAME) ||
        RE::UI::GetSingleton()->IsMenuOpen(RE::Console::MENU_NAME) ||
        RE::UI::GetSingleton()->IsMenuOpen(RE::LoadingMenu::MENU_NAME))
    {
        RE::DebugNotification("[CHIM] Aim at a loaded target before using Interact.");
        return;
    }
    auto r = std::make_shared<Request>();
    r->epoch = PlaythroughSession::Generation();
    r->dialogueGeneration = PrismaUIBridge::GetDialogueStopGeneration();
    r->cancellationGeneration = reactionCancellationGeneration.load();
    r->target = target->CreateRefHandle();
    r->id = NewRequestId();
    json items = json::array();
    for (auto &[object, data] : player->GetInventory())
    {
        if (!object || data.first <= 0 || !data.second || !object->GetName() || !*object->GetName())
            continue;
        int special = 0;
        if (data.second->extraLists)
            for (auto extra : *data.second->extraLists)
            {
                if (!extra)
                    continue;
                int count = extra->GetCount();
                special += count;
                auto name = extra->GetDisplayName(object);
                r->choices.push_back({object->GetFormID(), extra, count, name && *name ? name : object->GetName(),
                                      Fingerprint(object, extra)});
                std::string details;
                if (auto enchant = extra->GetByType<RE::ExtraEnchantment>(); enchant && enchant->enchantment)
                    details += std::string("Enchantment: ") + enchant->enchantment->GetName() + ". ";
                if (auto health = extra->GetByType<RE::ExtraHealth>())
                    details += std::format("Tempering: {:.2f}. ", health->health);
                if (auto charge = extra->GetByType<RE::ExtraCharge>())
                    details += std::format("Charge: {:.0f}. ", charge->charge);
                items.push_back({{"key", r->choices.size() - 1},
                                 {"name", r->choices.back().name},
                                 {"count", count},
                                 {"details", details + "Copy " + std::to_string(r->choices.size())}});
            }
        if (data.first > special)
        {
            r->choices.push_back(
                {object->GetFormID(), nullptr, data.first - special, object->GetName(), Fingerprint(object, nullptr)});
            items.push_back(
                {{"key", r->choices.size() - 1}, {"name", object->GetName()}, {"count", data.first - special}});
        }
    }
    {
        std::lock_guard lock(currentMutex);
        current = r;
    }
    PrismaUIBridge::ShowItemInteraction({{"id", r->id}, {"target", target->GetName()}, {"items", items}});
}

void Command(const std::string &command)
{
    SKSE::log::info("[INTERACT] UI command queued for game thread");
    SKSE::GetTaskInterface()->AddTask([command] {
        try
        {
            auto r = Current();
            if (!Live(r))
            {
                Cancel();
                return;
            }
            const auto input = json::parse(command);
            if (input.value("id", "") != r->id)
            {
                ShowInputError("This interaction is no longer current. Close this menu and cast Interact again.");
                return;
            }
            const auto op = input.value("op", "");
            std::string commandType = "unknown";
            if (op == "submit" || op == "approve" || op == "cancel")
                commandType = op;
            SKSE::log::info("[INTERACT] Game thread received {} command for {}", commandType, r->id);
            if (op == "cancel")
            {
                Cancel();
                return;
            }
            if (op == "approve" && r->submitted && r->step == -1 && !r->plan.is_null())
            {
                r->approved = true;
                PrismaUIBridge::HideItemInteraction();
                RunStep(r);
                return;
            }
            if (op != "submit" || r->submitted)
                return;
            r->hasItem = !input.at("key").is_null();
            const auto key = r->hasItem ? input.at("key").get<std::size_t>() : 0;
            if (r->hasItem && key >= r->choices.size())
            {
                ShowInputError("Choose an inventory item before interacting.");
                return;
            }
            r->selected = r->hasItem ? r->choices[key] : Choice{};
            r->quantity = r->hasItem ? input.at("quantity").get<int>() : 0;
            const auto intent = input.at("intent").get<std::string>();
            if ((r->hasItem && (r->quantity < 1 || r->quantity > 100 || r->quantity > r->selected.count)) ||
                intent.empty() || intent.size() > 4000)
            {
                ShowInputError("Enter a description and a whole quantity within the available amount.");
                return;
            }
            auto target = r->target.get();
            auto player = RE::PlayerCharacter::GetSingleton();
            RE::TESBoundObject *item = nullptr;
            RE::ExtraDataList *extra = nullptr;
            if (!player || !target || (r->hasItem && !InventoryChoice(player, r->selected, r->quantity, item, extra)))
            {
                ShowInputError(
                    "The target or selected item has changed. Choose another item, or close and reopen Interact.");
                return;
            }
            auto actor = target->As<RE::Actor>();
            // Actor's inherited subobjects move between Skyrim runtimes; use the runtime-aware accessors.
            auto playerStats = player->AsActorValueOwner();
            auto targetStats = actor ? actor->AsActorValueOwner() : nullptr;
            if (!playerStats || (actor && !targetStats))
            {
                ShowInputError("Player or target statistics are unavailable. Close and reopen Interact.");
                return;
            }
            auto base = target->GetBaseObject();
            auto inventory = player->GetInventory();
            bool questItem = item && inventory.at(item).second->IsQuestObject();
            r->allowed = json::array({"observe", "activate", "resize", "disable"});
            auto targetFood = base->As<RE::AlchemyItem>();
            if (targetFood && targetFood->IsFood())
                r->allowed.push_back("pickup");
            if (!item && targetFood && !targetFood->IsPoison() && target->extraList.GetCount() == 1 &&
                !target->extraList.HasType(RE::ExtraDataType::kEnchantment) &&
                !target->extraList.HasType(RE::ExtraDataType::kPoison))
                r->allowed.push_back("consume_world");
            if (actor)
            {
                for (auto effect : {"injure", "kill", "push", "combat"})
                    r->allowed.push_back(effect);
            }
            if (actor && item)
                r->allowed.push_back("give");
            if (item && base->GetFormType() == RE::FormType::Container)
                r->allowed.push_back("store");
            if (target->GetLock())
                for (auto effect : {"lock", "unlock"})
                    r->allowed.push_back(effect);
            if (base->GetFormType() == RE::FormType::Door || base->GetFormType() == RE::FormType::Container)
                for (auto effect : {"open", "close"})
                    r->allowed.push_back(effect);
            if (base->As<RE::BGSDestructibleObjectForm>() && base->As<RE::BGSDestructibleObjectForm>()->data)
                r->allowed.push_back("destroy");
            if (!actor && base->GetFormType() == RE::FormType::Misc)
                r->allowed.push_back("push");
            auto potion = item ? item->As<RE::AlchemyItem>() : nullptr;
            if (actor && potion && !potion->IsPoison())
                r->allowed.push_back("consume");
            if (actor && item && (item->As<RE::TESObjectWEAP>() || item->As<RE::TESObjectARMO>()))
                r->allowed.push_back("equip");
            // Scrolls carry authored spells; restrict to the small known vanilla elemental/paralysis family.
            auto scroll = item ? item->As<RE::ScrollItem>() : nullptr;
            bool supportedMagic = actor && scroll &&
                                  (scroll->GetFormID() == 0x00096598 || scroll->GetFormID() == 0x00096599 ||
                                   scroll->GetFormID() == 0x0009659A);
            if (supportedMagic)
            {
                for (auto effect : scroll->effects)
                {
                    if (!effect || !effect->baseEffect || effect->effectItem.area != 0 ||
                        (effect->baseEffect->GetArchetype() != RE::EffectArchetypes::ArchetypeID::kValueModifier &&
                         effect->baseEffect->GetArchetype() != RE::EffectArchetypes::ArchetypeID::kDualValueModifier))
                        supportedMagic = false;
                }
            }
            if (supportedMagic)
                r->allowed.push_back("magic");
            if (questItem)
            {
                json filtered = json::array();
                for (const auto &effect : r->allowed)
                {
                    if (effect != "give" && effect != "store" && effect != "consume" && effect != "equip" &&
                        effect != "magic")
                        filtered.push_back(effect);
                }
                r->allowed = filtered;
            }
            json itemData = nullptr;
            if (item)
            {
                itemData = {{"name", r->selected.name},
                            {"type", static_cast<int>(item->GetFormType())},
                            {"quantity", r->quantity},
                            {"quest_item", questItem},
                            {"effects", Effects(potion ? static_cast<RE::MagicItem *>(potion)
                                                       : static_cast<RE::MagicItem *>(scroll))}};
                if (auto weapon = item->As<RE::TESObjectWEAP>())
                    itemData["base_damage"] = weapon->GetAttackDamage();
                RE::EnchantmentItem *enchantment = nullptr;
                if (auto enchanting = item->As<RE::TESEnchantableForm>())
                    enchantment = enchanting->formEnchanting;
                if (extra)
                {
                    if (auto e = extra->GetByType<RE::ExtraEnchantment>())
                        enchantment = e->enchantment;
                    if (auto health = extra->GetByType<RE::ExtraHealth>())
                        itemData["tempering_factor"] = health->health;
                    if (auto charge = extra->GetByType<RE::ExtraCharge>())
                        itemData["charge"] = charge->charge;
                    itemData["equipped"] =
                        extra->HasType(RE::ExtraDataType::kWorn) || extra->HasType(RE::ExtraDataType::kWornLeft);
                }
                else
                    itemData["equipped"] = false;
                itemData["enchantment_effects"] = Effects(enchantment);
            }
            json targetData = {{"name", target->GetName()},
                               {"actor", actor != nullptr},
                               {"scale", target->GetScale()},
                               {"awareness", "unknown"}};
            targetData["ref_id"] = std::format("{:08X}", target->GetFormID());
            targetData["speaker"] = "";
            if (targetFood)
                targetData["consumable_effects"] = Effects(targetFood);
            if (actor)
            {
                for (const auto &agent : AIAgentManager::getInstance().getAgents())
                    if (agent && !agent->isNarrator() && agent->getActor() == actor)
                    {
                        targetData["speaker"] = agent->getActorName();
                        break;
                    }
                targetData["health"] = targetStats->GetActorValue(RE::ActorValue::kHealth);
                targetData["dead"] = actor->IsDead();
                targetData["combat"] = actor->IsInCombat();
                targetData["essential"] = actor->IsEssential();
                targetData["max_health"] = targetStats->GetPermanentActorValue(RE::ActorValue::kHealth);
                targetData["race"] = actor->GetRace() ? actor->GetRace()->GetName() : "unknown";
                targetData["armor_rating"] = targetStats->GetActorValue(RE::ActorValue::kDamageResist);
                targetData["magic_resistance"] = targetStats->GetActorValue(RE::ActorValue::kResistMagic);
                targetData["fire_resistance"] = targetStats->GetActorValue(RE::ActorValue::kResistFire);
                targetData["frost_resistance"] = targetStats->GetActorValue(RE::ActorValue::kResistFrost);
                targetData["shock_resistance"] = targetStats->GetActorValue(RE::ActorValue::kResistShock);
                targetData["protected"] = actor->GetActorBase()->IsProtected();
                targetData["unique"] = actor->GetActorBase()->IsUnique();
                auto weapon = actor->GetEquippedObject(false);
                targetData["weapon"] = weapon ? weapon->GetName() : "none";
            }
            targetData["type"] = static_cast<int>(base->GetFormType());
            targetData["lock_level"] = static_cast<int>(target->GetLockLevel());
            auto owner = target->GetOwner();
            targetData["owner"] = owner ? owner->GetName() : "none";
            targetData["open_state"] = "unknown";
            targetData["quest_associations"] = "unknown";
            targetData["authored_destruction"] =
                std::find(r->allowed.begin(), r->allowed.end(), "destroy") != r->allowed.end();
            r->snapshot = {
                {"item", itemData},
                {"target", targetData},
                {"player",
                 {{"health", playerStats->GetActorValue(RE::ActorValue::kHealth)},
                  {"stamina", playerStats->GetActorValue(RE::ActorValue::kStamina)},
                  {"magicka", playerStats->GetActorValue(RE::ActorValue::kMagicka)},
                  {"combat", player->IsInCombat()},
                  {"sneaking", player->IsSneaking()}}},
                {"location", player->GetCurrentLocation() ? player->GetCurrentLocation()->GetName() : "unknown"}};
            r->submitted = true;
            json payload = {{"op", "resolve"},         {"id", r->id},
                            {"intent", intent},        {"gamets", GetGameTimeStamp()},
                            {"snapshot", r->snapshot}, {"capabilities", r->allowed}};
            PrismaUIBridge::UpdateItemInteraction(
                {{"state", "busy"}, {"status", "Resolving interaction..."}, {"confirm", false}});
            ThreadPool::getInstance().enqueue("InteractResolve", [r, payload] {
                SKSE::log::info("[INTERACT] Resolution request started for {}", payload.at("id").get<std::string>());
                json result = json::object();
                try
                {
                    result = HTTPManager::postGameDataJson("item_interaction.php", payload, 120000);
                }
                catch (const std::exception &)
                {
                    SKSE::log::warn("[INTERACT] Resolution transport failed");
                }
                SKSE::GetTaskInterface()->AddTask([r, result] {
                    if (!Live(r))
                        return;
                    try
                    {
                        if (!result.value("ok", false) || result.value("id", "") != r->id)
                            throw std::runtime_error("Resolution failed");
                        r->plan = result.at("plan");
                        if (!r->plan.at("steps").is_array() || r->plan["steps"].size() > 5)
                            throw std::runtime_error("Invalid sequence");
                        bool confirm = false;
                        int inventorySteps = 0;
                        int pickupSteps = 0;
                        for (std::size_t index = 0; index < r->plan["steps"].size(); ++index)
                        {
                            const auto &s = r->plan["steps"][index];
                            const auto effect = s.at("effect").get<std::string>();
                            if (std::find(r->allowed.begin(), r->allowed.end(), effect) == r->allowed.end())
                                throw std::runtime_error("Unsupported effect");
                            const float value = s.at("value").get<float>();
                            static const std::map<std::string, std::pair<float, float>> limits = {
                                {"consume_world", {0, 0}}, {"pickup", {0, 0}},   {"observe", {0, 0}},
                                {"give", {1, 100}},        {"store", {1, 100}},  {"consume", {1, 1}},
                                {"equip", {1, 1}},         {"injure", {1, 100}}, {"kill", {0, 0}},
                                {"push", {1, 10}},         {"lock", {0, 100}},   {"unlock", {0, 0}},
                                {"activate", {0, 0}},      {"open", {0, 0}},     {"close", {0, 0}},
                                {"destroy", {1, 100}},     {"disable", {0, 0}},  {"resize", {0.25f, 2}},
                                {"magic", {0, 0}},         {"combat", {0, 0}}};
                            if ((effect == "pickup" || effect == "consume_world") && ++pickupSteps > 1)
                                throw std::runtime_error("Repeated pickup");
                            const auto bounds = limits.at(effect);
                            if (!std::isfinite(value) || value < bounds.first || value > bounds.second ||
                                !s.at("alive").is_boolean())
                                throw std::runtime_error("Invalid value");
                            if ((effect == "give" || effect == "store" || effect == "consume" || effect == "equip" ||
                                 effect == "lock") &&
                                std::floor(value) != value)
                                throw std::runtime_error("Fractional quantity");
                            if (effect == "combat" && !s.at("alive").get<bool>())
                                throw std::runtime_error("Combat requires living target");
                            for (auto dep : s.at("requires"))
                                if (!dep.is_number_integer() || dep.get<int>() < 0 || dep.get<std::size_t>() >= index)
                                    throw std::runtime_error("Invalid dependency");
                            if (effect == "kill" || effect == "disable")
                                confirm = true;
                            if (effect == "give" || effect == "store" || effect == "consume" || effect == "equip" ||
                                effect == "magic")
                                ++inventorySteps;
                        }
                        if (inventorySteps > 1)
                            throw std::runtime_error("Conflicting inventory steps");
                        if (confirm)
                            PrismaUIBridge::UpdateItemInteraction(
                                {{"confirm", true},
                                 {"status", "This interaction can kill the selected target or disable it, including "
                                            "protected or quest-sensitive targets. Continue?"}});
                        else
                        {
                            PrismaUIBridge::HideItemInteraction();
                            RunStep(r);
                        }
                    }
                    catch (const std::exception &)
                    {
                        SKSE::log::warn("[INTERACT] Resolution failed before execution for {}", r->id);
                        const json cancel = {{"op", "cancel"}, {"id", r->id}};
                        ThreadPool::getInstance().enqueue("InteractCancel", [cancel] {
                            HTTPManager::postGameDataJson("item_interaction.php", cancel, 5000);
                        });
                        // No mechanical step has started. A deliberate retry gets a fresh server claim.
                        r->id = NewRequestId();
                        r->submitted = false;
                        r->plan = nullptr;
                        PrismaUIBridge::UpdateItemInteraction(
                            {{"id", r->id},
                             {"preserveDraft", true},
                             {"state", "error"},
                             {"confirm", false},
                             {"status", "CHIM could not resolve this interaction. No effects were played. Your "
                                        "description is kept; you can try again."}});
                    }
                });
            });
        }
        catch (const std::exception &)
        {
            SKSE::log::warn("[INTERACT] UI command was rejected before execution");
            ShowInputError("CHIM could not read this interaction. Your description has been kept.");
        }
    });
}

class CastSink : public RE::BSTEventSink<RE::TESSpellCastEvent>
{
    RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent *event,
                                          RE::BSTEventSource<RE::TESSpellCastEvent> *) override
    {
        auto power = RE::TESDataHandler::GetSingleton()->LookupForm<RE::SpellItem>(powerLocalId, "AIAgent.esp");
        if (event && power && event->spell == power->GetFormID() &&
            event->object.get() == RE::PlayerCharacter::GetSingleton())
            SKSE::GetTaskInterface()->AddTask([] { Open(); });
        return RE::BSEventNotifyControl::kContinue;
    }
};
void Initialize()
{
    static CastSink sink;
    RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(&sink);
}
void GrantPower()
{
    auto power = RE::TESDataHandler::GetSingleton()->LookupForm<RE::SpellItem>(powerLocalId, "AIAgent.esp");
    if (power && RE::PlayerCharacter::GetSingleton())
        RE::PlayerCharacter::GetSingleton()->AddSpell(power);
}
void Register(RE::BSScript::IVirtualMachine *vm)
{
    vm->RegisterFunction(
        "CanExecute", "CHIMItemInteraction",
        +[](RE::StaticFunctionTag *, std::string id, int step) { return CanExecute(id, step); }, false);
    vm->RegisterFunction(
        "Complete", "CHIMItemInteraction",
        +[](RE::StaticFunctionTag *, std::string id, int step, std::string status, std::string detail) {
            Complete(id, step, status, detail);
        },
        false);
}
} // namespace ItemInteraction
