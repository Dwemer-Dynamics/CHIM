#include "ItemInteraction.h"
#include "ChimInteraction.h"
#include "Globals.h"
#include "HTTPManager.h"
#include "Misc.h"
#include "PlaythroughSession.h"
#include "PrismaUIBridge.h"
#include "SpeakManager.h"
#include "SpatialAwareness.h"
#include "ThreadPool.h"
#include <atomic>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <set>

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
struct MagicChoice
{
    RE::FormID form = 0;
    RE::FormID spell = 0;
    int rank = 0;
    std::string kind;
};
struct Request
{
    std::string id;
    std::string failureSceneToken;
    std::uint64_t epoch;
    std::uint64_t dialogueGeneration = 0;
    std::uint64_t cancellationGeneration = 0;
    std::chrono::steady_clock::time_point reactionExpires{};
    RE::ObjectRefHandle target;
    std::vector<Choice> choices;
    std::vector<MagicChoice> magicChoices;
    std::optional<MagicChoice> selectedMagic;
    std::vector<RE::FormID> dispellableSpells;
    std::map<RE::FormID, std::set<std::pair<std::uintptr_t, std::uint16_t>>> dispelInstances;
    RE::ObjectRefHandle magicRecipient;
    float magicHealthBefore = 0;
    std::vector<std::uintptr_t> magicEffectsBefore;
    bool magicIssued = false;
    Choice selected{};
    std::map<int, Choice> equipment;
    bool hasItem = false;
    bool pickupIssued = false;
    int quantity = 1;
    int step = -1;
    int finalizedInjuryStep = -1;
    std::atomic<int> allowedStep{-1};
    bool submitted = false;
    bool inventoryMoved = false;
    std::chrono::steady_clock::time_point deadline{};
    RE::FormID pendingSceneryShader = 0;
    std::vector<RE::NiPointer<RE::ShaderReferenceEffect>> priorSceneryShaders;
    RE::FormID pendingStatusSpell = 0;
    bool pendingStagger = false;
    std::chrono::steady_clock::time_point statusCheckUntil{};
    json snapshot;
    json allowed;
    json plan;
    json receipts = json::array();
    std::vector<std::string> narrationChunks;
    std::set<std::string> narrationCompleted;
    std::string narrationText;
    bool narrationStreamDone = false;
    bool narrationAcknowledged = false;
    bool reactionReleased = false;
    std::vector<ScriptLine> reactionLines;
    std::size_t reactionBytes = 0;
};
std::shared_ptr<Request> current;
std::map<std::string, std::shared_ptr<Request>> pendingReactions;
// Retain only our short-lived scenery shaders so refresh never stops another mod's effect.
std::map<std::pair<RE::FormID, std::string>, RE::NiPointer<RE::ShaderReferenceEffect>> sceneryFire;
std::uint64_t sceneryFireEpoch = 0;
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

// Persistent authored spell variants let Skyrim own expiry and save/load persistence.
int StatusFamily(const std::string &effect)
{
    static const std::vector<std::string> names = {"poison", "burning", "paralysis", "calm", "fear", "frenzy", "frost", "shock", "drain_stamina", "drain_magicka", "slow", "haste", "weaken_armor", "fortify_armor", "weaken_weapon", "fortify_weapon", "absorb_health", "absorb_stamina", "absorb_magicka", "ethereal", "soul_trap", "reanimate", "turn_undead", "banish"};
    auto it = std::find(names.begin(), names.end(), effect);
    return it == names.end() ? -1 : static_cast<int>(it - names.begin());
}
RE::SpellItem *StatusSpell(int family, int duration)
{
    static const std::vector<int> durations = {5, 10, 20, 30};
    auto it = std::find(durations.begin(), durations.end(), duration);
    if (family < 0 || family >= 24 || it == durations.end()) return nullptr;
    auto data = RE::TESDataHandler::GetSingleton();
    const auto spellLocal = family < 6 ? 0x60010 + family * 4 : 0x60200 + (family - 6) * 4;
    const auto effectLocal = family < 6 ? 0x60000 + family : 0x60100 + (family - 6) * 4;
    auto spell = data ? data->LookupForm<RE::SpellItem>(spellLocal + static_cast<int>(it - durations.begin()), "AIAgent.esp") : nullptr;
    const std::size_t count = family == 6 ? 2 : 1;
    if (!spell || spell->effects.size() != count || spell->GetDelivery() != RE::MagicSystem::Delivery::kTargetActor ||
        spell->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget) return nullptr;
    for (std::size_t index = 0; index < count; ++index)
    {
        auto expected = data->LookupForm<RE::EffectSetting>(effectLocal + index, "AIAgent.esp");
        auto part = spell->effects[index];
        if (!part || !expected || part->baseEffect != expected || expected->data.projectileBase ||
            expected->data.delivery != RE::MagicSystem::Delivery::kTargetActor ||
            expected->data.castingType != RE::MagicSystem::CastingType::kFireAndForget || part->effectItem.area != 0 ||
            part->effectItem.duration != static_cast<std::uint32_t>(family == 23 ? 0 : duration)) return nullptr;
    }
    return spell;
}
RE::TESEffectShader *SceneryFireShader(const std::string &effect = "burning_visual")
{
    auto spell = StatusSpell(effect == "frost_visual" ? 6 : ((effect == "shock_visual" || effect == "impact_burst") ? 7 : 1), 10);
    return spell ? spell->effects[0]->baseEffect->data.effectShader : nullptr;
}

// Observe only a live application of this exact authored spell, never a dormant/dispelled entry.
bool HasAppliedStatus(RE::Actor *actor, RE::SpellItem *spell, std::string &detail)
{
    auto magic = actor ? actor->AsMagicTarget() : nullptr;
    auto effects = magic ? magic->GetActiveEffectList() : nullptr;
    if (!effects || !spell) return false;
    std::size_t confirmed = 0;
    for (auto part : spell->effects)
    {
        bool found = false;
        for (auto active : *effects)
            if (active && active->spell == spell && active->GetCasterActor().get() == RE::PlayerCharacter::GetSingleton() && active->GetBaseObject() == part->baseEffect &&
                active->duration > active->elapsedSeconds && active->conditionStatus != RE::ActiveEffect::ConditionStatus::kFalse &&
                !active->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled) &&
                std::isfinite(active->magnitude) &&
                (part->baseEffect->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kNoMagnitude) || std::abs(active->magnitude) > 0.0f))
            { found = true; break; }
        if (found) ++confirmed;
    }
    if (confirmed != spell->effects.size()) return false;
    detail = std::format("All {} authored status components active. Engine owns expiry; future effects, resistance and rendered visuals are not guaranteed.", confirmed);
    return true;
}

