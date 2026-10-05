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
    RE::ObjectRefHandle target;
    std::vector<Choice> choices;
    Choice selected{};
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
std::mutex currentMutex;
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
    bool blocked = !target || target->IsDisabled() || !target->Is3DLoaded() || target->IsDeleted() ||
                   RE::PlayerCharacter::GetSingleton()->GetPosition().GetDistance(target->GetPosition()) > 512.0f;
    RE::TESBoundObject *held = nullptr;
    RE::ExtraDataList *heldExtra = nullptr;
    if (!r->inventoryMoved && !InventoryChoice(RE::PlayerCharacter::GetSingleton(), r->selected, 1, held, heldExtra))
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
    if (effect == "give" || effect == "store" || effect == "consume" || effect == "equip" || effect == "magic")
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        RE::TESBoundObject *object = nullptr;
        RE::ExtraDataList *extra = nullptr;
        const int count = (effect == "give" || effect == "store") ? static_cast<int>(value) : 1;
        if (count > r->quantity || !InventoryChoice(player, r->selected, count, object, extra))
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
            bool consumed = potion && actor->DrinkPotion(potion, transferred);
            Complete(r->id, r->step, consumed ? "succeeded" : "failed",
                     consumed ? "Item transferred and its real potion or food effects consumed."
                              : "Item transferred but consumption failed.");
        }
        else
        {
            RE::ActorEquipManager::GetSingleton()->EquipObject(actor, object, transferred, 1, nullptr, false, false,
                                                               true, true);
            bool equipped = actor->GetEquippedObject(false) == object || actor->GetEquippedObject(true) == object;
            if (object->As<RE::TESObjectARMO>())
                equipped = entry != targetInventory.end() && entry->second.second->IsWorn();
            Complete(r->id, r->step, equipped ? "succeeded" : "unknown",
                     equipped ? "Item transferred and equipped."
                              : "Item transferred; equipment state could not be confirmed.");
        }
        return;
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
        r->receipts.push_back({{"status", status}, {"detail", detail}});
        RunStep(r);
    });
}
void Tick()
{
    static std::atomic<bool> queued = false;
    if (queued.exchange(true))
        return;
    SKSE::GetTaskInterface()->AddTask([] {
        queued = false;
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
            r->step = static_cast<int>(r->plan["steps"].size());
            Finish(r);
        }
    });
}
void Cancel()
{
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
        target.get() == player || player->GetPosition().GetDistance(target->GetPosition()) > 512.0f ||
        RE::UI::GetSingleton()->GameIsPaused() || PrismaUIBridge::IsAnyHotkeyPanelFocused() ||
        RE::UI::GetSingleton()->IsMenuOpen(RE::DialogueMenu::MENU_NAME) ||
        RE::UI::GetSingleton()->IsMenuOpen(RE::Console::MENU_NAME) ||
        RE::UI::GetSingleton()->IsMenuOpen(RE::LoadingMenu::MENU_NAME))
    {
        RE::DebugNotification("[CHIM] Aim at a nearby loaded target before using Interact.");
        return;
    }
    auto r = std::make_shared<Request>();
    r->epoch = PlaythroughSession::Generation();
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
            const auto key = input.at("key").get<std::size_t>();
            if (key >= r->choices.size())
            {
                ShowInputError("Choose an inventory item before interacting.");
                return;
            }
            r->selected = r->choices[key];
            r->quantity = input.at("quantity").get<int>();
            const auto intent = input.at("intent").get<std::string>();
            if (r->quantity < 1 || r->quantity > 100 || r->quantity > r->selected.count || intent.empty() ||
                intent.size() > 4000)
            {
                ShowInputError("Enter a description and a whole quantity within the available amount.");
                return;
            }
            auto target = r->target.get();
            auto player = RE::PlayerCharacter::GetSingleton();
            RE::TESBoundObject *item = nullptr;
            RE::ExtraDataList *extra = nullptr;
            if (!target || !InventoryChoice(player, r->selected, r->quantity, item, extra))
            {
                ShowInputError(
                    "The target or selected item has changed. Choose another item, or close and reopen Interact.");
                return;
            }
            auto actor = target->As<RE::Actor>();
            auto base = target->GetBaseObject();
            auto inventory = player->GetInventory();
            bool questItem = inventory.at(item).second->IsQuestObject();
            r->allowed = json::array({"observe", "activate", "resize", "disable"});
            if (actor)
            {
                for (auto effect : {"give", "injure", "kill", "push", "combat"})
                    r->allowed.push_back(effect);
            }
            if (base->GetFormType() == RE::FormType::Container)
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
            auto potion = item->As<RE::AlchemyItem>();
            if (actor && potion && !potion->IsPoison())
                r->allowed.push_back("consume");
            if (actor && (item->As<RE::TESObjectWEAP>() || item->As<RE::TESObjectARMO>()))
                r->allowed.push_back("equip");
            // Scrolls carry authored spells; restrict to the small known vanilla elemental/paralysis family.
            auto scroll = item->As<RE::ScrollItem>();
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
            json itemData = {{"name", r->selected.name},
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
            json targetData = {{"name", target->GetName()},
                               {"actor", actor != nullptr},
                               {"scale", target->GetScale()},
                               {"distance", player->GetPosition().GetDistance(target->GetPosition())},
                               {"awareness", "unknown"}};
            if (actor)
            {
                targetData["health"] = actor->GetActorValue(RE::ActorValue::kHealth);
                targetData["dead"] = actor->IsDead();
                targetData["combat"] = actor->IsInCombat();
                targetData["essential"] = actor->IsEssential();
                targetData["max_health"] = actor->GetPermanentActorValue(RE::ActorValue::kHealth);
                targetData["race"] = actor->GetRace() ? actor->GetRace()->GetName() : "unknown";
                targetData["armor_rating"] = actor->GetActorValue(RE::ActorValue::kDamageResist);
                targetData["magic_resistance"] = actor->GetActorValue(RE::ActorValue::kResistMagic);
                targetData["fire_resistance"] = actor->GetActorValue(RE::ActorValue::kResistFire);
                targetData["frost_resistance"] = actor->GetActorValue(RE::ActorValue::kResistFrost);
                targetData["shock_resistance"] = actor->GetActorValue(RE::ActorValue::kResistShock);
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
                 {{"health", player->GetActorValue(RE::ActorValue::kHealth)},
                  {"stamina", player->GetActorValue(RE::ActorValue::kStamina)},
                  {"magicka", player->GetActorValue(RE::ActorValue::kMagicka)},
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
                        for (std::size_t index = 0; index < r->plan["steps"].size(); ++index)
                        {
                            const auto &s = r->plan["steps"][index];
                            const auto effect = s.at("effect").get<std::string>();
                            if (std::find(r->allowed.begin(), r->allowed.end(), effect) == r->allowed.end())
                                throw std::runtime_error("Unsupported effect");
                            const float value = s.at("value").get<float>();
                            static const std::map<std::string, std::pair<float, float>> limits = {
                                {"observe", {0, 0}},    {"give", {1, 100}},    {"store", {1, 100}},
                                {"consume", {1, 1}},    {"equip", {1, 1}},     {"injure", {1, 100}},
                                {"kill", {0, 0}},       {"push", {1, 10}},     {"lock", {0, 100}},
                                {"unlock", {0, 0}},     {"activate", {0, 0}},  {"open", {0, 0}},
                                {"close", {0, 0}},      {"destroy", {1, 100}}, {"disable", {0, 0}},
                                {"resize", {0.25f, 2}}, {"magic", {0, 0}},     {"combat", {0, 0}}};
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
