#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ActorTargetIdentifierUtils
{
    struct ParsedTarget
    {
        std::string fallbackName;
        std::uint32_t refId = 0;
        bool hasRefId = false;
        // A "RefID:" marker was present. Without hasRefId the reference was malformed and names nobody.
        bool hasRefIdMarker = false;
    };

    inline std::string Trim(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\n\r\f\v");
        if (first == std::string::npos) {
            return "";
        }

        const auto last = value.find_last_not_of(" \t\n\r\f\v");
        return value.substr(first, last - first + 1);
    }

    // Reads "[RefID: <hex>]" (optional 0x, 1-8 digits, inner spaces) starting at `open`; returns the index past ']'.
    inline std::size_t ReadDecorator(const std::string& raw, std::size_t open, std::uint32_t& refId)
    {
        const auto npos = std::string::npos;
        if (open >= raw.size() || raw[open] != '[') return npos;
        auto at = open + 1;
        const auto skipSpace = [&]() {
            while (at < raw.size() && std::isspace(static_cast<unsigned char>(raw[at]))) ++at;
        };
        skipSpace();
        static constexpr std::string_view marker = "refid:";
        if (raw.size() - at < marker.size()) return npos;
        for (std::size_t i = 0; i < marker.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(raw[at + i])) != marker[i]) return npos;
        }
        at += marker.size();
        skipSpace();
        if (at + 1 < raw.size() && raw[at] == '0' && (raw[at + 1] == 'x' || raw[at + 1] == 'X')) at += 2;
        const auto digits = at;
        while (at < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[at]))) ++at;
        if (at == digits || at - digits > 8) return npos;
        refId = static_cast<std::uint32_t>(std::stoul(raw.substr(digits, at - digits), nullptr, 16));
        skipSpace();
        if (at >= raw.size() || raw[at] != ']') return npos;
        return at + 1;
    }

    // Accepted grammar (case-insensitive marker, surrounding whitespace ignored):
    //   "<label> [RefID: <hex>]"   the generated decorator, terminal;
    //   "[RefID: <hex>] <label>"   leading form, label must not carry another marker;
    //   "[RefID: <hex>]" / "RefID: <hex>"   legacy bare reference with no label.
    // Any other text containing "RefID:" (trailing text, embedded or unbracketed markers, bad digits) is a
    // malformed explicit reference: hasRefIdMarker without hasRefId, and its display text never selects an actor.
    inline ParsedTarget Parse(std::string_view input)
    {
        ParsedTarget result{};
        const std::string raw = Trim(std::string(input));
        result.fallbackName = raw;

        std::string lower = raw;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (lower.find("refid:") == std::string::npos) {
            return result;
        }
        result.hasRefIdMarker = true;

        const auto accept = [&](std::uint32_t refId, std::string label) {
            result.refId = refId;
            result.hasRefId = refId != 0;
            result.fallbackName = Trim(std::move(label));
            return result;
        };

        std::uint32_t refId = 0;
        // Terminal decorator: the last '[' whose decorator ends the string.
        if (!raw.empty() && raw.back() == ']') {
            const auto open = raw.rfind('[');
            if (open != std::string::npos && ReadDecorator(raw, open, refId) == raw.size()) {
                return accept(refId, raw.substr(0, open));
            }
        }
        // Leading decorator.
        if (const auto end = ReadDecorator(raw, 0, refId); end != std::string::npos) {
            const auto label = raw.substr(end);
            if (lower.find("refid:", end) == std::string::npos && label.find_first_of("[]") == std::string::npos) {
                return accept(refId, label);
            }
            return result;
        }
        // Legacy bare "RefID: <hex>" with nothing else.
        if (lower.starts_with("refid:")) {
            const auto wrapped = "[" + raw + "]";
            if (ReadDecorator(wrapped, 0, refId) == wrapped.size()) return accept(refId, "");
        }
        return result;
    }

    // A decorated identifier names exactly its RefID, whatever its label says; a bare one names its label; a
    // malformed reference names nobody.
    inline bool IdentifiesActor(std::string_view identifier, std::string_view actorName, std::uint32_t refId)
    {
        const auto parsed = Parse(identifier);
        if (parsed.hasRefIdMarker) return parsed.hasRefId && parsed.refId == refId;
        return Trim(std::string(identifier)) == actorName;
    }

    // A malformed reference ("[RefID: zz]") is still explicit: it never falls back to its label.
    inline bool IsUnresolvableExplicit(const ParsedTarget& target) { return target.hasRefIdMarker && !target.hasRefId; }

    // Listener/speaker string sent for an agent. The typed narrator stays plain by name: its engine object is the
    // player, whose RefID never selects it. Every physical agent, a namesake called "The Narrator" included,
    // keeps its decorated identifier.
    inline std::string AgentSelector(bool agentIsNarrator, std::string actorName, std::string actorIdentifier)
    {
        return agentIsNarrator ? std::move(actorName) : std::move(actorIdentifier);
    }

    // The string whose md5 selects a request's profile: the profile key of the physical agent at the exact RefID,
    // otherwise the selector itself (the typed narrator's plain name, the player, older non-reference callers).
    struct ProfileAgentView
    {
        bool found = false;
        bool isNarrator = false;
        std::string profileKey;
    };
    inline std::string ProfileHashSource(std::string_view selector, const ProfileAgentView& agentAtRefId)
    {
        if (Parse(selector).hasRefId && agentAtRefId.found && !agentAtRefId.isNarrator &&
            !agentAtRefId.profileKey.empty()) {
            return agentAtRefId.profileKey;
        }
        return std::string(selector);
    }

    struct NameCandidate
    {
        std::uint32_t refId = 0;
        std::string label;
        bool eligible = false;  // Passes the caller's visibility rule; ambiguity counts every candidate.
    };

    inline std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    // A bare name selects one actor only when one distinct actor carries it: exact labels first, then labels
    // containing the name. Several distinct matches are ambiguous (never the nearest); a sole match that fails
    // the visibility rule selects nobody. Returns the index into `candidates`, or -1.
    inline int SelectUniqueNameMatch(const std::vector<NameCandidate>& candidates, std::string_view name,
                                     bool* ambiguous = nullptr)
    {
        if (ambiguous) *ambiguous = false;
        const auto wanted = Lower(Trim(std::string(name)));
        if (wanted.empty()) return -1;
        for (const bool exactTier : {true, false}) {
            int selected = -1;
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                const auto label = Lower(Trim(candidates[index].label));
                const bool matches = exactTier ? label == wanted : label.find(wanted) != std::string::npos;
                if (!matches) continue;
                if (selected >= 0 && candidates[static_cast<std::size_t>(selected)].refId != candidates[index].refId) {
                    if (ambiguous) *ambiguous = true;
                    return -1;
                }
                if (selected < 0 || candidates[index].eligible) selected = static_cast<int>(index);
            }
            if (selected >= 0) {
                return candidates[static_cast<std::size_t>(selected)].eligible ? selected : -1;
            }
        }
        return -1;
    }

    // Role-command arguments that name an existing physical actor, so they are bound when the command is queued.
    enum class RoleActorArgKind
    {
        AgentName,      // Resolved with getAgentByName by its branch.
        RoleTarget,     // Resolved with the narrator role target resolver by its branch.
        TrainerName,    // An agent, else a unique loaded NPC near the player.
        DecimalRef,     // A decimal reference that is bound only when it is a loaded actor (never a base form).
        HexRef,         // A hexadecimal reference, likewise bound only when it is a loaded actor.
    };

    struct RoleActorArg
    {
        std::size_t index = 0;
        RoleActorArgKind kind = RoleActorArgKind::AgentName;
    };

    // Mirrors parseRoleCommand's branch order and `contains` matching, so a command binds what its branch uses.
    inline std::vector<RoleActorArg> RoleCommandActorArgs(std::string_view command)
    {
        using K = RoleActorArgKind;
        const auto has = [command](std::string_view part) { return command.find(part) != std::string_view::npos; };
        if (command == "DirectorScene" || command == "DirectorSceneFailed") return {};
        // spawnCharacter's NPC, outfit, weapon and source are base forms; only its place can be an existing actor.
        if (has("spawnCharacter")) return {{4, K::DecimalRef}};
        if (has("spawnItem") || has("spawnBook")) return {{2, K::DecimalRef}};
        if (has("generateLetter")) return {};
        if (has("moveToPlayer") || has("stayAtPlace") || has("TravelTo")) return {{0, K::AgentName}};
        if (has("TeleportNPCRaw") || has("TeleportNPC") || has("KillTargetRaw")) return {{0, K::RoleTarget}};
        if (has("SpawnNPCRaw")) return {};
        if (has("SpawnItemRaw") || has("SpawnGoldRaw")) return {{0, K::RoleTarget}};
        if (has("CombatPlayer") || has("Instruction") || has("Suggestion") || has("Disposition") ||
            has("Despawn")) {
            return {{0, K::AgentName}};
        }
        if (has("EndQuest") || has("StartQuest") || has("UpdateQuest")) return {};
        if (has("Sandbox")) return {{0, K::AgentName}};
        if (has("ImpersonatePlayer") || has("QuestNotifySound")) return {};
        if (command == "UploadBookContentByTitle" || command == "UploadBookContent") return {};
        if (has("RawDebugNotification") || has("DebugNotification") || has("InternalSetting")) return {};
        if (has("QuestTrackReference")) return {{0, K::HexRef}};
        if (has("RefreshNPCVoice")) return {};
        if (has("RenameNPC") || has("BackgroundCmd")) return {{0, K::HexRef}};
        if (has("ScriptProxy")) return {};
        if (has("ShowTrainingMenu")) return {{0, K::TrainerName}};
        return {};
    }

    // Ordinary (agent "command" queue) arguments that name an actor. Every listed handler resolves its target with
    // findActorInCell or resolveActionActorTarget, which take a decorated "Name [RefID: X]" exactly; items, spells,
    // locations, amounts and crime types are never listed.
    enum class CommandActorArgKind
    {
        Name,              // The whole '@' argument is the actor label.
        JsonTarget,        // The argument is a JSON object whose "target" string is the actor label.
        JsonTargetOrName,  // JSON with "target" when the argument contains '{', else the whole argument (legacy).
    };

    struct CommandActorArg
    {
        std::size_t index = 0;
        CommandActorArgKind kind = CommandActorArgKind::Name;
    };

    // Mirrors parseCommand's branch order and `contains` matching (Commands.cpp).
    inline std::vector<CommandActorArg> OrdinaryCommandActorArgs(std::string_view command)
    {
        using K = CommandActorArgKind;
        const auto has = [command](std::string_view part) { return command.find(part) != std::string_view::npos; };
        if (has("Halt")) return {};
        if (has("AddBounty") || has("PayBounty") || has("ArrestPlayer") || has("ForgiveCrime")) return {};
        if (has("Attack") || has("Brawl")) return {{0, K::Name}};          // StartAttack/StartBrawl
        if (has("OpenInventory") || has("SetCurrentTask")) return {};
        if (has("MoveTo")) return {{0, K::Name}};                           // actor first, else a location
        if (has("TravelTo") || has("CheckInventory") || has("WalkSpeed") || has("ReadQuestJournal") ||
            has("SearchMemory") || has("InspectSurroundings") || has("LookAround")) {
            return {};
        }
        if (has("Inspect")) return {{0, K::Name}};
        if (has("TakeASeat") || has("GoToSleep") || has("WaitHere") || has("Surrender") || has("UseSoulGaze")) {
            return {};
        }
        // {"target":..,"item":..}, or legacy "spell@target" where the spell is argument 0.
        if (has("CastSpell")) return {{0, K::JsonTarget}, {1, K::Name}};
        if (has("TakeGoldFromPlayer") || has("RentRoom") || has("HireCarriage") || has("HireFerry") ||
            has("FollowPlayer") || has("MakeFollower")) {
            return {};
        }
        if (has("Follow")) return {{0, K::Name}};
        if (has("ComeCloser") || has("EndConversation") || has("ReturnBackHome")) return {};
        if (has("GiveGoldTo")) return {{0, K::JsonTargetOrName}};
        if (has("TradeItems")) return {{0, K::Name}};
        if (has("Consume")) return {};
        if (has("GiveItemTo")) return {{0, K::JsonTargetOrName}};
        return {};
    }

    // CastSpell's non-actor target literals: the caster itself or a location, never an actor lookup.
    inline bool IsCastSpellNonActorTarget(std::string_view command, std::string_view label)
    {
        if (command.find("CastSpell") == std::string_view::npos) return false;
        const auto lowered = Lower(Trim(std::string(label)));
        return lowered == "self" || lowered == "target location";
    }
}
