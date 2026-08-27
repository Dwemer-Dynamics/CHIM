#include "AutoActivateRules.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

namespace AutoActivateRules {
    namespace {
        enum class Decision : std::uint8_t { Include, Exclude };

        enum class TriState : std::uint8_t { Any, Yes, No };

        struct Rule {
            std::string id;
            std::string name;
            bool enabled{true};
            Decision decision{Decision::Exclude};
            int priority{50};
            std::string npcName;
            std::string baseActor;
            std::string race;
            std::string faction;
            std::string sourceMod;
            std::string normalizedNpcName;
            std::string normalizedBaseActor;
            std::string normalizedRace;
            std::string normalizedFaction;
            std::string normalizedSourceMod;
            TriState canTalk{TriState::Any};
            TriState unique{TriState::Any};
            TriState guard{TriState::Any};
            TriState follower{TriState::Any};
            TriState hostile{TriState::Any};
            TriState creature{TriState::Any};
        };

        struct ActorFacts {
            std::string npcName;
            std::string baseEditorId;
            std::string baseStableId;
            std::string raceName;
            std::string raceEditorId;
            std::string raceStableId;
            std::string sourceMod;
            std::vector<std::string> factions;
            bool canTalk{false};
            bool unique{false};
            bool guard{false};
            bool follower{false};
            bool hostile{false};
            bool creature{false};
        };

        struct RuleNeeds {
            bool enabledRules{false};
            bool npcName{false};
            bool baseActor{false};
            bool race{false};
            bool faction{false};
            bool sourceMod{false};
            bool canTalk{false};
            bool unique{false};
            bool guard{false};
            bool follower{false};
            bool hostile{false};
            bool creature{false};
        };

        std::shared_mutex g_rulesMutex;
        std::vector<Rule> g_rules;
        RuleNeeds g_ruleNeeds;
        std::atomic<std::uint64_t> g_revision{0};

