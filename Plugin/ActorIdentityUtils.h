#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ActorIdentityUtils
{
    // Registration field: the reference's origin, independent of its current load-order prefix.
    inline std::string BuildReferenceSource(std::string_view pluginName, std::uint32_t refId, bool lightPlugin = false)
    {
        if (pluginName.empty() || refId == 0 || refId >= 0xFF000000 ||
            pluginName.find_first_of("/\\|@#\r\n") != std::string_view::npos) {
            return {};
        }
        const auto localId = lightPlugin ? refId & 0xFFF : refId & 0xFFFFFF;
        return std::format("{}/{:08X}", pluginName, localId);
    }

    // Profile ownership follows the placed reference, not its name or current load-order slot.
    inline std::string BuildProfileKey(std::string source, std::uint32_t refId)
    {
        const auto separator = source.find('/');
        if (separator != std::string::npos) {
            std::transform(source.begin(), source.begin() + separator, source.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            source[separator] = '|';
            return "ref:" + source;
        }
        return refId ? std::format("runtime:{:08X}", refId) : std::string{};
    }

    inline constexpr std::string_view PlayerActorKey = "player";
    inline constexpr std::string_view NarratorActorKey = "narrator";

    // Lowercase 8-4-4-4-12 hex and not the nil UUID, as HerikaServer's dyn: keys require.
    inline bool IsDynamicUuid(std::string_view uuid)
    {
        if (uuid.size() != 36) {
            return false;
        }
        bool nonZero = false;
        for (std::size_t index = 0; index < uuid.size(); ++index) {
            const char c = uuid[index];
            if (index == 8 || index == 13 || index == 18 || index == 23) {
                if (c != '-') {
                    return false;
                }
                continue;
            }
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
            nonZero = nonZero || c != '0';
        }
        return nonZero;
    }

    // Random bytes become a version 4 UUID, so the result is never nil.
    inline std::string FormatDynamicUuid(std::array<std::uint8_t, 16> bytes)
    {
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);
        std::string uuid;
        uuid.reserve(36);
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) {
                uuid += '-';
            }
            uuid += std::format("{:02x}", bytes[index]);
        }
        return uuid;
    }

    inline bool IsActorKey(std::string_view key);

    // Physical actor key sent to the server. Placed references keep the same ref: form as
    // BuildProfileKey; dynamic (FF) actors need their persisted UUID. Names never become keys.
    inline std::string BuildActorKey(std::string_view source, std::string_view dynamicUuid = {})
    {
        if (source.find('/') != std::string_view::npos) {
            // A plugin name the server grammar rejects (for example a .esx file) leaves the actor unresolved.
            auto key = BuildProfileKey(std::string(source), 0);
            return IsActorKey(key) ? key : std::string{};
        }
        return IsDynamicUuid(dynamicUuid) ? "dyn:" + std::string(dynamicUuid) : std::string{};
    }

    // Exactly chimIsActorKey(): player, narrator, dyn:<uuid>, or ref:<plugin>|00XXXXXX where the plugin is
    // lowercase, ends in .esm/.esp/.esl, has no | / \ @ # : control or DEL byte and does not start with a space.
    inline bool IsActorKey(std::string_view key)
    {
        if (key == PlayerActorKey || key == NarratorActorKey) {
            return true;
        }
        if (key.starts_with("dyn:")) {
            return IsDynamicUuid(key.substr(4));
        }
        if (!key.starts_with("ref:") || key.size() < 4 + 5 + 9) {
            return false;
        }
        const auto plugin = key.substr(4, key.size() - 4 - 9);
        const auto local = key.substr(key.size() - 8);
        if (key[key.size() - 9] != '|' || plugin.size() < 5 || plugin.front() == ' ') {
            return false;
        }
        const auto extension = plugin.substr(plugin.size() - 4);
        if (extension != ".esm" && extension != ".esp" && extension != ".esl") {
            return false;
        }
        const bool pluginValid = std::none_of(plugin.begin(), plugin.end(), [](unsigned char c) {
            return c < 0x20 || c == 0x7F || (c >= 'A' && c <= 'Z') || c == '|' || c == '/' || c == '\\' ||
                   c == '@' || c == '#' || c == ':';
        });
        return pluginValid && local.starts_with("00") && std::all_of(local.begin(), local.end(), [](char c) {
                   return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
               });
    }

    // A dynamic actor is remembered by its runtime reference plus the base it was created from,
    // so a recycled FF FormID carrying a different base never inherits the old identity.
    struct DynamicIdentityEntry {
        std::uint32_t formId = 0;
        std::uint32_t baseId = 0;
        std::string uuid;
    };

    inline constexpr std::size_t DynamicIdentityEntrySize = 4 + 4 + 36;

    inline bool IsDynamicFormId(std::uint32_t formId) { return formId >= 0xFF000000; }

    // Whether a saved dynamic identity survives a save snapshot. A non-resident reference keeps its UUID;
    // only positive evidence (the FormID now holds a deleted reference or another base) drops it.
    inline bool RetainDynamicIdentity(bool resident, bool deleted, std::uint32_t currentBaseId,
                                      std::uint32_t savedBaseId)
    {
        return !resident || (!deleted && currentBaseId == savedBaseId);
    }

    inline std::string SerializeDynamicIdentities(const std::vector<DynamicIdentityEntry>& entries)
    {
        std::string data;
        data.reserve(entries.size() * DynamicIdentityEntrySize);
        for (const auto& entry : entries) {
            if (!IsDynamicFormId(entry.formId) || !IsDynamicUuid(entry.uuid)) {
                continue;
            }
            for (const auto value : {entry.formId, entry.baseId}) {
                for (int shift = 0; shift < 32; shift += 8) {
                    data += static_cast<char>((value >> shift) & 0xFF);
                }
            }
            data += entry.uuid;
        }
        return data;
    }

    // Rejects the whole record on any malformed or duplicate entry rather than guessing.
    inline std::optional<std::vector<DynamicIdentityEntry>> ParseDynamicIdentities(std::string_view data)
    {
        if (data.size() % DynamicIdentityEntrySize != 0) {
            return std::nullopt;
        }
        std::vector<DynamicIdentityEntry> entries;
        for (std::size_t offset = 0; offset < data.size(); offset += DynamicIdentityEntrySize) {
            auto readU32 = [&](std::size_t at) {
                std::uint32_t value = 0;
                for (int index = 0; index < 4; ++index) {
                    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[at + index])) << (index * 8);
                }
                return value;
            };
            DynamicIdentityEntry entry{readU32(offset), readU32(offset + 4),
                                       std::string(data.substr(offset + 8, 36))};
            if (!IsDynamicFormId(entry.formId) || !IsDynamicUuid(entry.uuid) ||
                std::any_of(entries.begin(), entries.end(), [&](const auto& other) {
                    return other.formId == entry.formId || other.uuid == entry.uuid;
                })) {
                return std::nullopt;
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    inline std::string BuildPromptIdentifier(std::string_view displayName, std::uint32_t refId)
    {
        if (displayName.empty() || refId == 0) {
            return std::string(displayName);
        }
        return std::format("{} [RefID: {:08X}]", displayName, refId);
    }

    // A physical actor bound on the game thread when an action was first queued.
    struct BoundActor {
        std::uint32_t formId = 0;
        std::string actorKey;  // Canonical key at binding; empty only for a static reference without a source.
    };

    // What dispatch observes now. Every field is read without computing a new identity: the handle captured at
    // binding, the reference its FormID currently names, and the key the identity registry currently holds.
    struct BoundActorObservation {
        bool handleLive = false;          // The bound handle still resolves (a destroyed reference releases it).
        std::uint32_t handleFormId = 0;   // FormID of the reference behind the bound handle.
        bool sameReference = false;       // That FormID still names the reference behind the handle.
        bool deleted = false;
        std::string currentActorKey;      // Registry key for a dynamic FormID; ignored for static references.
    };

    // A cached profile key alone is not proof: a deleted FF actor's agent entry can outlive it while its FormID
    // is recycled for a new actor with a new dyn: identity. The bound handle and key must both still match.
    inline bool StillSameBoundActor(const BoundActor& bound, const BoundActorObservation& now)
    {
        if (bound.formId == 0 || !now.handleLive || now.deleted || !now.sameReference ||
            now.handleFormId != bound.formId) {
            return false;
        }
        if (IsDynamicFormId(bound.formId)) {
            return !bound.actorKey.empty() && now.currentActorKey == bound.actorKey;
        }
        return true;
    }

    // A speaker captured when its line (or rechat) was dispatched, rechecked after a wait such as TTS or a
    // deferred retry. Immutable values only: the agent entry's own fields may change while it is reused.
    enum class CapturedSpeakerKind { None, Narrator, Player, Physical };
    struct CapturedSpeakerId {
        CapturedSpeakerKind kind = CapturedSpeakerKind::None;
        std::uint32_t formId = 0;    // Physical only.
        std::string profileKey;      // Physical only: the agent's profile key at capture.
        std::uint64_t loadEpoch = 0;
    };
    struct CapturedSpeakerObservation {
        std::uint64_t loadEpoch = 0;
        bool agentFound = false;     // Narrator: the typed narrator agent; physical: the agent at the FormID.
        bool agentIsNarrator = false;
        std::uint32_t agentFormId = 0;
        std::string agentProfileKey;
        bool boundActorStill = false;  // The handle bound at capture still holds the same reference and key.
    };

    inline bool StillSameCapturedSpeaker(const CapturedSpeakerId& captured, const CapturedSpeakerObservation& now)
    {
        if (captured.kind == CapturedSpeakerKind::None || captured.loadEpoch != now.loadEpoch) return false;
        if (captured.kind == CapturedSpeakerKind::Player) return true;
        if (!now.agentFound) return false;
        if (captured.kind == CapturedSpeakerKind::Narrator) return now.agentIsNarrator;
        return !now.agentIsNarrator && captured.formId != 0 && now.agentFormId == captured.formId &&
               now.agentProfileKey == captured.profileKey && now.boundActorStill;
    }

    // Internal rechat bookkeeping key (chain, in-flight, last rechatter, retry, completion): the captured
    // speaker's canonical role plus its load generation, never the decorated label a recycled FF slot or a
    // same-name reference can share. Empty when a physical speaker has no bound canonical key: such a speaker
    // never begins a rechat. Never sent on the wire.
    inline std::string RechatBookkeepingKey(const CapturedSpeakerId& captured, std::string_view actorKey)
    {
        std::string role;
        switch (captured.kind) {
            case CapturedSpeakerKind::Narrator: role = NarratorActorKey; break;
            case CapturedSpeakerKind::Player: role = PlayerActorKey; break;
            case CapturedSpeakerKind::Physical:
                if (actorKey.empty() || actorKey == NarratorActorKey || actorKey == PlayerActorKey) return {};
                role = actorKey;
                break;
            default: return {};
        }
        return role + "#" + std::to_string(captured.loadEpoch);
    }
    // gamedata.php actor rows (paired contract version 1). Each row carries the identity of the actor it
    // was read from: the typed player is "player"/00000014; any other actor needs its canonical physical key.
    inline constexpr int GameDataActorIdentityVersion = 1;
    inline constexpr std::uint32_t PlayerRefId = 0x14;

    struct GameDataActorIdentity {
        std::string actorKey;              // Empty when the actor has no canonical key at capture.
        std::uint32_t refId = 0;           // Runtime reference FormID at capture.
        std::uint64_t loadGeneration = 0;

        bool Keyed() const { return refId != 0 && !actorKey.empty(); }
        // An unkeyed actor is held back, never sent by name: a name-only row could update a namesake. An FF actor
        // waits for the game thread to register it; a non-FF reference that has no canonical key is never sent.
        bool Deferred() const { return !Keyed(); }
        bool Unkeyable() const { return !Keyed() && !IsDynamicFormId(refId); }
    };

    // The key must match the reference kind it was captured from; the player key belongs to the player only.
    inline GameDataActorIdentity CaptureGameDataActor(bool player, std::uint32_t refId, std::string actorKey,
                                                      std::uint64_t loadGeneration)
    {
        if (player) {
            return {std::string(PlayerActorKey), PlayerRefId, loadGeneration};
        }
        const bool valid = refId != 0 && refId != PlayerRefId && IsActorKey(actorKey) &&
                           actorKey != PlayerActorKey && actorKey != NarratorActorKey &&
                           actorKey.starts_with("dyn:") == IsDynamicFormId(refId);
        return {valid ? std::move(actorKey) : std::string{}, refId, loadGeneration};
    }

    // Field values for actor_identity_version / actor_key / actor_refid; nothing for an unkeyed actor.
    inline std::optional<std::pair<std::string, std::string>> GameDataActorFields(const GameDataActorIdentity& identity)
    {
        if (!identity.Keyed()) return std::nullopt;
        return std::pair{identity.actorKey, std::format("{:08X}", identity.refId)};
    }

    // Adds actor_identity_version/actor_key/actor_refid to one gamedata row; false (row unchanged) when unkeyed.
    template <class Json>
    bool ApplyGameDataActorIdentity(Json& row, const GameDataActorIdentity& identity)
    {
        const auto fields = GameDataActorFields(identity);
        if (!fields) return false;
        row["actor_identity_version"] = GameDataActorIdentityVersion;
        row["actor_key"] = fields->first;
        row["actor_refid"] = fields->second;
        return true;
    }

    // activity_status attack target, paired with the row's actor_identity_version 1. attack_target_key and
    // attack_target_refid are always present: both strings for a keyed combat target (the typed player is
    // "player"/00000014), both null when there is no target or it cannot be keyed. attack_target stays display
    // only and is written only beside a key, so an unkeyed target never reaches the server by name.
    template <class Json>
    void ApplyGameDataAttackTarget(Json& row, const std::optional<GameDataActorIdentity>& target,
                                   const std::string& displayName)
    {
        const auto fields = target ? GameDataActorFields(*target) : std::nullopt;
        if (!fields) {
            row["attack_target_key"] = nullptr;
            row["attack_target_refid"] = nullptr;
            return;
        }
        row["attack_target_key"] = fields->first;
        row["attack_target_refid"] = fields->second;
        if (!displayName.empty()) row["attack_target"] = displayName;
    }

    // Change hashes are cached per FormID. Prefixing the captured identity means a new actor in a recycled FF slot
    // or a new load never matches the previous entry, and a late acknowledgement only confirms its own actor.
    inline std::string GameDataChangeHash(const GameDataActorIdentity& identity, std::string_view hash)
    {
        return std::format("{}|{:08X}|{}#", identity.actorKey, identity.refId, identity.loadGeneration) +
               std::string(hash);
    }
}
