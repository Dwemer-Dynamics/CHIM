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

#ifdef GetObject
#undef GetObject
#endif

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
    std::map<int, Choice> equipment;
    bool hasItem = false;
    bool pickupIssued = false;
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
    bool reactionReleased = false;
    std::vector<ScriptLine> reactionLines;
    std::size_t reactionBytes = 0;
};
std::shared_ptr<Request> current;
std::map<std::string, std::shared_ptr<Request>> pendingReactions;
std::mutex currentMutex;
thread_local bool releasingReaction = false;
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
RE::ActorValue RestorationValue(const std::string &effect)
{
    if (effect == "heal") return RE::ActorValue::kHealth;
    if (effect == "restore_stamina") return RE::ActorValue::kStamina;
    if (effect == "restore_magicka") return RE::ActorValue::kMagicka;
    return RE::ActorValue::kNone;
}

// Restoration consumes the item's real authored effect; never add an independent actor-value adjustment.
bool Restores(RE::AlchemyItem *potion, RE::ActorValue value)
{
    if (!potion || potion->IsPoison() || value == RE::ActorValue::kNone) return false;
    for (auto effect : potion->effects)
        if (effect && effect->baseEffect && !effect->baseEffect->IsDetrimental() &&
            effect->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kValueModifier &&
            effect->baseEffect->data.primaryAV == value && effect->effectItem.magnitude > 0 &&
            effect->effectItem.duration == 0 && !effect->conditions.head && !effect->baseEffect->conditions.head)
            return true;
    return false;
}

// Single-target, single-effect authored scroll families with observable postconditions only.
bool SupportedScroll(RE::ScrollItem *scroll)
{
    if (!scroll || scroll->effects.size() != 1 || scroll->GetDelivery() == RE::MagicSystem::Delivery::kSelf ||
        scroll->GetDelivery() == RE::MagicSystem::Delivery::kTargetLocation) return false;
    auto effect = scroll->effects[0];
    if (!effect || !effect->baseEffect || effect->effectItem.area || effect->conditions.head ||
        effect->baseEffect->conditions.head || effect->baseEffect->data.explosion) return false;
    auto base = effect->baseEffect;
    auto archetype = base->GetArchetype();
    if (archetype == RE::EffectArchetypes::ArchetypeID::kParalysis ||
        archetype == RE::EffectArchetypes::ArchetypeID::kCalm ||
        archetype == RE::EffectArchetypes::ArchetypeID::kDemoralize ||
        archetype == RE::EffectArchetypes::ArchetypeID::kFrenzy) return true;
    if (archetype != RE::EffectArchetypes::ArchetypeID::kValueModifier &&
        archetype != RE::EffectArchetypes::ArchetypeID::kDualValueModifier) return false;
    auto value = base->data.primaryAV;
    return value == RE::ActorValue::kHealth || value == RE::ActorValue::kStamina || value == RE::ActorValue::kMagicka;
}

// Use the same conservative world-item eligibility at snapshot and execution time.
bool CanPickUp(RE::TESObjectREFR *target)
{
    if (!target || target->HasQuestObject() || target->extraList.GetCount() != 1)
        return false;
    auto base = target->GetBaseObject();
    if (!base || !base->GetPlayable())
        return false;
    switch (base->GetFormType())
    {
    case RE::FormType::Weapon: case RE::FormType::Armor: case RE::FormType::AlchemyItem:
    case RE::FormType::Ingredient: case RE::FormType::Misc: case RE::FormType::Book:
    case RE::FormType::Scroll: case RE::FormType::KeyMaster: case RE::FormType::SoulGem:
        return true;
    default: return false;
    }
}

void RunStep(const std::shared_ptr<Request> &r);
void StartReaction(const std::shared_ptr<Request> &r);

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