bool KnownMagic(RE::PlayerCharacter *player, const MagicChoice &choice)
{
    if (!player) return false;
    auto spell = RE::TESForm::LookupByID<RE::SpellItem>(choice.spell);
    if (!spell) return false;
    if (choice.kind == "shout")
    {
        auto shout = RE::TESForm::LookupByID<RE::TESShout>(choice.form);
        return shout && player->HasShout(shout) && choice.rank >= 1 && choice.rank <= 3 &&
               shout->variations[choice.rank - 1].spell == spell;
    }
    const auto type = spell->GetSpellType();
    return player->HasSpell(spell) && spell->GetPlayable() &&
           (type == RE::MagicSystem::SpellType::kSpell || type == RE::MagicSystem::SpellType::kPower ||
            type == RE::MagicSystem::SpellType::kLesserPower);
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

// Human-readable form categories keep the Director from guessing mechanics from item names.
json Effects(RE::MagicItem *item);
json MagicSnapshot(const MagicChoice &choice)
{
    auto spell = RE::TESForm::LookupByID<RE::SpellItem>(choice.spell);
    auto form = RE::TESForm::LookupByID(choice.form);
    if (!spell || !form) return nullptr;
    static const std::vector<std::string> deliveries = {"self", "touch", "aimed", "target_actor", "target_location"};
    const auto delivery = static_cast<std::size_t>(spell->GetDelivery());
    return {{"name", form->GetName()}, {"kind", choice.kind}, {"rank", choice.rank},
            {"delivery", delivery < deliveries.size() ? deliveries[delivery] : "unknown"},
            {"casting_type", spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration ? "concentration_single_application" : "fire_and_forget"},
            {"resource_cost_applied", false}, {"effect_verification", "new exact spell effect or matching health change; unobservable results remain unknown"}, {"effects", Effects(spell)}};
}

json MagicMenuChoices(const std::shared_ptr<Request> &r)
{
    json entries = json::array();
    for (std::size_t i = 0; i < r->magicChoices.size(); ++i)
    {
        const auto metadata = MagicSnapshot(r->magicChoices[i]);
        if (metadata.is_null()) continue;
        entries.push_back({{"key", i}, {"name", metadata["name"]}, {"kind", metadata["kind"]},
                           {"details", metadata["delivery"].get<std::string>() +
                               (r->magicChoices[i].rank ? " - unlocked rank " + std::to_string(r->magicChoices[i].rank) : "")}});
    }
    return entries;
}

// Human-readable form categories, not guesses from names or lore.
std::string SemanticType(RE::TESBoundObject *object)
{
    if (!object) return "unknown";
    if (object->As<RE::TESObjectWEAP>()) return "weapon";
    if (object->As<RE::TESObjectARMO>()) return "armor";
    if (auto potion = object->As<RE::AlchemyItem>())
    {
        if (potion->IsPoison()) return "poison";
        return potion->IsFood() ? "food" : "potion";
    }
    if (object->As<RE::ScrollItem>()) return "scroll";
    if (object->GetFormType() == RE::FormType::Ingredient) return "ingredient";
    if (object->GetFormType() == RE::FormType::Container) return "container";
    if (object->GetFormType() == RE::FormType::Door) return "door";
    if (object->GetFormType() == RE::FormType::Flora) return "harvestable plant";
    if (object->GetFormType() == RE::FormType::Tree) return "tree";
    if (object->GetFormType() == RE::FormType::Activator) return "activator";
    if (object->GetFormType() == RE::FormType::Static) return "static scenery";
    if (object->GetFormType() == RE::FormType::Book) return "book";
    if (object->GetFormType() == RE::FormType::Misc) return "miscellaneous object";
    return "other object";
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

// Bound player and target context identically; indefinite effects have no claimed expiry time.
json ActiveEffectSnapshot(RE::Actor *actor)
{
    json result = json::array();
    auto magic = actor ? actor->AsMagicTarget() : nullptr;
    if (auto effects = magic ? magic->GetActiveEffectList() : nullptr)
        for (auto active : *effects)
        {
            if (!active || !active->GetBaseObject() || active->conditionStatus == RE::ActiveEffect::ConditionStatus::kFalse ||
                active->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) continue;
            result.push_back({{"name", active->GetBaseObject()->GetName()}, {"magnitude", active->magnitude},
                {"remaining_seconds", active->duration > 0 ? json(std::max(0.0f, active->duration - active->elapsedSeconds)) : json(nullptr)}});
            if (result.size() == 8) break;
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
    json payload = {{"op", "receipt"}, {"id", r->id}, {"gamets", GetGameTimeStamp()}, {"receipts", r->receipts}, {"failure_scene_token", r->failureSceneToken}, {"defer_audio", true}};
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
                if (!n.is_object()) n = json::object();
                const auto parentId = n.contains("utterance_id") && n["utterance_id"].is_string() ? n["utterance_id"].get<std::string>() : std::string{};
                const auto chunks = n.value("chunks", json::array());
                bool validChunks = n.contains("text") && n["text"].is_string() && !n["text"].get<std::string>().empty() && chunks.is_array() && !chunks.empty() && chunks.size() <= 32 && parentId == "interact-" + r->id;
                std::set<std::string> chunkIds;
                if (validChunks)
                    for (std::size_t index = 0; index < chunks.size(); ++index)
                    {
                        const auto &chunk = chunks[index];
                        if (!chunk.is_object() || !chunk.contains("text") || !chunk["text"].is_string() ||
                            chunk["text"].get_ref<const std::string &>().empty() ||
                            !chunk.contains("utterance_id") || !chunk["utterance_id"].is_string() ||
                            chunk["utterance_id"] != parentId + "-" + std::to_string(index) ||
                            !chunk.contains("tts_cache_key") || !chunk["tts_cache_key"].is_string())
                        { validChunks = false; break; }
                        const auto cache = chunk["tts_cache_key"].get<std::string>();
                        if (cache.size() != 32 || cache.find_first_not_of("0123456789abcdef") != std::string::npos ||
                            !chunkIds.insert(chunk["utterance_id"].get<std::string>()).second)
                        { validChunks = false; break; }
                    }
                if (!validChunks)
                {
                    RE::DebugNotification("[CHIM] Effects finished; narration chunks were unavailable.");
                    std::lock_guard lock(currentMutex);
                    if (current == r) current.reset();
                    return;
                }
                for (const auto &chunk : chunks) r->narrationChunks.push_back(chunk.at("utterance_id").get<std::string>());
                r->narrationText = n.value("text", "");
                {
                    std::lock_guard lock(currentMutex);
                    if (pendingReactions.size() >= 8) pendingReactions.erase(pendingReactions.begin());
                    r->reactionExpires = std::chrono::steady_clock::now() + std::chrono::seconds(120);
                    pendingReactions[parentId] = r;
                }
                if (r->snapshot["target"].value("actor", false)) StartReaction(r);
                ThreadPool::getInstance().enqueue("InteractNarratorAudio", [r, parentId, chunks] {
                    std::size_t received = 0;
                    bool done = false;
                    const auto receive = [&](const json &message) {
                        if (!PlaythroughSession::Allowed(r->epoch) || !ChimInteraction::Enabled() ||
                            r->dialogueGeneration != PrismaUIBridge::GetDialogueStopGeneration() ||
                            r->cancellationGeneration != reactionCancellationGeneration.load() ||
                            !message.value("ok", false) || message.value("id", "") != r->id) return false;
                        if (message.value("done", false))
                        {
                            done = received == chunks.size() && message.value("chunk_count", 0u) == chunks.size();
                            return done;
                        }
                        const auto index = message.at("chunk_index").get<std::size_t>();
                        if (index >= chunks.size()) return false;
                        const auto &chunk = message.at("narration");
                        if (!chunk.value("audio_ready", false)) return false;
                        if (chunk.at("utterance_id") != chunks[index].at("utterance_id") ||
                            chunk.at("text") != chunks[index].at("text") ||
                            chunk.at("tts_cache_key") != chunks[index].at("tts_cache_key")) return false;
                        if (index < received) return true; // Cached replay after transport retry.
                        if (index != received) return false;
                        ++received;
                        ScriptLine line(chunk.at("text").get<std::string>(), "", "", "", NARRATOR_NAME, "", 1.0f, -1,
                                        "explicit_disable_rechat", chunk.at("utterance_id").get<std::string>());
                        line.ttsCacheKey = chunk.at("tts_cache_key").get<std::string>();
                        SKSE::GetTaskInterface()->AddTask([r, line] {
                            if (PlaythroughSession::Allowed(r->epoch) && ChimInteraction::Enabled() &&
                                r->dialogueGeneration == PrismaUIBridge::GetDialogueStopGeneration() &&
                                r->cancellationGeneration == reactionCancellationGeneration.load())
                                SpeakManager::getInstance().insertInQueue(line);
                        });
                        return true;
                    };
                    for (int attempt = 0; attempt < 2 && !done; ++attempt)
                        HTTPManager::postGameDataStream("item_interaction.php",
                            {{"op", "audio"}, {"id", r->id}, {"stream", true}}, receive, 120000);
                    if (done) SKSE::GetTaskInterface()->AddTask([r, parentId] {
                        std::lock_guard lock(currentMutex);
                        const auto found = pendingReactions.find(parentId);
                        if (found != pendingReactions.end() && found->second == r) r->narrationStreamDone = true;
                    });
                    else SKSE::GetTaskInterface()->AddTask([r, parentId] {
                        NarrationComplete(parentId, false);
                        if (PlaythroughSession::Allowed(r->epoch) && r->cancellationGeneration == reactionCancellationGeneration.load())
                            RE::DebugNotification("[CHIM] Effects finished; some narration audio could not be prepared.");
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

// Movement is limited to loose inventory objects, never actors, doors or static buildings.
bool MovableObject(RE::TESObjectREFR *target)
{
    return target && !target->As<RE::Actor>() && !target->HasQuestObject() && CanPickUp(target);
}

bool ActorKeyword(RE::Actor *actor, const char *name)
{
    auto keyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(name);
    return actor && keyword && actor->HasKeyword(keyword);
}
bool UndeadActor(RE::Actor *actor) { return ActorKeyword(actor, "ActorTypeUndead"); }


// Conservative point rays reject blocked paths and unsupported destinations; this is not a swept-volume test.
bool CheckedMove(RE::TESObjectREFR *target, const RE::NiPoint3 &destination)
{
    auto cell = target ? target->GetParentCell() : nullptr;
    auto world = cell ? cell->GetbhkWorld() : nullptr;
    if (!world) return false;
    const float scale = RE::bhkWorld::GetWorldScale();
    const auto ray = [&](RE::NiPoint3 from, RE::NiPoint3 to, bool wantHit) {
        RE::bhkPickData pick{};
        pick.rayInput.from = RE::hkVector4(from * scale);
        pick.rayInput.to = RE::hkVector4(to * scale);
        pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kCameraPick);
        const bool hit = world->PickObject(pick) && pick.rayOutput.HasHit();
        if (wantHit)
        {
            if (!hit || !pick.rayOutput.rootCollidable) return false;
            auto ref = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
            const float floorZ = from.z + (to.z - from.z) * pick.rayOutput.hitFraction;
            return floorZ <= destination.z + 2.0f && ref != target && (!ref || !ref->As<RE::Actor>());
        }
        return !hit;
    };
    const auto start = target->GetPosition();
    for (const RE::NiPoint3 offset : {RE::NiPoint3(0,0,16), RE::NiPoint3(16,0,16), RE::NiPoint3(-16,0,16),
                                      RE::NiPoint3(0,16,16), RE::NiPoint3(0,-16,16)})
        if (!ray(start + offset, destination + offset, false)) return false;
    return ray(destination + RE::NiPoint3(0,0,16), destination - RE::NiPoint3(0,0,256), true);
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
        if (r->plan["steps"].empty())
        {
            auto target = r->target.get();
            auto actor = target ? target->As<RE::Actor>() : nullptr;
            const bool lifeChanged = actor && actor->IsDead() != r->snapshot["target"].value("dead", false);
            if (r->failureSceneToken.empty() || !target || lifeChanged || target->IsDeleted() || target->IsDisabled() || !target->Is3DLoaded())
            {
                Cancel();
                return;
            }
            RE::DebugNotification("[CHIM] Interaction failed.");
        }
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
    if ((effect == "give" || effect == "store" || effect == "consume" || effect == "equip" ||
         effect == "magic" || effect == "drop" || effect == "place") && r->hasItem && !r->inventoryMoved &&
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
    if (effect == "move" || effect == "rotate" || effect == "directional_throw")
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!MovableObject(target.get()) || !player || target->GetParentCell() != player->GetParentCell())
        { Complete(r->id, r->step, "failed", "The captured object is not a movable loose reference in the current cell."); return; }
        auto position = target->GetPosition();
        if (effect == "rotate")
        {
            auto angle = target->GetAngle();
            const auto axis = step.value("axis", "");
            const float radians = value * 0.01745329252f;
            if (axis == "x") angle.x += radians; else if (axis == "y") angle.y += radians; else if (axis == "z") angle.z += radians;
            else { Complete(r->id, r->step, "failed", "Invalid rotation axis."); return; }
            target->SetAngle(angle);
            const auto actual = target->GetAngle();
            const bool changed = std::abs(std::remainder(actual.x-angle.x,6.283185307f)) < 0.02f &&
                std::abs(std::remainder(actual.y-angle.y,6.283185307f)) < 0.02f && std::abs(std::remainder(actual.z-angle.z,6.283185307f)) < 0.02f;
            Complete(r->id, r->step, changed ? "succeeded" : "unknown", changed ? "Captured reference orientation verified; collision settling is not guaranteed." : "Rotation requested once; orientation was not confirmed.");
            return;
        }
        const auto direction = step.value("direction", "");
        RE::NiPoint3 vector{};
        if (effect == "directional_throw")
        {
            vector = position - player->GetPosition();
            const auto length = vector.Length();
            if (direction == "up") vector = RE::NiPoint3(0,0,1);
            else if ((direction == "toward" || direction == "away") && length > 0.01f) vector = vector * ((direction == "toward" ? -1.0f : 1.0f) / length);
            else { Complete(r->id, r->step, "failed", "Throw direction is undefined."); return; }
            auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            auto id=r->id;auto index=r->step;auto ref=target.get();float x=vector.x,y=vector.y,z=vector.z,amount=value;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            if (!vm || !vm->DispatchStaticCall("CHIMItemInteraction", "ThrowObject", RE::MakeFunctionArguments(std::move(id),std::move(index),std::move(ref),std::move(x),std::move(y),std::move(z),std::move(amount)),callback))
                Complete(r->id,r->step,"failed","Object impulse could not be dispatched.");
            return;
        }
        const float heading = player->GetAngleZ();
        if (direction == "forward") vector=RE::NiPoint3(std::sin(heading),std::cos(heading),0);
        else if (direction == "backward") vector=RE::NiPoint3(-std::sin(heading),-std::cos(heading),0);
        else if (direction == "right") vector=RE::NiPoint3(std::cos(heading),-std::sin(heading),0);
        else if (direction == "left") vector=RE::NiPoint3(-std::cos(heading),std::sin(heading),0);
        else if (direction == "up") vector=RE::NiPoint3(0,0,1);
        else if (direction == "down") vector=RE::NiPoint3(0,0,-1);
        else { Complete(r->id,r->step,"failed","Invalid movement direction.");return; }
        const auto destination = position + vector * value;
        if (!CheckedMove(target.get(), destination)) { Complete(r->id,r->step,"failed","Path or floor checks rejected the nearby destination.");return; }
        target->SetPosition(destination);
        const bool moved=(target->GetPosition()-destination).Length()<2.0f;
        Complete(r->id,r->step,moved?"succeeded":"unknown",moved?"Captured reference position verified after bounded path and floor checks; full collision clearance is not guaranteed.":"Movement requested once; destination was not confirmed.");
        return;
    }
    if (effect == "stagger")
    {
        if (!actor || actor->IsDead()) { Complete(r->id, r->step, "failed", "A living actor is required."); return; }
        const bool accepted = actor->SetGraphVariableFloat("staggerMagnitude", value) &&
            actor->SetGraphVariableFloat("staggerDirection", 0.0f) && actor->NotifyAnimationGraph("staggerStart");
        if (!accepted) { Complete(r->id,r->step,"unknown","Animation graph did not accept stagger."); return; }
        r->pendingStagger = true;
        r->statusCheckUntil = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        return;
    }
    if (effect == "extinguish" || effect == "neutralize_poison" || effect == "release_paralysis" || effect == "dispel")
    {
        const int family = effect == "extinguish" ? 1 : (effect == "neutralize_poison" ? 0 : 2);
        RE::SpellItem *selectedSpell = nullptr;
        if (effect == "dispel")
        {
            const auto index = static_cast<std::size_t>(value);
            if (value != std::floor(value) || index >= r->dispellableSpells.size())
            { Complete(r->id, r->step, "failed", "The captured spell selection is invalid."); return; }
            selectedSpell = RE::TESForm::LookupByID<RE::SpellItem>(r->dispellableSpells[index]);
            if (!selectedSpell || selectedSpell->GetSpellType() != RE::MagicSystem::SpellType::kSpell)
            { Complete(r->id, r->step, "failed", "The captured spell is no longer eligible."); return; }
        }
        std::vector<RE::ActiveEffect *> matches;
        if (actor)
            if (auto list = actor->AsMagicTarget()->GetActiveEffectList())
                for (auto active : *list)
                    if (active && !active->flags.any(RE::ActiveEffect::Flag::kDispelled) &&
                        (selectedSpell ? active->spell == selectedSpell :
                         (active->spell == StatusSpell(family, 5) || active->spell == StatusSpell(family, 10) ||
                          active->spell == StatusSpell(family, 20) || active->spell == StatusSpell(family, 30)))) matches.push_back(active);
        if (selectedSpell)
        {
            std::set<std::pair<std::uintptr_t, std::uint16_t>> identities;
            for (auto active : matches)
            {
                if (active->duration <= active->elapsedSeconds || active->duration <= 0 ||
                    active->flags.any(RE::ActiveEffect::Flag::kInactive) || active->conditionStatus == RE::ActiveEffect::ConditionStatus::kFalse)
                { Complete(r->id,r->step,"skipped","The captured spell instance changed; nothing was dispelled."); return; }
                identities.emplace(reinterpret_cast<std::uintptr_t>(active), active->usUniqueID);
            }
            if (identities != r->dispelInstances[selectedSpell->GetFormID()])
            { Complete(r->id,r->step,"skipped","The captured spell instance changed; nothing was dispelled."); return; }
        }
        for (auto active : matches) active->Dispel(true);
        bool cleared = true;
        if (actor)
            if (auto remaining = actor->AsMagicTarget()->GetActiveEffectList())
                for (auto active : *remaining)
                    if (active && !active->flags.any(RE::ActiveEffect::Flag::kDispelled) &&
                        (selectedSpell ? active->spell == selectedSpell :
                         (active->spell == StatusSpell(family,5) || active->spell == StatusSpell(family,10) ||
                          active->spell == StatusSpell(family,20) || active->spell == StatusSpell(family,30)))) cleared = false;
        bool shaderStopped = false;
        if (effect == "extinguish" && sceneryFireEpoch == r->epoch)
        {
            const auto key = std::make_pair(target->GetFormID(), std::string("burning_visual"));
            auto found = sceneryFire.find(key);
            if (found != sceneryFire.end())
            {
                if (found->second) { found->second->finished = true; shaderStopped = true; }
                sceneryFire.erase(found);
            }
        }
        Complete(r->id, r->step, matches.empty() && !shaderStopped ? "skipped" : (cleared ? "succeeded" : "unknown"),
                 cleared ? (matches.empty() && !shaderStopped ? "The selected effect is already absent; nothing changed." : "Only the selected captured spell or CHIM-owned effects were marked dispelled/stopped.") :
                           "Dispel requested once; removal was not confirmed.");
        return;
    }
    if (effect == "cast_selected_magic")
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!r->selectedMagic || !KnownMagic(player, *r->selectedMagic) || r->magicIssued)
        {
            Complete(r->id, r->step, "failed", "Selected known magic is no longer available or was already cast.");
            return;
        }
        auto spell = RE::TESForm::LookupByID<RE::SpellItem>(r->selectedMagic->spell);
        auto recipient = spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf ? player : target.get();
        r->magicRecipient = recipient->CreateRefHandle();
        if (auto victim = recipient->As<RE::Actor>())
        {
            r->magicHealthBefore = victim->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
            if (auto effects = victim->AsMagicTarget()->GetActiveEffectList())
                for (auto active : *effects)
                    if (active && active->spell == spell) r->magicEffectsBefore.push_back(reinterpret_cast<std::uintptr_t>(active));
        }
        r->magicIssued = true;
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        auto id = r->id;
        auto index = r->step;
        auto shout = r->selectedMagic->kind == "shout" ? RE::TESForm::LookupByID<RE::TESShout>(r->selectedMagic->form) : nullptr;
        auto rank = r->selectedMagic->rank;
        if (!vm || !vm->DispatchStaticCall("CHIMItemInteraction", "CastSelectedMagic",
            RE::MakeFunctionArguments(std::move(id), std::move(index), std::move(recipient), std::move(spell), std::move(shout), std::move(rank)), callback))
            Complete(r->id, r->step, "failed", "Selected magic could not be dispatched.");
        return;
    }
    if (effect == "burning_visual" || effect == "frost_visual" || effect == "shock_visual" || effect == "impact_burst")
    {
        auto shader = SceneryFireShader(effect);
        auto lists = RE::ProcessLists::GetSingleton();
        if (actor || !shader || !lists)
        {
            Complete(r->id, r->step, "failed", "Scenery fire visuals are unavailable.");
            return;
        }
        if (sceneryFireEpoch != r->epoch)
        {
            sceneryFire.clear();
            sceneryFireEpoch = r->epoch;
        }
        for (auto it = sceneryFire.begin(); it != sceneryFire.end();)
            if (!it->second || it->second->finished || it->second->age >= it->second->lifetime)
                it = sceneryFire.erase(it);
            else
                ++it;
        const auto key = std::make_pair(target->GetFormID(), effect);
        if (sceneryFire.contains(key))
        {
            sceneryFire.at(key)->finished = true;
            sceneryFire.erase(key);
        }
        if (sceneryFire.size() >= 32)
        {
            Complete(r->id, r->step, "failed", "Too many scenery fire effects are already active.");
            return;
        }
        r->priorSceneryShaders.clear();
        lists->ForEachShaderEffect([&](RE::ShaderReferenceEffect *effect) {
            if (effect->effectData == shader && effect->target == r->target)
                r->priorSceneryShaders.emplace_back(effect);
            return r->priorSceneryShaders.size() >= 32 ? RE::BSContainer::ForEachResult::kStop
                                                      : RE::BSContainer::ForEachResult::kContinue;
        });
        if (r->priorSceneryShaders.size() >= 32)
        {
            r->priorSceneryShaders.clear();
            Complete(r->id, r->step, "failed", "Too many existing fire shader instances to verify a new application.");
            return;
        }
        // The observed AE call returned 0x1, not a usable effect pointer despite CommonLib's signature.
        // Never dereference or retain its return; only ProcessLists supplies verified engine-owned instances.
        target->ApplyEffectShader(shader, effect == "impact_burst" ? 1.0f : static_cast<float>(step.value("duration", 10)));
        r->pendingSceneryShader = shader->GetFormID();
        r->statusCheckUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        return;
    }
    const int family = StatusFamily(effect);
    if (family >= 0)
    {
        const int duration = effect == "banish" ? 5 : step.value("duration", 10);
        auto spell = StatusSpell(family, duration);
        auto player = RE::PlayerCharacter::GetSingleton();
        auto caster = player ? player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant) : nullptr;
        auto magic = actor ? actor->AsMagicTarget() : nullptr;
        if (!actor || (effect == "reanimate" ? !actor->IsDead() : actor->IsDead()) || !spell || !caster || !magic ||
            (effect == "banish" && (!actor->IsSummoned() || !ActorKeyword(actor, "ActorTypeDaedra"))) || (effect == "turn_undead" && !UndeadActor(actor)) || (effect == "reanimate" && ActorKeyword(actor, "MagicNoReanimate")))
        {
            Complete(r->id, r->step, "failed", "Timed status is unavailable for this target.");
            return;
        }
        // Refresh this CHIM family only. Do not alter vanilla or another mod's active effects.
        std::vector<RE::ActiveEffect *> refresh;
        if (auto effects = magic->GetActiveEffectList())
            for (auto active : *effects)
                if (active && (active->spell == StatusSpell(family, 5) || active->spell == StatusSpell(family, 10) ||
                               active->spell == StatusSpell(family, 20) || active->spell == StatusSpell(family, 30)) &&
                    !active->flags.any(RE::ActiveEffect::Flag::kDispelled)) refresh.push_back(active);
        for (auto active : refresh) active->Dispel(true);
        SKSE::log::info("[INTERACT] Status cast id={} step={} spell={:08X} target={:08X} magnitude={} duration={} refreshed={}",
                        r->id, r->step, spell->GetFormID(), actor->GetFormID(), value, duration, refresh.size());
        float magnitude = value;
        if (effect == "weaken_weapon" || effect == "fortify_weapon")
            magnitude *= std::max(0.0f, actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kAttackDamageMult)) / 100.0f;
        if (effect == "slow" || effect == "haste")
            magnitude *= std::max(0.0f, actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeedMult)) / 100.0f;
        if (effect == "weaken_weapon")
            magnitude = std::min(magnitude, std::max(0.0f, actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kAttackDamageMult)));
        if (magnitude <= 0) { Complete(r->id, r->step, "failed", "No positive modifier can be applied."); return; }
        caster->CastSpellImmediate(spell, false, actor, 1.0f, false, magnitude, player);
        r->pendingStatusSpell = spell->GetFormID();
        r->statusCheckUntil = std::chrono::steady_clock::now() + std::chrono::seconds(effect == "reanimate" ? 5 : 2);
        return;
    }
    const auto restoration = RestorationValue(effect);
    if (restoration != RE::ActorValue::kNone)
    {
        auto stats = actor ? actor->AsActorValueOwner() : nullptr;
        if (!actor || actor->IsDead() || !stats)
        {
            Complete(r->id, r->step, "failed", "Living actor statistics are unavailable.");
            return;
        }
        const float before = stats->GetActorValue(restoration);
        stats->RestoreActorValue(restoration, value);
        const bool restored = stats->GetActorValue(restoration) > before;
        Complete(r->id, r->step, restored ? "succeeded" : "failed",
                 restored ? "The requested actor statistic increased." : "The actor statistic did not increase.");
        return;
    }
    const auto destructible = target->GetBaseObject()->As<RE::BGSDestructibleObjectForm>();
    if (effect == "disable" || (effect == "destroy" && !actor && (!destructible || !destructible->data)))
    {
        // Use the runtime-aware reference API, avoiding Papyrus's enable-parent rejection.
        // Do not unlink the enable parent or operate on any related reference.
        target->Disable();
        const bool disabled = target->IsDisabled();
        SKSE::log::info("[INTERACT] Native disable {} target {:08X} disabled={}", r->id,
                        target->GetFormID(), disabled);
        Complete(r->id, r->step, disabled ? "succeeded" : "unknown",
                 disabled ? "Captured reference removed through native disable; no debris or destruction animation."
                          : "Native disable requested once; the captured reference disabled flag was not confirmed.");
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
    if (effect == "give" || effect == "store" || effect == "consume" || effect == "equip" || effect == "magic" ||
        effect == "drop" || effect == "place")
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
    bool approved = true; // Submitting the explicit action authorizes execution; retain the Papyrus ABI.
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
    if (!Live(r) || r->id != id || r->allowedStep.load() != step) return false;
    if (r->selectedMagic && step >= 0 && step < static_cast<int>(r->plan["steps"].size()) &&
        r->plan["steps"][step].value("effect", "") == "cast_selected_magic")
        return KnownMagic(RE::PlayerCharacter::GetSingleton(), *r->selectedMagic);
    return true;
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
            {"resize", "Resize"}, {"magic", "Cast"}, {"cast_selected_magic", "Cast selected magic"}, {"combat", "Start combat with"},
            {"consume_world", "Consume"}, {"heal", "Heal"}, {"restore_stamina", "Restore stamina of"},
            {"restore_magicka", "Restore magicka of"}, {"drop", "Drop"}, {"place", "Place"},
            {"disarm", "Disarm"}, {"unequip", "Unequip armor from"},
            {"poison", "Poison"}, {"burning", "Burn"}, {"burning_visual", "Set alight"}, {"paralysis", "Paralyze"},
            {"calm", "Calm"}, {"fear", "Frighten"}, {"frenzy", "Enrage"},
            {"frost","Freeze"},{"shock","Shock"},{"drain_stamina","Drain stamina from"},{"drain_magicka","Drain magicka from"},
            {"slow","Slow"},{"haste","Hasten"},{"weaken_armor","Weaken armor on"},{"fortify_armor","Fortify armor on"},
            {"weaken_weapon","Weaken weapon damage of"},{"fortify_weapon","Fortify weapon damage of"},
            {"absorb_health","Absorb health from"},{"absorb_stamina","Absorb stamina from"},{"absorb_magicka","Absorb magicka from"},
            {"ethereal","Make ethereal"},{"soul_trap","Soul trap"},{"reanimate","Reanimate"},{"turn_undead","Turn undead"},{"banish","Banish"},
            {"stagger","Stagger"},{"extinguish","Extinguish"},{"neutralize_poison","Neutralize poison on"},{"release_paralysis","Release paralysis on"},
            {"dispel","Dispel selected spell on"},{"directional_throw","Throw"},{"rotate","Rotate"},{"move","Move"},
            {"frost_visual","Frost visual on"},{"shock_visual","Shock visual on"},{"impact_burst","Impact burst on"}};
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
    constexpr auto parentLength = std::string_view("interact-").size() + 32;
    const auto parentId = utteranceId.starts_with("interact-") && utteranceId.size() > parentLength
                              ? utteranceId.substr(0, parentLength) : utteranceId;
    auto found = pendingReactions.find(parentId);
    if (found == pendingReactions.end())
        return;
    SKSE::log::info("[INTERACT] Narration gate {} completed={} held={}", found->second->id, completed,
                    found->second->reactionLines.size());
    if (!completed)
        pendingReactions.erase(found);
    else
    {
        const auto &r = found->second;
        if (std::find(r->narrationChunks.begin(), r->narrationChunks.end(), utteranceId) == r->narrationChunks.end()) return;
        r->narrationCompleted.insert(utteranceId);
        r->reactionExpires = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        // Tick releases only after transport completion as well as every playback acknowledgement.
    }
}