        std::string Trim(std::string value) {
            const auto first =
                std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
            const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
                                  return std::isspace(ch) != 0;
                              }).base();
            if (first >= last) {
                return {};
            }
            return std::string(first, last);
        }

        std::string Normalize(std::string value) {
            value = Trim(std::move(value));
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        std::string FormEditorId(RE::TESForm* form) {
            if (!form) {
                return {};
            }
            const char* editorId = form->GetFormEditorID();
            return editorId ? editorId : "";
        }

        std::string FormName(RE::TESForm* form) {
            if (!form) {
                return {};
            }
            const char* name = form->GetName();
            return name ? name : "";
        }

        std::string StableFormId(RE::TESForm* form) {
            if (!form) {
                return {};
            }
            auto* file = form->GetFile(0);
            if (!file || file->GetFilename().empty()) {
                return {};
            }
            return std::format("{}/{:08X}", file->GetFilename(), static_cast<std::uint32_t>(form->GetLocalFormID()));
        }

        std::string SourceMod(RE::TESForm* form) {
            if (!form) {
                return {};
            }
            auto* file = form->GetFile(0);
            return file ? std::string(file->GetFilename()) : "";
        }

        bool MatchesExact(const std::string& selector, const std::string& value) {
            return selector.empty() || selector == value;
        }

        bool MatchesAnyExact(const std::string& selector, const std::vector<std::string>& values) {
            if (selector.empty()) {
                return true;
            }
            return std::ranges::any_of(values, [&selector](const std::string& value) { return selector == value; });
        }

        bool MatchesTriState(TriState expected, bool value) {
            return expected == TriState::Any || (expected == TriState::Yes) == value;
        }

        bool HasCondition(const Rule& rule) {
            return !rule.npcName.empty() || !rule.baseActor.empty() || !rule.race.empty() || !rule.faction.empty() ||
                   !rule.sourceMod.empty() || rule.canTalk != TriState::Any || rule.unique != TriState::Any ||
                   rule.guard != TriState::Any || rule.follower != TriState::Any || rule.hostile != TriState::Any ||
                   rule.creature != TriState::Any;
        }

        bool MatchesRule(const Rule& rule, const ActorFacts& facts) {
            return MatchesExact(rule.normalizedNpcName, facts.npcName) &&
                   (rule.normalizedBaseActor.empty() || rule.normalizedBaseActor == facts.baseEditorId ||
                    rule.normalizedBaseActor == facts.baseStableId) &&
                   (rule.normalizedRace.empty() || rule.normalizedRace == facts.raceName ||
                    rule.normalizedRace == facts.raceEditorId || rule.normalizedRace == facts.raceStableId) &&
                   MatchesAnyExact(rule.normalizedFaction, facts.factions) &&
                   MatchesExact(rule.normalizedSourceMod, facts.sourceMod) &&
                   MatchesTriState(rule.canTalk, facts.canTalk) && MatchesTriState(rule.unique, facts.unique) &&
                   MatchesTriState(rule.guard, facts.guard) && MatchesTriState(rule.follower, facts.follower) &&
                   MatchesTriState(rule.hostile, facts.hostile) && MatchesTriState(rule.creature, facts.creature);
        }

        ActorFacts BuildActorFacts(RE::Actor* actor, RE::Actor* player, const RuleNeeds& needs) {
            ActorFacts facts;
            if (!actor) {
                return facts;
            }

            if (needs.npcName) {
                const char* name = actor->GetDisplayFullName();
                if (name) {
                    facts.npcName = Normalize(name);
                }
            }

            const bool needsBase = needs.baseActor || needs.sourceMod || needs.unique || needs.faction;
            auto* baseActor = needsBase ? actor->GetActorBase() : nullptr;
            const bool needsRace = needs.race || needs.creature;
            auto* race = needsRace ? actor->GetRace() : nullptr;
            if (needs.baseActor) {
                facts.baseEditorId = Normalize(FormEditorId(baseActor));
                facts.baseStableId = Normalize(StableFormId(baseActor));
            }
            if (needs.race) {
                facts.raceName = Normalize(FormName(race));
                facts.raceEditorId = Normalize(FormEditorId(race));
                facts.raceStableId = Normalize(StableFormId(race));
            }
            if (needs.sourceMod) {
                facts.sourceMod = Normalize(SourceMod(baseActor ? static_cast<RE::TESForm*>(baseActor) : actor));
            }
            if (needs.canTalk) {
                facts.canTalk = actor->CanTalkToPlayer();
            }
            if (needs.unique) {
                facts.unique = baseActor && baseActor->IsUnique();
            }
            if (needs.guard) {
                facts.guard = actor->IsGuard();
            }
            if (needs.follower) {
                facts.follower = actor->IsPlayerTeammate();
            }
            if (needs.hostile) {
                facts.hostile = player && actor->IsHostileToActor(player);
            }
            if (needs.creature) {
                facts.creature = race && race->HasKeywordString("ActorTypeCreature");
            }

            if (needs.faction && baseActor) {
                facts.factions.reserve(baseActor->factions.size() * 3);
                for (const auto& factionInfo : baseActor->factions) {
                    auto* faction = factionInfo.faction;
                    if (!faction) {
                        continue;
                    }
                    for (std::string value : {FormEditorId(faction), FormName(faction), StableFormId(faction)}) {
                        value = Normalize(std::move(value));
                        if (!value.empty()) {
                            facts.factions.push_back(std::move(value));
                        }
                    }
                }
            }
            return facts;
        }

        RuleNeeds BuildRuleNeeds(const std::vector<Rule>& rules) {
            RuleNeeds needs;
            for (const auto& rule : rules) {
                if (!rule.enabled) {
                    continue;
                }
                needs.enabledRules = true;
                needs.npcName = needs.npcName || !rule.npcName.empty();
                needs.baseActor = needs.baseActor || !rule.baseActor.empty();
                needs.race = needs.race || !rule.race.empty();
                needs.faction = needs.faction || !rule.faction.empty();
                needs.sourceMod = needs.sourceMod || !rule.sourceMod.empty();
                needs.canTalk = needs.canTalk || rule.canTalk != TriState::Any;
                needs.unique = needs.unique || rule.unique != TriState::Any;
                needs.guard = needs.guard || rule.guard != TriState::Any;
                needs.follower = needs.follower || rule.follower != TriState::Any;
                needs.hostile = needs.hostile || rule.hostile != TriState::Any;
                needs.creature = needs.creature || rule.creature != TriState::Any;
            }
            return needs;
        }

        bool ReadBoundedString(const nlohmann::json& value, const char* key, std::string& output, std::string& error) {
            const auto it = value.find(key);
            if (it == value.end() || it->is_null()) {
                output.clear();
                return true;
            }
            if (!it->is_string()) {
                error = std::string("Rule field '") + key + "' must be text.";
                return false;
            }
            output = Trim(it->get<std::string>());
            if (output.size() > kMaxTextLength) {
                error = std::string("Rule field '") + key + "' is longer than 64 characters.";
                return false;
            }
            return true;
        }

        bool ReadTriState(const nlohmann::json& value, const char* key, TriState& output, std::string& error) {
            std::string raw;
            if (!ReadBoundedString(value, key, raw, error)) {
                return false;
            }
            raw = Normalize(std::move(raw));
            if (raw.empty() || raw == "any") {
                output = TriState::Any;
                return true;
            }
            if (raw == "yes") {
                output = TriState::Yes;
                return true;
            }
            if (raw == "no") {
                output = TriState::No;
                return true;
            }
            error = std::string("Rule field '") + key + "' must be Any, Yes, or No.";
            return false;
        }

        bool ParseRule(const nlohmann::json& value, std::size_t index, Rule& rule, std::string& error) {
            if (!value.is_object()) {
                error = std::format("Rule {} must be an object.", index + 1);
                return false;
            }
            if (!ReadBoundedString(value, "id", rule.id, error) || rule.id.empty()) {
                if (error.empty()) {
                    error = std::format("Rule {} is missing an ID.", index + 1);
                }
                return false;
            }
            if (!ReadBoundedString(value, "name", rule.name, error)) {
                return false;
            }

            const auto enabled = value.find("enabled");
            if (enabled != value.end()) {
                if (!enabled->is_boolean()) {
                    error = std::format("Rule {} has an invalid enabled value.", index + 1);
                    return false;
                }
                rule.enabled = enabled->get<bool>();
            }

            std::string decision;
            if (!ReadBoundedString(value, "decision", decision, error)) {
                return false;
            }
            decision = Normalize(std::move(decision));
            if (decision == "include") {
                rule.decision = Decision::Include;
            } else if (decision.empty() || decision == "exclude") {
                rule.decision = Decision::Exclude;
            } else {
                error = std::format("Rule {} must use Include or Exclude.", index + 1);
                return false;
            }

            const auto priority = value.find("priority");
            if (priority != value.end()) {
                if (!priority->is_number_integer()) {
                    error = std::format("Rule {} has an invalid priority.", index + 1);
                    return false;
                }
                rule.priority = priority->get<int>();
            }
            if (rule.priority < 0 || rule.priority > 100) {
                error = std::format("Rule {} priority must be from 0 to 100.", index + 1);
                return false;
            }

            if (!ReadBoundedString(value, "npc_name", rule.npcName, error) ||
                !ReadBoundedString(value, "base_actor", rule.baseActor, error) ||
                !ReadBoundedString(value, "race", rule.race, error) ||
                !ReadBoundedString(value, "faction", rule.faction, error) ||
                !ReadBoundedString(value, "source_mod", rule.sourceMod, error) ||
                !ReadTriState(value, "can_talk", rule.canTalk, error) ||
                !ReadTriState(value, "unique", rule.unique, error) ||
                !ReadTriState(value, "guard", rule.guard, error) ||
                !ReadTriState(value, "follower", rule.follower, error) ||
                !ReadTriState(value, "hostile", rule.hostile, error) ||
                !ReadTriState(value, "creature", rule.creature, error)) {
                return false;
            }

            if (!HasCondition(rule)) {
                error = std::format("Rule {} needs at least one condition.", index + 1);
                return false;
            }
            rule.normalizedNpcName = Normalize(rule.npcName);
            rule.normalizedBaseActor = Normalize(rule.baseActor);
            rule.normalizedRace = Normalize(rule.race);
            rule.normalizedFaction = Normalize(rule.faction);
            rule.normalizedSourceMod = Normalize(rule.sourceMod);
            return true;
        }

        const char* TriStateName(TriState value) {
            switch (value) {
                case TriState::Yes:
                    return "yes";
                case TriState::No:
                    return "no";
                default:
                    return "any";
            }
        }

        nlohmann::json RuleToJson(const Rule& rule) {
            return {{"id", rule.id},
                    {"name", rule.name},
                    {"enabled", rule.enabled},
                    {"decision", rule.decision == Decision::Include ? "include" : "exclude"},
                    {"priority", rule.priority},
                    {"npc_name", rule.npcName},
                    {"base_actor", rule.baseActor},
                    {"race", rule.race},
                    {"faction", rule.faction},
                    {"source_mod", rule.sourceMod},
                    {"can_talk", TriStateName(rule.canTalk)},
                    {"unique", TriStateName(rule.unique)},
                    {"guard", TriStateName(rule.guard)},
                    {"follower", TriStateName(rule.follower)},
                    {"hostile", TriStateName(rule.hostile)},
                    {"creature", TriStateName(rule.creature)}};
        }

        nlohmann::json CurrentTargetToJson() {
            auto* crosshair = RE::CrosshairPickData::GetSingleton();
            auto target = crosshair && crosshair->target ? crosshair->target.get() : nullptr;
            auto* actor = target ? target->As<RE::Actor>() : nullptr;
            if (!actor || actor->IsPlayer()) {
                return nullptr;
            }

            auto* baseActor = actor->GetActorBase();
            auto* race = actor->GetRace();
            nlohmann::json factions = nlohmann::json::array();
            if (baseActor) {
                for (const auto& factionInfo : baseActor->factions) {
                    auto* faction = factionInfo.faction;
                    if (!faction) {
                        continue;
                    }
                    factions.push_back({{"name", FormName(faction)},
                                        {"editor_id", FormEditorId(faction)},
                                        {"stable_id", StableFormId(faction)}});
                }
            }

            return {
                {"name", actor->GetDisplayFullName() ? actor->GetDisplayFullName() : ""},
                {"base_actor", !FormEditorId(baseActor).empty() ? FormEditorId(baseActor) : StableFormId(baseActor)},
                {"base_actor_stable_id", StableFormId(baseActor)},
                {"race", !FormEditorId(race).empty() ? FormEditorId(race) : FormName(race)},
                {"race_stable_id", StableFormId(race)},
                {"source_mod", SourceMod(baseActor ? static_cast<RE::TESForm*>(baseActor) : actor)},
                {"factions", std::move(factions)}};
        }
    }

    bool ShouldAutoActivate(RE::Actor* actor, RE::Actor* player, bool legacyEligible) {
        std::shared_lock lock(g_rulesMutex);
        if (!g_ruleNeeds.enabledRules) {
            return legacyEligible;
        }
        const ActorFacts facts = BuildActorFacts(actor, player, g_ruleNeeds);
        for (const auto& rule : g_rules) {
            if (rule.enabled && MatchesRule(rule, facts)) {
                return rule.decision == Decision::Include;
            }
        }
        return legacyEligible;
    }

    bool ReplaceFromJsonText(std::string_view raw, std::string& error) {
        if (raw.size() > kMaxSerializedBytes) {
            error = "Auto Activate rules are too large.";
            return false;
        }

        nlohmann::json payload;
        try {
            payload = nlohmann::json::parse(raw);
        } catch (const std::exception&) {
            error = "Auto Activate rules contain invalid JSON.";
            return false;
        }

        const nlohmann::json* values = &payload;
        if (payload.is_object()) {
            const auto rules = payload.find("rules");
            if (rules == payload.end()) {
                error = "Auto Activate rules payload is missing its rules list.";
                return false;
            }
            values = &*rules;
        }
        if (!values->is_array()) {
            error = "Auto Activate rules must be a list.";
            return false;
        }
        if (values->size() > kMaxRules) {
            error = "Auto Activate rules are limited to 32 entries.";
            return false;
        }

        std::vector<Rule> parsed;
        parsed.reserve(values->size());
        std::unordered_set<std::string> ids;
        for (std::size_t index = 0; index < values->size(); ++index) {
            Rule rule;
            if (!ParseRule((*values)[index], index, rule, error)) {
                return false;
            }
            if (!ids.insert(rule.id).second) {
                error = std::format("Rule {} has a duplicate ID.", index + 1);
                return false;
            }
            parsed.push_back(std::move(rule));
        }

        std::stable_sort(parsed.begin(), parsed.end(), [](const Rule& left, const Rule& right) {
            if (left.priority != right.priority) {
                return left.priority > right.priority;
            }
            if (left.decision != right.decision) {
                return left.decision == Decision::Exclude;
            }
            return left.name < right.name;
        });

        const RuleNeeds needs = BuildRuleNeeds(parsed);
        {
            std::unique_lock lock(g_rulesMutex);
            g_rules = std::move(parsed);
            g_ruleNeeds = needs;
        }
        g_revision.fetch_add(1, std::memory_order_release);
        return true;
    }

    std::string Serialize() {
        nlohmann::json rules = nlohmann::json::array();
        std::shared_lock lock(g_rulesMutex);
        for (const auto& rule : g_rules) {
            rules.push_back(RuleToJson(rule));
        }
        return rules.dump();
    }

    nlohmann::json BuildPrismaSnapshot() {
        nlohmann::json rules = nlohmann::json::array();
        {
            std::shared_lock lock(g_rulesMutex);
            for (const auto& rule : g_rules) {
                rules.push_back(RuleToJson(rule));
            }
        }
        return {{"revision", g_revision.load(std::memory_order_acquire)},
                {"rules", std::move(rules)},
                {"target", CurrentTargetToJson()}};
    }

    void Clear() {
        {
            std::unique_lock lock(g_rulesMutex);
            g_rules.clear();
            g_ruleNeeds = {};
        }
        g_revision.fetch_add(1, std::memory_order_release);
    }
}