// Start generation independently; the utterance gate holds every reply until narration completes.
void StartReaction(const std::shared_ptr<Request> &r)
{
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
        SKSE::log::info("[INTERACT] Reaction prefetch {}", r->id);
        json context = {{"id", r->id}, {"target_ref", std::format("{:08X}", actor->GetFormID())}};
        HTTPManager::streamForActor(std::format("chatnf_interact_reaction|{}|{}|{}", getCurrentTimeMillis(),
                                                GetGameTimeStamp(), context.dump()),
                                    actor, PlayerConversationRoutingPolicy::RequestEligibility::ExplicitTarget);
    });
}

// Receipts are retriable reports; mutations are never retried by this request.
void Finish(const std::shared_ptr<Request> &r)
{
    if (!Live(r))
        return;
    r->allowedStep = -2;
    PrismaUIBridge::HideItemInteraction();
    json payload = {{"op", "receipt"}, {"id", r->id}, {"gamets", GetGameTimeStamp()}, {"receipts", r->receipts}, {"defer_audio", true}};
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
                if (r->snapshot["target"].value("actor", false))
                    StartReaction(r);
                ThreadPool::getInstance().enqueue("InteractNarratorAudio", [r, line] {
                    auto audio = HTTPManager::postGameDataJson("item_interaction.php",
                        {{"op", "audio"}, {"id", r->id}}, 120000);
                    if (!audio.value("ok", false) && PlaythroughSession::Allowed(r->epoch) &&
                        r->cancellationGeneration == reactionCancellationGeneration.load() &&
                        r->dialogueGeneration == PrismaUIBridge::GetDialogueStopGeneration())
                        audio = HTTPManager::postGameDataJson("item_interaction.php",
                            {{"op", "audio"}, {"id", r->id}}, 120000);
                    SKSE::GetTaskInterface()->AddTask([r, line, audio] {
                        if (!PlaythroughSession::Allowed(r->epoch) || !ChimInteraction::Enabled() ||
                            r->dialogueGeneration != PrismaUIBridge::GetDialogueStopGeneration() ||
                            r->cancellationGeneration != reactionCancellationGeneration.load())
                            return;
                        if (audio.value("ok", false))
                            SpeakManager::getInstance().insertInQueue(line);
                        else
                        {
                            NarrationComplete(line.utteranceId, false);
                            RE::DebugNotification("[CHIM] Effects finished; narration audio could not be prepared.");
                        }
                    });
                });
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
    if (effect == "disarm" || effect == "unequip")
    {
        const int slot = static_cast<int>(value);
        auto captured = r->equipment.find(slot);
        RE::TESBoundObject *object = nullptr;
        RE::ExtraDataList *extra = nullptr;
        if (!actor || actor->IsDead() || captured == r->equipment.end() ||
            (effect == "disarm") != (slot < 2) || !InventoryChoice(actor, captured->second, 1, object, extra) ||
            !extra || (!extra->HasType(RE::ExtraDataType::kWorn) && !extra->HasType(RE::ExtraDataType::kWornLeft)) ||
            actor->GetInventory().at(object).second->IsQuestObject())
        {
            Complete(r->id, r->step, "failed", "The captured equipped instance or slot changed.");
            return;
        }
        RE::TESForm *currentSlot = slot < 2 ? actor->GetEquippedObject(slot == 1)
            : actor->GetWornArmor(static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << (slot - 30)));
        if (currentSlot != object)
        {
            Complete(r->id, r->step, "failed", "The captured equipment slot changed.");
            return;
        }
        RE::BGSEquipSlot *equipSlot = nullptr;
        if (slot < 2)
        {
            auto defaults = RE::BGSDefaultObjectManager::GetSingleton();
            auto weapon = object->As<RE::TESObjectWEAP>();
            equipSlot = weapon ? weapon->GetEquipSlot() : nullptr;
            auto either = defaults ? defaults->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kEitherHandEquip) : nullptr;
            if (equipSlot && equipSlot == either)
                equipSlot = defaults->GetObject<RE::BGSEquipSlot>(slot == 1 ? RE::DEFAULT_OBJECT::kLeftHandEquip
                                                                                       : RE::DEFAULT_OBJECT::kRightHandEquip);
            const auto wornType = slot == 1 ? RE::ExtraDataType::kWornLeft : RE::ExtraDataType::kWorn;
            if (!extra->HasType(wornType))
            {
                Complete(r->id, r->step, "failed", "The captured weapon changed hands.");
                return;
            }
            if (!equipSlot)
            {
                Complete(r->id, r->step, "failed", "The captured hand is unavailable.");
                return;
            }
        }
        RE::ActorEquipManager::GetSingleton()->UnequipObject(actor, object, extra, 1, equipSlot, false, false, true, true);
        RE::TESBoundObject *remaining = nullptr;
        RE::ExtraDataList *remainingExtra = nullptr;
        if (!InventoryChoice(actor, captured->second, 1, remaining, remainingExtra) || !remainingExtra ||
            remainingExtra->HasType(RE::ExtraDataType::kWorn) || remainingExtra->HasType(RE::ExtraDataType::kWornLeft))
        {
            Complete(r->id, r->step, "unknown", "Unequip requested; exact instance state could not be confirmed.");
            return;
        }
        if (effect == "unequip")
        {
            Complete(r->id, r->step, "succeeded", "Captured armor unequipped and retained in the target inventory.");
            return;
        }
        const int before = actor->GetInventoryCounts()[object];
        auto dropped = actor->RemoveItem(object, 1, RE::ITEM_REMOVE_REASON::kDropping, remainingExtra, nullptr).get();
        bool changed = dropped && dropped->GetBaseObject() == object && dropped->extraList.GetCount() == 1 &&
                       actor->GetInventoryCounts()[object] == before - 1;
        Complete(r->id, r->step, changed ? "succeeded" : "unknown",
                 changed ? "Captured weapon unequipped and dropped as an actual world reference."
                         : "Captured weapon unequipped; its drop could not be confirmed.");
        return;
    }
    if (effect == "pickup")
    {
        if (!CanPickUp(target.get()))
        {
            Complete(r->id, r->step, "failed", "The target is no longer one eligible loose item.");
            return;
        }
        r->pickupIssued = true;
    }
    const auto restoration = RestorationValue(effect);
    if (effect == "give" || effect == "store" || effect == "consume" || effect == "equip" || effect == "magic" ||
        effect == "drop" || effect == "place" || restoration != RE::ActorValue::kNone)
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        RE::TESBoundObject *object = nullptr;
        RE::ExtraDataList *extra = nullptr;
        const int count = (effect == "give" || effect == "store" || effect == "drop" || effect == "place") ? static_cast<int>(value) : 1;
        if (!r->hasItem || count > r->quantity || !InventoryChoice(player, r->selected, count, object, extra))
        {
            Complete(r->id, r->step, "failed", "The exact selected inventory instance is no longer available.");
            return;
        }
        if (restoration != RE::ActorValue::kNone && (!actor || actor->IsDead() || !actor->AsActorValueOwner() || !Restores(object->As<RE::AlchemyItem>(), restoration)))
        {
            Complete(r->id, r->step, "failed", "The selected item cannot restore the requested living target statistic.");
            return;
        }
        if (effect == "drop" || effect == "place")
        {
            if (player->GetInventory().at(object).second->IsQuestObject() ||
                (effect == "place" && player->GetParentCell() != target->GetParentCell()))
            {
                Complete(r->id, r->step, "failed", "The item or placement cell is no longer eligible.");
                return;
            }
            const int before = player->GetInventoryCounts()[object];
            auto position = target->GetPosition();
            position.z += 20.0f;
            auto dropped = player->RemoveItem(object, count, RE::ITEM_REMOVE_REASON::kDropping, extra, nullptr,
                                              effect == "place" ? &position : nullptr).get();
            bool removed = before - player->GetInventoryCounts()[object] == count;
            r->inventoryMoved = removed;
            bool placed = dropped && dropped->GetBaseObject() == object && dropped->extraList.GetCount() == count;
            if (effect == "place" && placed)
                placed = dropped->GetPosition().GetDistance(position) < 100.0f;
            Complete(r->id, r->step, removed && placed ? "succeeded" : "unknown",
                     removed && placed ? "Exact selected item dropped; world reference and inventory decrease confirmed."
                                       : "Drop requested once; both inventory and world reference were not confirmed.");
            return;
        }
        if (effect == "magic")
        {
            auto scroll = object->As<RE::ScrollItem>();
            if (!SupportedScroll(scroll) || !actor || actor->IsDead())
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
            auto authored = scroll->effects[0]->baseEffect;
            float amount = static_cast<float>(authored->data.primaryAV);
            if (authored->IsDetrimental()) amount = -amount;
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
        if (effect == "consume" || restoration != RE::ActorValue::kNone)
        {
            auto potion = object->As<RE::AlchemyItem>();
            const auto beforeConsumption = actor->GetInventoryCounts()[object];
            auto stats = actor->AsActorValueOwner();
            const float previous = stats && restoration != RE::ActorValue::kNone ? stats->GetActorValue(restoration) : 0;
            bool accepted = potion && actor->DrinkPotion(potion, transferred);
            const auto afterConsumption = actor->GetInventoryCounts()[object];
            bool consumed = accepted && afterConsumption == beforeConsumption - 1;
            if (restoration != RE::ActorValue::kNone && consumed)
            {
                if (!stats)
                {
                    Complete(r->id, r->step, "unknown", "Item transferred and consumed; statistic verification unavailable.");
                    return;
                }
                auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
                RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
                std::string id = r->id;
                int index = r->step;
                std::string statistic = "Health";
                if (restoration == RE::ActorValue::kStamina) statistic = "Stamina";
                else if (restoration == RE::ActorValue::kMagicka) statistic = "Magicka";
                auto recipient = actor;
                float beforeValue = previous;
                if (!vm || !vm->DispatchStaticCall("CHIMItemInteraction", "VerifyRestoration",
                    RE::MakeFunctionArguments(std::move(id), std::move(index), std::move(recipient),
                                              std::move(statistic), std::move(beforeValue)), callback))
                    Complete(r->id, r->step, "unknown", "Item transferred and consumed; statistic verification unavailable.");
                return;
            }
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
            {"consume_world", "Consume"}, {"heal", "Heal"}, {"restore_stamina", "Restore stamina of"},
            {"restore_magicka", "Restore magicka of"}, {"drop", "Drop"}, {"place", "Place"},
            {"disarm", "Disarm"}, {"unequip", "Unequip armor from"}};
        const auto label = labels.find(effect);
        std::string action = label != labels.end() ? label->second : "Interaction with";
        const auto targetName = r->snapshot["target"].value("name", "target");
        if (effect == "give" || effect == "store")
            action += std::format(" {} {} {} {}", completedStep.at("value").get<int>(), r->selected.name,
                                  effect == "store" ? "in" : "to", targetName);
        else if (effect == "place")
            action += std::format(" {} {} near {}", completedStep.at("value").get<int>(), r->selected.name, targetName);
        else if (effect == "drop")
            action += std::format(" {} {}", completedStep.at("value").get<int>(), r->selected.name);
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

// Return true only for tagged reaction lines handled (held or discarded) by this gate.
bool HoldReaction(const ScriptLine &line, bool playback, RE::FormID playbackActor)
{
    const std::string prefix = "interact-reply-";
    if (!line.utteranceId.starts_with(prefix))
        return false;
    const auto id = line.utteranceId.substr(prefix.size(), 32);
    std::lock_guard lock(currentMutex);
    auto found = pendingReactions.find("interact-" + id);
    if (found == pendingReactions.end())
        return true;
    const auto &r = found->second;
    auto ref = r->target.get();
    auto actor = ref ? ref->As<RE::Actor>() : nullptr;
    if (!PlaythroughSession::Allowed(r->epoch) || !ChimInteraction::Enabled() ||
        r->dialogueGeneration != PrismaUIBridge::GetDialogueStopGeneration() ||
        r->cancellationGeneration != reactionCancellationGeneration.load() ||
        std::chrono::steady_clock::now() >= r->reactionExpires || !actor || actor->IsDead() ||
        actor->IsDisabled() || actor->IsDeleted() || !actor->Is3DLoaded() ||
        line.actor != r->snapshot["target"].value("speaker", ""))
    {
        pendingReactions.erase(found);
        return true;
    }
    if (playback && actor->GetFormID() != playbackActor)
        return true;
    bool exactAgent = false;
    for (const auto &agent : AIAgentManager::getInstance().getAgents())
        if (agent && !agent->isNarrator() && agent->getActor() == actor && agent->getActorName() == line.actor)
            exactAgent = true;
    if (!exactAgent)
    {
        pendingReactions.erase(found);
        return true;
    }
    if (r->reactionReleased && (releasingReaction || playback))
        return false;
    if (playback)
        return true;
    if (r->reactionLines.size() >= 32 || r->reactionBytes + line.subtitle.size() > 32768)
    {
        pendingReactions.erase(found);
        return true;
    }
    r->reactionBytes += line.subtitle.size();
    r->reactionLines.push_back(line);
    return true;
}

// Only the completion acknowledgement opens the gate; Tick drains chunks in arrival order.
void NarrationComplete(const std::string &utteranceId, bool completed)
{
    std::lock_guard lock(currentMutex);
    auto found = pendingReactions.find(utteranceId);
    if (found == pendingReactions.end())
        return;
    SKSE::log::info("[INTERACT] Narration gate {} completed={} held={}", found->second->id, completed,
                    found->second->reactionLines.size());
    if (!completed)
        pendingReactions.erase(found);
    else
        found->second->reactionReleased = true;
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
        std::vector<ScriptLine> ready;
        {
            std::lock_guard lock(currentMutex);
            for (auto &[id, pending] : pendingReactions)
                if (pending->reactionReleased)
                {
                    ready.insert(ready.end(), pending->reactionLines.begin(), pending->reactionLines.end());
                    pending->reactionLines.clear();
                }
        }
        // Incoming chunks keep buffering while this ordered batch is inserted without the state lock.
        releasingReaction = true;
        for (const auto &line : ready)
            SpeakManager::getInstance().insertInQueue(line);
        releasingReaction = false;
        auto r = Current();
        if (!r)
            return;
        if (!Live(r))
        {
            Cancel();
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
            if (input.contains("cheat_mode") && !input.at("cheat_mode").is_boolean())
            {
                ShowInputError("Cheat Mode must be on or off.");
                return;
            }
            const bool cheatMode = input.value("cheat_mode", false);
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
            if (CanPickUp(target.get()))
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
            if (actor && !actor->IsDead())
                for (const auto &effect : {"heal", "restore_stamina", "restore_magicka"})
                    if (Restores(potion, RestorationValue(effect))) r->allowed.push_back(effect);
            if (item && !questItem)
            {
                r->allowed.push_back("drop");
                if (player->GetParentCell() == target->GetParentCell()) r->allowed.push_back("place");
            }
            if (actor && item && (item->As<RE::TESObjectWEAP>() || item->As<RE::TESObjectARMO>()))
                r->allowed.push_back("equip");
            json equipmentData = json::array();
            if (actor && !actor->IsDead())
            {
                auto wornInventory = actor->GetInventory();
                for (int slot = 0; slot <= 61; ++slot)
                {
                    if (slot > 1 && slot < 30) continue;
                    RE::TESBoundObject *worn = nullptr;
                    if (slot < 2)
                    {
                        auto form = actor->GetEquippedObject(slot == 1);
                        worn = form ? form->As<RE::TESObjectWEAP>() : nullptr;
                    }
                    else worn = actor->GetWornArmor(static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << (slot - 30)));
                    if (!worn) continue;
                    auto found = wornInventory.find(worn);
                    if (found == wornInventory.end() || !found->second.second || found->second.second->IsQuestObject() ||
                        !found->second.second->extraLists) continue;
                    RE::ExtraDataList *exact = nullptr;
                    int copies = 0;
                    for (auto list : *found->second.second->extraLists)
                        if (list && (list->HasType(RE::ExtraDataType::kWorn) || list->HasType(RE::ExtraDataType::kWornLeft)))
                        { exact = list; ++copies; }
                    if (copies != 1) continue;
                    r->equipment[slot] = {worn->GetFormID(), exact, 1, worn->GetName(), Fingerprint(worn, exact)};
                    equipmentData.push_back({{"slot", slot}, {"name", worn->GetName()},
                                             {"action", slot < 2 ? "disarm" : "unequip"}});
                }
                for (const auto &[slot, choice] : r->equipment)
                {
                    const std::string effect = slot < 2 ? "disarm" : "unequip";
                    if (std::find(r->allowed.begin(), r->allowed.end(), effect) == r->allowed.end()) r->allowed.push_back(effect);
                }
            }
            // Scrolls retain authored effects; expose only the observable single-target families above.
            auto scroll = item ? item->As<RE::ScrollItem>() : nullptr;
            bool supportedMagic = actor && !actor->IsDead() && SupportedScroll(scroll);
            if (supportedMagic) r->allowed.push_back("magic");
            if (questItem)
            {
                json filtered = json::array();
                for (const auto &effect : r->allowed)
                {
                    if (effect != "give" && effect != "store" && effect != "consume" && effect != "equip" &&
                        effect != "magic" && effect != "heal" && effect != "restore_stamina" && effect != "restore_magicka")
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
            json targetData = {{"equipment", equipmentData}, {"name", target->GetName()},
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
                targetData["stamina"] = targetStats->GetActorValue(RE::ActorValue::kStamina);
                targetData["max_stamina"] = targetStats->GetPermanentActorValue(RE::ActorValue::kStamina);
                targetData["magicka"] = targetStats->GetActorValue(RE::ActorValue::kMagicka);
                targetData["max_magicka"] = targetStats->GetPermanentActorValue(RE::ActorValue::kMagicka);
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
                            {"cheat_mode", cheatMode},
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
                                {"magic", {0, 0}},         {"combat", {0, 0}}, {"heal", {1, 1}},
                                {"restore_stamina", {1, 1}}, {"restore_magicka", {1, 1}},
                                {"drop", {1, 100}}, {"place", {1, 100}}, {"disarm", {0, 1}}, {"unequip", {30, 61}}};
                            if ((effect == "pickup" || effect == "consume_world") && ++pickupSteps > 1)
                                throw std::runtime_error("Repeated pickup");
                            const auto bounds = limits.at(effect);
                            if (!std::isfinite(value) || value < bounds.first || value > bounds.second ||
                                !s.at("alive").is_boolean())
                                throw std::runtime_error("Invalid value");
                            if ((effect == "give" || effect == "store" || effect == "consume" || effect == "equip" ||
                                 effect == "lock" || effect == "drop" || effect == "place" || effect == "disarm" || effect == "unequip") &&
                                std::floor(value) != value)
                                throw std::runtime_error("Fractional quantity");
                            if ((effect == "combat" || effect == "disarm" || effect == "unequip" || RestorationValue(effect) != RE::ActorValue::kNone) && !s.at("alive").get<bool>())
                                throw std::runtime_error("Combat requires living target");
                            for (auto dep : s.at("requires"))
                                if (!dep.is_number_integer() || dep.get<int>() < 0 || dep.get<std::size_t>() >= index)
                                    throw std::runtime_error("Invalid dependency");
                            if (effect == "kill" || effect == "disable")
                                confirm = true;
                            if (effect == "give" || effect == "store" || effect == "consume" || effect == "equip" ||
                                effect == "magic" || effect == "drop" || effect == "place" || RestorationValue(effect) != RE::ActorValue::kNone)
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