void Tick()
{
    static std::atomic<bool> queued = false;
    if (queued.exchange(true))
        return;
    SKSE::GetTaskInterface()->AddTask([] {
        queued = false;
        if (sceneryFireEpoch != PlaythroughSession::Generation())
        {
            sceneryFire.clear();
            sceneryFireEpoch = PlaythroughSession::Generation();
        }
        for (auto it = sceneryFire.begin(); it != sceneryFire.end();)
            if (!it->second || it->second->finished || it->second->age >= it->second->lifetime)
                it = sceneryFire.erase(it);
            else
                ++it;


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
        std::vector<json> narrationAcknowledgements;
        {
            std::lock_guard lock(currentMutex);
            for (auto &[id, pending] : pendingReactions)
            {
                if (!pending->narrationAcknowledged && pending->narrationStreamDone &&
                    !pending->narrationChunks.empty() && pending->narrationCompleted.size() == pending->narrationChunks.size())
                {
                    pending->narrationAcknowledged = true;
                    pending->reactionReleased = true;
                    narrationAcknowledgements.push_back({{"speaker", "The Narrator"}, {"speech", pending->narrationText},
                                                         {"utterance_id", id}, {"location", GetPlayerLocation()}});
                }
                if (pending->reactionReleased)
                {
                    ready.insert(ready.end(), pending->reactionLines.begin(), pending->reactionLines.end());
                    pending->reactionLines.clear();
                }
            }
        }
        for (const auto &ack : narrationAcknowledgements)
            HTTPManager::log(std::format("_speech|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), ack.dump()));
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
        if (r->pendingSceneryShader)
        {
            auto target = r->target.get();
            auto shader = RE::TESForm::LookupByID<RE::TESEffectShader>(r->pendingSceneryShader);
            auto lists = RE::ProcessLists::GetSingleton();
            RE::NiPointer<RE::ShaderReferenceEffect> applied;
            std::size_t candidates = 0;
            const bool available = target && !target->IsDeleted() && !target->IsDisabled() && target->Is3DLoaded();
            if (available && shader && lists)
                lists->ForEachShaderEffect([&](RE::ShaderReferenceEffect *effect) {
                    if (effect->effectData == shader && effect->target == r->target && !effect->finished &&
                        effect->lifetime > 0.0f && effect->age < effect->lifetime &&
                        std::none_of(r->priorSceneryShaders.begin(), r->priorSceneryShaders.end(),
                                     [effect](const auto &prior) { return prior.get() == effect; }))
                    {
                        ++candidates;
                        applied = RE::NiPointer<RE::ShaderReferenceEffect>(effect);
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            const bool registered = candidates == 1;
            if (registered || candidates > 1 || !available || std::chrono::steady_clock::now() >= r->statusCheckUntil)
            {
                if (registered) sceneryFire[{target->GetFormID(), r->plan["steps"][r->step].value("effect", "")}] = applied;
                SKSE::log::info("[INTERACT] Scenery shader result id={} step={} registered={} new_instances={}",
                                r->id, r->step, registered, candidates);
                r->pendingSceneryShader = 0;
                r->priorSceneryShaders.clear();
                Complete(r->id, r->step, registered ? "succeeded" : "unknown",
                         registered ? "Requested shader registered on the captured scenery. Visual effect only: no health damage, spread, destruction or rendered pixels verified."
                                    : "Visual shader requested once; a unique new registration was not confirmed.");
            }
            return;
        }
        if (r->pendingStagger)
        {
            auto target = r->target.get();
            auto actor = target ? target->As<RE::Actor>() : nullptr;
            const bool observed = actor && !actor->IsDead() && actor->IsStaggering();
            if (observed || std::chrono::steady_clock::now() >= r->statusCheckUntil)
            {
                r->pendingStagger = false;
                Complete(r->id,r->step,observed?"succeeded":"unknown",observed?"Stagger state observed after one graph request.":"One stagger request was accepted; stagger state was not observed.");
            }
            return;
        }
        if (r->pendingStatusSpell)
        {
            auto target = r->target.get();
            auto actor = target ? target->As<RE::Actor>() : nullptr;
            auto spell = RE::TESForm::LookupByID<RE::SpellItem>(r->pendingStatusSpell);
            std::string statusDetail;
            bool applied = HasAppliedStatus(actor, spell, statusDetail);
            const auto pendingEffect = r->plan["steps"][r->step].value("effect", "");
            if (pendingEffect == "reanimate")
            {
                applied = applied && actor && !actor->IsDead() && actor->GetCommandingActor().get() == RE::PlayerCharacter::GetSingleton();
                if (applied) statusDetail = "The captured corpse is reanimated and commanded by the player; the engine owns its expiry.";
            }
            if (pendingEffect == "banish")
            {
                applied = target && (target->IsDeleted() || target->IsDisabled() || (actor && actor->IsDead()));
                if (applied) statusDetail = "The previously confirmed summoned target departed after the banishment cast.";
            }
            if (applied && pendingEffect == "soul_trap") statusDetail = "Soul trap is armed on the captured target; no death or filled soul gem is claimed.";
            if (applied || std::chrono::steady_clock::now() >= r->statusCheckUntil)
            {
                std::size_t matching = 0;
                if (auto magic = actor ? actor->AsMagicTarget() : nullptr)
                    if (auto effects = magic->GetActiveEffectList())
                        for (auto active : *effects)
                            if (active && active->spell == spell)
                            {
                                ++matching;
                                if (matching <= 4)
                                    SKSE::log::info("[INTERACT] Status verification id={} step={} magnitude={} duration={} elapsed={} condition_false={} inactive={} dispelled={}",
                                        r->id, r->step, active->magnitude, active->duration, active->elapsedSeconds,
                                        active->conditionStatus == RE::ActiveEffect::ConditionStatus::kFalse,
                                        active->flags.any(RE::ActiveEffect::Flag::kInactive), active->flags.any(RE::ActiveEffect::Flag::kDispelled));
                            }
                SKSE::log::info("[INTERACT] Status result id={} step={} applied={} matching_effects={}",
                                r->id, r->step, applied, matching);
                r->pendingStatusSpell = 0;
                Complete(r->id, r->step, applied ? "succeeded" : "unknown",
                         applied ? statusDetail
                                 : "Status cast once; application was not confirmed. Resistance or eligibility may prevent it.");
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
    if (old)
        SKSE::GetTaskInterface()->AddTask([old] {
            old->pendingSceneryShader = 0;
            old->priorSceneryShaders.clear();
        });
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
// Pick the first camera-ray collision only; terrain or an unusable blocker never selects something behind it.
RE::NiPointer<RE::TESObjectREFR> SceneryTarget(RE::PlayerCharacter *player)
{
    auto cell = player ? player->GetParentCell() : nullptr;
    auto world = cell ? cell->GetbhkWorld() : nullptr;
    RE::NiPoint3 origin{}, direction{};
    if (!world || !SpatialAwareness::GetPlayerCameraGaze(origin, direction)) return {};
    RE::bhkPickData pick{};
    const float scale = RE::bhkWorld::GetWorldScale();
    pick.rayInput.from = RE::hkVector4(origin * scale);
    pick.rayInput.to = RE::hkVector4((origin + direction * 8192.0f) * scale);
    pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kCameraPick);
    if (auto controller = player->GetCharController())
    {
        RE::CFilter playerFilter{};
        controller->GetCollisionFilterInfo(playerFilter);
        pick.rayInput.filterInfo.SetSystemGroup(playerFilter.GetSystemGroup());
    }
    if (!world->PickObject(pick) || !pick.rayOutput.HasHit() || !pick.rayOutput.rootCollidable) return {};
    auto target = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
    if (!target || target == player || target->IsDisabled() || target->IsDeleted() ||
        !target->Is3DLoaded() || !target->GetBaseObject() || target->As<RE::Actor>()) return {};
    return RE::NiPointer<RE::TESObjectREFR>(target);
}

std::string InteractionTargetName(RE::TESObjectREFR *target, bool logSource = false)
{
    const auto result = [target, logSource](std::string value, const char *source) {
        if (logSource) SKSE::log::info("[INTERACT] Target label ref={:08X} source={} label={}",
                                     target->GetFormID(), source, value);
        return value;
    };
    const auto name = target->GetName();
    if (name && *name) return result(name, "display_name");
    auto base = target->GetBaseObject();
    if (!base) return result(std::format("scenery [{:08X}]", target->GetFormID()), "reference_id");
    const auto editor = base->GetFormEditorID();
    if (editor && *editor) return result(editor, "base_editor_id");
    // Static forms discard their getter's EditorID; installed Tweaks retains the actual EDID separately.
    using EditorIDGetter = const char *(__cdecl *)(std::uint32_t);
    static const auto cachedEditorID = []() -> EditorIDGetter {
        auto tweaks = GetModuleHandleW(L"po3_Tweaks.dll");
        return tweaks ? reinterpret_cast<EditorIDGetter>(GetProcAddress(tweaks, "GetFormEditorID")) : nullptr;
    }();
    if (cachedEditorID)
        if (const auto cached = cachedEditorID(base->GetFormID()); cached && *cached) return result(cached, "tweaks_editor_id");
    return result(std::format("{} [{:08X}]", SemanticType(base), base->GetFormID()), "base_form_id");
}

void Open()
{
    if (Current())
    {
        RE::DebugNotification("[CHIM] An interaction is already active.");
        return;
    }
    auto player = RE::PlayerCharacter::GetSingleton();
    auto ui = RE::UI::GetSingleton();
    if (!player || !ui || !ChimInteraction::Enabled() ||
        !PlaythroughSession::Allowed(PlaythroughSession::Generation()) || ui->GameIsPaused() ||
        PrismaUIBridge::IsAnyHotkeyPanelFocused() || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) ||
        ui->IsMenuOpen(RE::Console::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return;
    auto crosshair = RE::CrosshairPickData::GetSingleton();
    auto target = crosshair ? crosshair->GetActiveTarget().get() : RE::NiPointer<RE::TESObjectREFR>{};
    if (player && (!target || target.get() == player || !target->GetBaseObject() || target->IsDisabled() || target->IsDeleted() || !target->Is3DLoaded()))
        target = SceneryTarget(player);
    if (!target || target.get() == player || !target->GetBaseObject() ||
        target->IsDisabled() || target->IsDeleted() || !target->Is3DLoaded())
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
    auto addSpell = [&](RE::SpellItem *spell) {
        if (!spell || !spell->GetName() || !*spell->GetName() || spell->GetFormID() ==
            (RE::TESDataHandler::GetSingleton()->LookupForm<RE::SpellItem>(powerLocalId, "AIAgent.esp") ?
             RE::TESDataHandler::GetSingleton()->LookupForm<RE::SpellItem>(powerLocalId, "AIAgent.esp")->GetFormID() : 0)) return;
        MagicChoice choice{spell->GetFormID(), spell->GetFormID(), 0, "spell"};
        if (spell->GetSpellType() == RE::MagicSystem::SpellType::kPower) choice.kind = "power";
        else if (spell->GetSpellType() == RE::MagicSystem::SpellType::kLesserPower) choice.kind = "power";
        if (!KnownMagic(player, choice) || std::any_of(r->magicChoices.begin(), r->magicChoices.end(),
            [&](const auto &known) { return known.form == choice.form; })) return;
        r->magicChoices.push_back(choice);
    };
    if (auto base = player->GetActorBase())
        if (auto list = base->GetSpellList())
            for (std::uint32_t i = 0; i < list->numSpells; ++i) addSpell(list->spells[i]);
    if (auto race = player->GetRace())
        if (auto list = race->actorEffects)
            for (std::uint32_t i = 0; i < list->numSpells; ++i) addSpell(list->spells[i]);
    for (auto spell : player->GetActorRuntimeData().addedSpells) addSpell(spell);
    PrismaUIBridge::ShowItemInteraction({{"id", r->id}, {"target", InteractionTargetName(target.get(), true)}, {"items", items}, {"magic", MagicMenuChoices(r)}});
    std::vector<RE::TESShout *> shouts;
    for (auto shout : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESShout>())
        if (shout && player->HasShout(shout) && shout->GetName() && *shout->GetName()) shouts.push_back(shout);
    if (!shouts.empty())
    {
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        auto id = r->id;
        if (vm) vm->DispatchStaticCall("CHIMItemInteraction", "CollectUnlockedShouts",
            RE::MakeFunctionArguments(std::move(id), std::move(shouts)), callback);
    }
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
            if (op == "submit" || op == "cancel")
                commandType = op;
            SKSE::log::info("[INTERACT] Game thread received {} command for {}", commandType, r->id);
            if (op == "cancel")
            {
                Cancel();
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
            r->selectedMagic.reset();
            if (input.contains("magic_key") && !input.at("magic_key").is_null())
            {
                if (!input.at("magic_key").is_number_unsigned() && !input.at("magic_key").is_number_integer())
                    throw std::runtime_error("Invalid magic selection");
                const auto key = input.at("magic_key").get<std::size_t>();
                if (key >= r->magicChoices.size() || !KnownMagic(player, r->magicChoices[key]))
                    throw std::runtime_error("Selected magic is no longer known");
                r->selectedMagic = r->magicChoices[key];
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
            if (r->selectedMagic) r->allowed.push_back("cast_selected_magic");
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
            if (!actor || (base->As<RE::BGSDestructibleObjectForm>() && base->As<RE::BGSDestructibleObjectForm>()->data))
                r->allowed.push_back("destroy");
            if (!actor)
                for (const auto visual : {"burning_visual", "frost_visual", "shock_visual", "impact_burst"})
                    if (SceneryFireShader(visual)) r->allowed.push_back(visual);
            if (!actor && base->GetFormType() == RE::FormType::Misc)
                r->allowed.push_back("push");
            auto potion = item ? item->As<RE::AlchemyItem>() : nullptr;
            if (actor && potion && !potion->IsPoison())
                r->allowed.push_back("consume");
            if (actor && !actor->IsDead())
                for (const auto &effect : {"heal", "restore_stamina", "restore_magicka"})
                    r->allowed.push_back(effect);
            if (actor && !actor->IsDead())
                for (const auto &effect : {"poison", "burning", "paralysis", "calm", "fear", "frenzy", "frost", "shock", "drain_stamina", "drain_magicka", "slow", "haste", "weaken_armor", "fortify_armor", "weaken_weapon", "fortify_weapon", "absorb_health", "absorb_stamina", "absorb_magicka", "ethereal", "soul_trap", "turn_undead"})
                    if ((std::string_view(effect) != "turn_undead" || UndeadActor(actor)) &&
                        (std::string_view(effect) != "soul_trap" || (!actor->IsEssential() && !actor->IsCommandedActor() && !ActorKeyword(actor,"MagicNoSoulTrap"))) &&
                        (!std::string_view(effect).starts_with("absorb_") || !ActorKeyword(actor,"ActorTypeDwarven")) && StatusSpell(StatusFamily(effect), 10)) r->allowed.push_back(effect);
            if (actor && actor->IsDead() && !ActorKeyword(actor, "MagicNoReanimate") && StatusSpell(StatusFamily("reanimate"), 10)) r->allowed.push_back("reanimate");
            if (actor && !actor->IsDead())
            {
                for (const auto effect : {"stagger", "neutralize_poison", "release_paralysis"}) r->allowed.push_back(effect);
                if (actor->IsSummoned() && ActorKeyword(actor, "ActorTypeDaedra") && StatusSpell(StatusFamily("banish"), 5)) r->allowed.push_back("banish");
            }
            r->allowed.push_back("extinguish");
            if (MovableObject(target.get()) && target->GetParentCell() == player->GetParentCell())
                for (const auto operation : {"directional_throw", "move", "rotate"}) r->allowed.push_back(operation);
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
                        effect != "magic")
                        filtered.push_back(effect);
                }
                r->allowed = filtered;
            }
            json itemData = nullptr;
            if (item)
            {
                // The temporary entry owns only its pointer-list nodes; the captured extra-data remains game-owned.
                RE::InventoryEntryData selectedEntry(item, 1);
                if (extra) selectedEntry.AddExtraList(extra);
                const auto goldValue = selectedEntry.GetValue();
                const auto weight = selectedEntry.GetWeight();
                auto ingredient = item->As<RE::IngredientItem>();
                RE::MagicItem *authoredMagic = potion ? static_cast<RE::MagicItem *>(potion) : static_cast<RE::MagicItem *>(scroll);
                if (ingredient) authoredMagic = ingredient;
                itemData = {{"name", r->selected.name},
                            {"type", static_cast<int>(item->GetFormType())},
                            {"semantic_type", SemanticType(item)},
                            {"quantity", r->quantity},
                            {"quest_item", questItem},
                            {"gold_value_per_item_not_barter_price", goldValue >= 0 ? json(goldValue) : json(nullptr)},
                            {"weight_per_item", std::isfinite(weight) && weight >= 0 ? json(weight) : json(nullptr)},
                            {"charge", nullptr}, {"max_charge", nullptr},
                            {"effects", Effects(authoredMagic)}};
                if (item->As<RE::TESObjectWEAP>() || item->As<RE::TESObjectARMO>()) itemData["tempering_factor"] = 1.0f;
                if (ingredient) itemData["effects_scope"] = "authored ingredient properties; may be undiscovered, not currently active";
                if (auto armor = item->As<RE::TESObjectARMO>())
                {
                    std::string armorClass = "unknown armor";
                    if (armor->IsLightArmor()) armorClass = "light armor";
                    else if (armor->IsHeavyArmor()) armorClass = "heavy armor";
                    else if (armor->IsClothing()) armorClass = "clothing";
                    itemData["armor_class"] = armorClass;
                    itemData["base_armor_rating_not_final_protection"] = armor->GetArmorRating();
                }
                if (auto weapon = item->As<RE::TESObjectWEAP>())
                {
                    static const std::vector<std::string> classes = {"unarmed", "sword", "dagger", "war axe", "mace",
                        "greatsword", "battleaxe or warhammer", "bow", "staff", "crossbow"};
                    const auto kind = static_cast<std::size_t>(weapon->GetWeaponType());
                    itemData["weapon_class"] = kind < classes.size() ? classes[kind] : "unknown weapon";
                    itemData["base_damage_not_final_hit_damage"] = weapon->GetAttackDamage();
                }
                itemData["applied_poison"] = nullptr;
                RE::EnchantmentItem *enchantment = nullptr;
                if (auto enchanting = item->As<RE::TESEnchantableForm>())
                {
                    enchantment = enchanting->formEnchanting;
                    if (item->As<RE::TESObjectWEAP>() && enchantment && enchanting->amountofEnchantment > 0)
                        itemData["max_charge"] = enchanting->amountofEnchantment;
                }
                if (extra)
                {
                    if (auto e = extra->GetByType<RE::ExtraEnchantment>())
                    {
                        enchantment = e->enchantment;
                        itemData["max_charge"] = item->As<RE::TESObjectWEAP>() && enchantment && e->charge > 0 ? json(e->charge) : json(nullptr);
                    }
                    if (auto health = extra->GetByType<RE::ExtraHealth>())
                        itemData["tempering_factor"] = health->health;
                    if (auto poison = extra->GetByType<RE::ExtraPoison>())
                        itemData["applied_poison"] = {{"name", poison->poison ? poison->poison->GetName() : "unknown"},
                            {"remaining_uses", poison->count}, {"effects", Effects(poison->poison)}};
                    if (auto charge = extra->GetByType<RE::ExtraCharge>())
                        itemData["charge"] = charge->charge;
                    itemData["equipped"] =
                        extra->HasType(RE::ExtraDataType::kWorn) || extra->HasType(RE::ExtraDataType::kWornLeft);
                }
                else
                    itemData["equipped"] = false;
                // Missing ExtraCharge means an otherwise charge-bearing instance is fully charged.
                if (itemData["charge"].is_null() && !itemData["max_charge"].is_null()) itemData["charge"] = itemData["max_charge"];
                itemData["enchantment_effects"] = Effects(enchantment);
                itemData["enchantment_name"] = enchantment ? enchantment->GetName() : "none";
            }
            json targetData = {{"equipment", equipmentData}, {"name", InteractionTargetName(target.get())},
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
                targetData["level"] = actor->GetLevel();
                targetData["undead"] = UndeadActor(actor);
                targetData["summoned"] = actor->IsSummoned();
                targetData["daedra"] = ActorKeyword(actor,"ActorTypeDaedra");
                targetData["no_reanimate"] = ActorKeyword(actor,"MagicNoReanimate");
                targetData["no_soul_trap"] = ActorKeyword(actor,"MagicNoSoulTrap");
                targetData["movement_speed_percent"] = targetStats->GetActorValue(RE::ActorValue::kSpeedMult);
                targetData["weapon_damage_multiplier"] = targetStats->GetActorValue(RE::ActorValue::kAttackDamageMult);
                targetData["poison_resistance"] = targetStats->GetActorValue(RE::ActorValue::kPoisonResist);
                targetData["active_effects"] = ActiveEffectSnapshot(actor);
                r->dispellableSpells.clear();
                targetData["dispellable_spells"] = json::array();
                if (!actor->IsDead())
                    if (auto effects = actor->AsMagicTarget()->GetActiveEffectList())
                        for (auto active : *effects)
                        {
                            auto spell = active && active->spell ? active->spell->As<RE::SpellItem>() : nullptr;
                            if (!spell || spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell || active->duration <= 0 ||
                                active->duration <= active->elapsedSeconds || active->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled) ||
                                active->conditionStatus == RE::ActiveEffect::ConditionStatus::kFalse ||
                                std::find(r->dispellableSpells.begin(), r->dispellableSpells.end(), spell->GetFormID()) != r->dispellableSpells.end()) continue;
                            bool scripted = std::any_of(spell->effects.begin(), spell->effects.end(), [](auto part) {
                                return !part || !part->baseEffect || part->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kScript;
                            });
                            if (scripted) continue;
                            targetData["dispellable_spells"].push_back({{"index", r->dispellableSpells.size()}, {"name", spell->GetName()}});
                            r->dispellableSpells.push_back(spell->GetFormID());
                            if (r->dispellableSpells.size() == 8) break;
                        }
                r->dispelInstances.clear();
                if (auto effects = actor->AsMagicTarget()->GetActiveEffectList())
                    for (auto active : *effects)
                        if (active && active->spell && !active->flags.any(RE::ActiveEffect::Flag::kDispelled) &&
                            std::find(r->dispellableSpells.begin(),r->dispellableSpells.end(),active->spell->GetFormID()) != r->dispellableSpells.end())
                            r->dispelInstances[active->spell->GetFormID()].emplace(reinterpret_cast<std::uintptr_t>(active),active->usUniqueID);
                // Offer a spell only when every captured undispelled component can be removed together.
                std::set<RE::FormID> ineligibleSpells;
                if (auto effects = actor->AsMagicTarget()->GetActiveEffectList())
                    for (auto active : *effects)
                        if (active && active->spell && !active->flags.any(RE::ActiveEffect::Flag::kDispelled) &&
                            (active->duration <= 0 || active->duration <= active->elapsedSeconds ||
                             active->flags.any(RE::ActiveEffect::Flag::kInactive) || active->conditionStatus == RE::ActiveEffect::ConditionStatus::kFalse))
                            ineligibleSpells.insert(active->spell->GetFormID());
                std::erase_if(r->dispellableSpells, [&](auto id) { return ineligibleSpells.contains(id); });
                targetData["dispellable_spells"] = json::array();
                for (std::size_t index = 0; index < r->dispellableSpells.size(); ++index)
                    if (auto spell = RE::TESForm::LookupByID<RE::SpellItem>(r->dispellableSpells[index]))
                        targetData["dispellable_spells"].push_back({{"index", index}, {"name", spell->GetName()}});
                if (!r->dispellableSpells.empty()) r->allowed.push_back("dispel");
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
            targetData["semantic_type"] = actor ? "actor" : SemanticType(base);
            targetData["type"] = static_cast<int>(base->GetFormType());
            targetData["lock_level"] = static_cast<int>(target->GetLockLevel());
            auto owner = target->GetOwner();
            targetData["owner"] = owner ? owner->GetName() : "none";
            targetData["open_state"] = "unknown";
            targetData["quest_associations"] = "unknown";
            targetData["authored_destruction"] =
                std::find(r->allowed.begin(), r->allowed.end(), "destroy") != r->allowed.end();
            json skills = json::object();
            static const std::vector<std::pair<std::string, RE::ActorValue>> skillValues = {
                {"One-handed", RE::ActorValue::kOneHanded}, {"Two-handed", RE::ActorValue::kTwoHanded},
                {"Archery", RE::ActorValue::kArchery}, {"Block", RE::ActorValue::kBlock},
                {"Smithing", RE::ActorValue::kSmithing}, {"Heavy armor", RE::ActorValue::kHeavyArmor},
                {"Light armor", RE::ActorValue::kLightArmor}, {"Pickpocket", RE::ActorValue::kPickpocket},
                {"Lockpicking", RE::ActorValue::kLockpicking}, {"Sneak", RE::ActorValue::kSneak},
                {"Alchemy", RE::ActorValue::kAlchemy}, {"Speech", RE::ActorValue::kSpeech},
                {"Alteration", RE::ActorValue::kAlteration}, {"Conjuration", RE::ActorValue::kConjuration},
                {"Destruction", RE::ActorValue::kDestruction}, {"Illusion", RE::ActorValue::kIllusion},
                {"Restoration", RE::ActorValue::kRestoration}, {"Enchanting", RE::ActorValue::kEnchanting}};
            for (const auto &[name, value] : skillValues) skills[name] = playerStats->GetActorValue(value);
            r->snapshot = {
                {"item", itemData},
                {"target", targetData},
                {"player",
                 {{"level", player->GetLevel()}, {"skills", skills},
                  {"armor_rating", playerStats->GetActorValue(RE::ActorValue::kDamageResist)},
                  {"active_effects", ActiveEffectSnapshot(player)},
                  {"max_health", player->GetActorValueMax(RE::ActorValue::kHealth)},
                  {"max_stamina", player->GetActorValueMax(RE::ActorValue::kStamina)},
                  {"max_magicka", player->GetActorValueMax(RE::ActorValue::kMagicka)},
                  {"health", playerStats->GetActorValue(RE::ActorValue::kHealth)},
                  {"stamina", playerStats->GetActorValue(RE::ActorValue::kStamina)},
                  {"magicka", playerStats->GetActorValue(RE::ActorValue::kMagicka)},
                  {"combat", player->IsInCombat()},
                  {"sneaking", player->IsSneaking()}}},
                {"location", player->GetCurrentLocation() ? player->GetCurrentLocation()->GetName() : "unknown"}};
            r->snapshot["selected_magic"] = r->selectedMagic ? MagicSnapshot(*r->selectedMagic) : json(nullptr);
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
                        r->failureSceneToken = result.value("failure_scene_token", "");
                        const bool failureScene = r->failureSceneToken.size() == 32 &&
                            r->failureSceneToken.find_first_not_of("0123456789abcdef") == std::string::npos;
                        if (!r->plan.at("steps").is_array() || (r->plan["steps"].empty() && !failureScene) || r->plan["steps"].size() > 5)
                            throw std::runtime_error("Invalid sequence");
                        int inventorySteps = 0;
                        int pickupSteps = 0;
                        int selectedMagicSteps = 0;
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
                                {"magic", {0, 0}}, {"cast_selected_magic", {0, 0}},
                                {"combat", {0, 0}}, {"heal", {1, 100}},
                                {"restore_stamina", {1, 100}}, {"restore_magicka", {1, 100}}, {"poison", {1, 10}}, {"burning", {1, 10}}, {"burning_visual", {1, 1}},
                                {"paralysis", {1, 1}}, {"calm", {1, 100}}, {"fear", {1, 100}}, {"frenzy", {1, 100}},
                                {"frost", {1,10}}, {"shock", {1,10}}, {"drain_stamina", {1,10}}, {"drain_magicka", {1,10}},
                                {"slow", {1,50}}, {"haste", {1,50}}, {"weaken_armor", {1,100}}, {"fortify_armor", {1,100}},
                                {"weaken_weapon", {1,50}}, {"fortify_weapon", {1,50}}, {"absorb_health", {1,10}},
                                {"absorb_stamina", {1,10}}, {"absorb_magicka", {1,10}}, {"ethereal", {1,1}}, {"soul_trap", {1,1}},
                                {"reanimate", {1,100}}, {"turn_undead", {1,100}}, {"banish", {1,100}}, {"stagger", {0,1}},
                                {"extinguish", {0,0}}, {"neutralize_poison", {0,0}}, {"release_paralysis", {0,0}}, {"dispel", {0,7}},
                                {"directional_throw", {1,100}}, {"rotate", {-180,180}}, {"move", {1,256}},
                                {"frost_visual", {1,1}}, {"shock_visual", {1,1}}, {"impact_burst", {1,1}},
                                {"drop", {1, 100}}, {"place", {1, 100}}, {"disarm", {0, 1}}, {"unequip", {30, 61}}};
                            if ((effect == "pickup" || effect == "consume_world") && ++pickupSteps > 1)
                                throw std::runtime_error("Repeated pickup");
                            if (effect == "cast_selected_magic" && ++selectedMagicSteps > 1)
                                throw std::runtime_error("Selected magic can be cast only once");
                            const auto bounds = limits.at(effect);
                            if (!std::isfinite(value) || value < bounds.first || value > bounds.second ||
                                !s.at("alive").is_boolean())
                                throw std::runtime_error("Invalid value");
                            if ((effect == "give" || effect == "store" || effect == "consume" || effect == "equip" ||
                                 effect == "lock" || effect == "drop" || effect == "place" || effect == "disarm" || effect == "unequip") &&
                                std::floor(value) != value)
                                throw std::runtime_error("Fractional quantity");
                            if ((effect == "combat" || effect == "disarm" || effect == "unequip" || RestorationValue(effect) != RE::ActorValue::kNone || (StatusFamily(effect) >= 0 && effect != "reanimate") || effect == "stagger" || effect == "dispel" || effect == "neutralize_poison" || effect == "release_paralysis") && !s.at("alive").get<bool>())
                                throw std::runtime_error("Combat requires living target");
                            if ((effect == "burning_visual" || effect == "frost_visual" || effect == "shock_visual" || effect == "impact_burst" || effect == "move" || effect == "rotate" || effect == "directional_throw" || effect == "reanimate") && s.at("alive").get<bool>())
                                throw std::runtime_error("Scenery fire requires a non-actor target");
                            if (s.contains("duration"))
                            {
                                if (!s.at("duration").is_number_integer())
                                    throw std::runtime_error("Invalid status duration");
                                const int duration = s.at("duration").get<int>();
                                bool validDuration = duration == 0;
                                if (effect == "burning_visual" || effect == "frost_visual" || effect == "shock_visual")
                                    validDuration = duration == 5 || duration == 10 || duration == 20 || duration == 30;
                                else if (StatusFamily(effect) >= 0 && effect != "banish")
                                    validDuration = StatusSpell(StatusFamily(effect), duration) != nullptr;
                                if (!validDuration) throw std::runtime_error("Unsupported status duration");
                            }
                            const auto direction = s.value("direction", "");
                            const auto axis = s.value("axis", "");
                            if (effect == "directional_throw")
                            {
                                if (direction != "toward" && direction != "away" && direction != "up") throw std::runtime_error("Invalid throw direction");
                            }
                            else if (effect == "move")
                            {
                                if (direction != "forward" && direction != "backward" && direction != "left" && direction != "right" && direction != "up" && direction != "down") throw std::runtime_error("Invalid move direction");
                            }
                            else if (!direction.empty()) throw std::runtime_error("Unexpected direction");
                            if (effect == "rotate")
                            { if (axis != "x" && axis != "y" && axis != "z") throw std::runtime_error("Invalid rotation axis"); }
                            else if (!axis.empty()) throw std::runtime_error("Unexpected rotation axis");
                            if (effect == "dispel" && (value != std::floor(value) || value >= r->dispellableSpells.size())) throw std::runtime_error("Invalid captured dispel selection");
                            for (auto dep : s.at("requires"))
                                if (!dep.is_number_integer() || dep.get<int>() < 0 || dep.get<std::size_t>() >= index)
                                    throw std::runtime_error("Invalid dependency");
                            if (effect == "give" || effect == "store" || effect == "consume" || effect == "equip" ||
                                effect == "magic" || effect == "drop" || effect == "place")
                                ++inventorySteps;
                        }
                        if (inventorySteps > 1)
                            throw std::runtime_error("Conflicting inventory steps");
                        PrismaUIBridge::HideItemInteraction();
                        RunStep(r);
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
                        r->failureSceneToken.clear();
                        PrismaUIBridge::UpdateItemInteraction(
                            {{"id", r->id},
                             {"preserveDraft", true},
                             {"state", "error"},
                             {"confirm", false},
                             {"status", result.value("code", "") == "no_action"
                                            ? "No suitable action could be resolved. Nothing happened; your description is kept."
                                            : "CHIM could not resolve this interaction. No effects were played. Your "
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
// Finish the Papyrus assault/damage operation on the game thread; never repeat its damage.
void FinishInjury(const std::string &id, int step, float healthBefore)
{
    SKSE::GetTaskInterface()->AddTask([id, step, healthBefore] {
        if (!CanExecute(id, step))
            return;
        auto r = Current();
        if (r->finalizedInjuryStep == step || r->plan["steps"][step].value("effect", "") != "injure")
            return;
        r->finalizedInjuryStep = step;
        auto target = r->target.get();
        auto actor = target ? target->As<RE::Actor>() : nullptr;
        if (!actor || actor->IsDeleted() || actor->IsDisabled() || !actor->Is3DLoaded())
        {
            Complete(id, step, "unknown", "Assault alarm and damage issued; target became unavailable.");
            return;
        }
        auto stats = actor->AsActorValueOwner();
        const bool damaged = stats && stats->GetActorValue(RE::ActorValue::kHealth) < healthBefore;
        std::string detail = damaged ? "Health decreased; assault alarm sent. "
                                     : "Assault alarm sent; health decrease was not confirmed. ";
        if (actor->IsDead())
            detail += "Target died; corpse stagger was skipped.";
        else
        {
            const bool magnitude = actor->SetGraphVariableFloat("staggerMagnitude", 0.5f);
            const bool direction = actor->SetGraphVariableFloat("staggerDirection", 0.0f);
            const bool accepted = magnitude && direction && actor->NotifyAnimationGraph("staggerStart");
            if (actor->IsStaggering())
                detail += "Stagger state observed.";
            else if (accepted)
                detail += "Stagger request accepted; animation playback was not confirmed.";
            else
                detail += "Stagger request was not accepted by the animation graph.";
        }
        Complete(id, step, damaged ? "succeeded" : "unknown", detail);
    });
}
// Papyrus supplies the official unlocked-word check; callbacks only append to this live menu's stable keys.
bool CanPrepareMagic(const std::string &id)
{
    auto r = Current();
    return Live(r) && r->id == id && !r->submitted;
}
void AddUnlockedShout(const std::string &id, RE::TESShout *shout, int rank)
{
    SKSE::GetTaskInterface()->AddTask([id, shout, rank] {
        if (!CanPrepareMagic(id) || !shout || rank < 1 || rank > 3) return;
        auto r = Current();
        auto spell = shout->variations[rank - 1].spell;
        MagicChoice choice{shout->GetFormID(), spell ? spell->GetFormID() : 0, rank, "shout"};
        if (KnownMagic(RE::PlayerCharacter::GetSingleton(), choice)) r->magicChoices.push_back(choice);
    });
}
void RefreshMagicChoices(const std::string &id)
{
    SKSE::GetTaskInterface()->AddTask([id] {
        if (!CanPrepareMagic(id)) return;
        auto r = Current();
        if (std::any_of(r->magicChoices.begin(), r->magicChoices.end(), [](const auto &choice) { return choice.kind == "shout"; }))
            PrismaUIBridge::UpdateItemInteraction({{"id", id}, {"preserveDraft", true}, {"magic", MagicMenuChoices(r)}});
    });
}
// A cast command is not proof of its authored consequences; observe actual recipient state once.
void FinishSelectedMagic(const std::string &id, int step)
{
    SKSE::GetTaskInterface()->AddTask([id, step] {
        if (!CanExecute(id, step)) return;
        auto r = Current();
        if (!r->selectedMagic || r->plan["steps"][step].value("effect", "") != "cast_selected_magic") return;
        auto recipient = r->magicRecipient.get();
        auto spell = RE::TESForm::LookupByID<RE::SpellItem>(r->selectedMagic->spell);
        bool observed = false;
        if (recipient && spell && !recipient->IsDeleted() && !recipient->IsDisabled())
        {
            if (auto actor = recipient->As<RE::Actor>())
            {
                const auto health = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
                for (const auto effect : spell->effects)
                    if (effect && effect->baseEffect && effect->baseEffect->data.primaryAV == RE::ActorValue::kHealth &&
                        effect->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kValueModifier)
                    {
                        const bool hostile = effect->baseEffect->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHostile);
                        observed = observed || (hostile ? health < r->magicHealthBefore : health > r->magicHealthBefore);
                    }
                if (auto effects = actor->AsMagicTarget()->GetActiveEffectList())
                    for (auto active : *effects)
                        if (active && active->spell == spell && active->conditionStatus != RE::ActiveEffect::ConditionStatus::kFalse &&
                            !active->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled) &&
                            std::find(r->magicEffectsBefore.begin(), r->magicEffectsBefore.end(), reinterpret_cast<std::uintptr_t>(active)) == r->magicEffectsBefore.end())
                            observed = true;
            }
        }
        Complete(id, step, observed ? "succeeded" : "unknown",
                 observed ? "Selected authored magic cast once; recipient health changed in the authored direction or a new exact spell effect appeared. Other authored consequences are not verified."
                          : "Selected authored magic cast once; its physical result could not be confirmed. No resources, cooldown, equipment or learned magic were changed.");
    });
}

void Register(RE::BSScript::IVirtualMachine *vm)
{
    vm->RegisterFunction("CanPrepareMagic", "CHIMItemInteraction",
        +[](RE::StaticFunctionTag *, std::string id) { return CanPrepareMagic(id); }, false);
    vm->RegisterFunction("AddUnlockedShout", "CHIMItemInteraction",
        +[](RE::StaticFunctionTag *, std::string id, RE::TESShout *shout, int rank) { AddUnlockedShout(id, shout, rank); }, false);
    vm->RegisterFunction("RefreshMagicChoices", "CHIMItemInteraction",
        +[](RE::StaticFunctionTag *, std::string id) { RefreshMagicChoices(id); }, false);
    vm->RegisterFunction("FinishSelectedMagic", "CHIMItemInteraction",
        +[](RE::StaticFunctionTag *, std::string id, int step) { FinishSelectedMagic(id, step); }, false);
    vm->RegisterFunction(
        "FinishInjury", "CHIMItemInteraction",
        +[](RE::StaticFunctionTag *, std::string id, int step, float healthBefore) {
            FinishInjury(id, step, healthBefore);
        }, false);
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
