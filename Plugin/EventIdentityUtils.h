#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ActorIdentityUtils.h"
#include "ActorTargetIdentifierUtils.h"
#include "json.hpp"

// Event-time audience metadata carried in request field 4 (base64 JSON, the same object as the player
// routing snapshot). HerikaServer's chimDecodeEventIdentityField() validates it: a present role key must
// be canonical, so unknown roles are omitted rather than sent empty.
namespace EventIdentityUtils
{
    inline constexpr int IdentityVersion = 2;
    inline constexpr std::size_t MaxParticipantNameLength = 256;

    struct Participant {
        std::string name;
        std::string id;  // Empty when the actor has no physical key; never derived from the name.
    };

    // Mirrors chimIsEventParticipantName(): valid UTF-8, not blank, at most 256 characters, no control bytes.
    inline bool IsParticipantName(std::string_view name)
    {
        if (name.find_first_not_of(' ') == std::string_view::npos) {
            return false;
        }
        std::size_t characters = 0;
        for (std::size_t index = 0; index < name.size(); ++characters) {
            const auto lead = static_cast<unsigned char>(name[index]);
            if (lead < 0x20 || lead == 0x7F) {
                return false;
            }
            const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3
                                                     : (lead >> 3) == 0x1E ? 4 : 0;
            if (length == 0 || index + length > name.size() || (length == 2 && lead < 0xC2) ||
                (length == 4 && lead > 0xF4)) {
                return false;
            }
            for (std::size_t next = 1; next < length; ++next) {
                if ((static_cast<unsigned char>(name[index + next]) & 0xC0) != 0x80) {
                    return false;
                }
            }
            if (length == 3) {
                const auto second = static_cast<unsigned char>(name[index + 1]);
                if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second > 0x9F)) return false;
            } else if (length == 4) {
                const auto second = static_cast<unsigned char>(name[index + 1]);
                if ((lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second > 0x8F)) return false;
            }
            index += length;
        }
        return characters <= MaxParticipantNameLength;
    }

    // Built on the game thread from actor pointers, then copied unchanged into the request queue.
    // An uncaptured identity is a legacy event. A captured one is sent as version 2 even when its audience is
    // empty, so the server never substitutes the latest nearby list. An invalid one is never sent partially.
    struct EventIdentity {
        std::vector<Participant> participants;
        std::string speakerKey;
        std::vector<std::string> listenerKeys;
        std::string targetKey;
        bool captured = false;
        std::string invalidReason;

        static EventIdentity Capture()
        {
            EventIdentity identity;
            identity.captured = true;
            return identity;
        }

        bool Valid() const { return invalidReason.empty(); }
        bool Sendable() const { return captured && Valid(); }

        // Audience membership is only granted here: speaker, listener and target roles never add one.
        // A key that was computed but is not canonical invalidates the capture instead of becoming unresolved.
        bool AddParticipant(std::string name, std::string id)
        {
            captured = true;
            const auto first = name.find_first_not_of(' ');
            name = first == std::string::npos ? std::string{} : name.substr(first, name.find_last_not_of(' ') - first + 1);
            if (!id.empty() && !ActorIdentityUtils::IsActorKey(id)) {
                invalidReason = "participant_id_invalid";
                return false;
            }
            if (!IsParticipantName(name)) {
                invalidReason = "participant_name_invalid";
                return false;
            }
            if (!id.empty()) {
                if (std::any_of(participants.begin(), participants.end(),
                                [&](const Participant& p) { return p.id == id; })) {
                    return true;
                }
            } else if (std::any_of(participants.begin(), participants.end(),
                                   [&](const Participant& p) { return p.id.empty() && p.name == name; })) {
                // Unkeyed namesakes are indistinguishable on the wire; the server keeps one entry as well.
                return true;
            }
            participants.push_back({std::move(name), std::move(id)});
            return true;
        }

        void SetSpeaker(std::string key) { SetRole(speakerKey, std::move(key), "speaker_key"); }

        void AddListener(const std::string& key)
        {
            if (key.empty()) return;
            if (!ActorIdentityUtils::IsActorKey(key)) {
                invalidReason = "role_key_invalid:listener_keys";
            } else if (std::find(listenerKeys.begin(), listenerKeys.end(), key) == listenerKeys.end()) {
                listenerKeys.push_back(key);
            }
        }

        void SetTarget(std::string key) { SetRole(targetKey, std::move(key), "target_key"); }

    private:
        // An actor without a key simply has no role key; a non-canonical computed key is an error.
        void SetRole(std::string& field, std::string key, const char* name)
        {
            if (key.empty()) {
                field.clear();
            } else if (ActorIdentityUtils::IsActorKey(key)) {
                field = std::move(key);
            } else {
                invalidReason = std::string("role_key_invalid:") + name;
            }
        }
    };

    // World events whose audience is the hearing scope at the moment they happen. Control, registration,
    // configuration and context-feed requests are not listed and stay in the legacy format.
    inline bool IsObservableEventType(std::string_view type)
    {
        static constexpr std::string_view types[] = {
            "backgroundaction", "book", "chat", "combatbark", "combatend", "combatendmighty", "death",
            "funcret", "goodmorning", "goodnight", "infoaction", "infoloc", "infonpc", "itemfound", "itemtransfer",
            "location", "lockpicked", "npc_reanimated", "npcspellcast", "playerdied", "vision", "waitstart",
            "waitstop"};
        return std::find(std::begin(types), std::end(types), type) != std::end(types);
    }

    // Single-data-field events owned by the actor they are streamed for (HerikaServer logs rechat and attributes
    // the memory reply to that actor), so their audience is that speaker's hearing scope rather than the player's.
    inline bool IsSpeakerScopedEventType(std::string_view type)
    {
        return type == "rechat" || type == "memory";
    }

    inline std::string_view RequestType(std::string_view request)
    {
        return request.substr(0, request.find('|'));
    }

    // Adds identity fields to an existing field-4 object without touching its routing fields.
    inline void Append(nlohmann::json& object, const EventIdentity& identity)
    {
        if (!identity.Sendable()) {
            return;
        }
        nlohmann::json participants = nlohmann::json::array();
        for (const auto& participant : identity.participants) {
            nlohmann::json entry = {{"name", participant.name}};
            if (!participant.id.empty()) entry["id"] = participant.id;
            participants.push_back(std::move(entry));
        }
        object["identity_version"] = IdentityVersion;
        object["participants"] = std::move(participants);
        if (!identity.speakerKey.empty()) object["speaker_key"] = identity.speakerKey;
        if (!identity.listenerKeys.empty()) object["listener_keys"] = identity.listenerKeys;
        if (!identity.targetKey.empty()) object["target_key"] = identity.targetKey;
    }

    // Field-4 JSON for event paths that carry no routing snapshot; empty when nothing sendable was captured.
    inline std::string Serialize(const EventIdentity& identity)
    {
        if (!identity.Sendable()) {
            return {};
        }
        nlohmann::json object = nlohmann::json::object();
        Append(object, identity);
        return object.dump();
    }

    inline std::string Base64(std::string_view bytes)
    {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        for (std::size_t index = 0; index < bytes.size(); index += 3) {
            unsigned value = static_cast<unsigned char>(bytes[index]) << 16;
            if (index + 1 < bytes.size()) value |= static_cast<unsigned char>(bytes[index + 1]) << 8;
            if (index + 2 < bytes.size()) value |= static_cast<unsigned char>(bytes[index + 2]);
            out += alphabet[(value >> 18) & 63];
            out += alphabet[(value >> 12) & 63];
            out += index + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=';
            out += index + 2 < bytes.size() ? alphabet[value & 63] : '=';
        }
        return out;
    }

    // HerikaServer splits the request on every '|', so data must not contain one before field 4 is added.
    // JSON data keeps its exact meaning through the | escape ('|' only occurs inside JSON strings);
    // plain text follows the client's existing convention of replacing '|' with '/'.
    inline std::string EscapeDataField(std::string data)
    {
        if (data.find('|') == std::string::npos) {
            return data;
        }
        const auto first = data.find_first_not_of(" \t\r\n");
        const bool json = first != std::string::npos && (data[first] == '{' || data[first] == '[') &&
                          nlohmann::json::accept(data);
        std::string escaped;
        escaped.reserve(data.size() + 8);
        for (const char c : data) {
            if (c != '|') {
                escaped += c;
            } else if (json) {
                escaped += "\\u007c";
            } else {
                escaped += '/';
            }
        }
        return escaped;
    }

    // Adds field 4 (base64 of a JSON object) to a single-data-field request (type|ts|gamets|data).
    // Everything after the third '|' is the data field and is escaped, so the result has exactly five fields.
    // Returns false, leaving the request unchanged, when it lacks the three prefix fields.
    inline bool AppendField4(std::string& request, std::string_view objectJson)
    {
        std::size_t separator = 0;
        for (int field = 0; field < 3; ++field) {
            separator = request.find('|', field == 0 ? 0 : separator + 1);
            if (separator == std::string::npos) {
                return false;
            }
        }
        request = request.substr(0, separator + 1) + EscapeDataField(request.substr(separator + 1)) + "|" +
                  Base64(objectJson);
        return true;
    }

    // Adds field 5 (base64 of a JSON object) to a request whose field 4 is already owned by its own data, as
    // bored's seed actor is (bored|ts|gamets|location|seed). Existing fields are never escaped or rewritten, so
    // the request must have exactly five fields; anything else returns false and leaves it unchanged.
    inline bool AppendField5(std::string& request, std::string_view objectJson)
    {
        if (std::count(request.begin(), request.end(), '|') != 4) {
            return false;
        }
        request += "|";
        request += Base64(objectJson);
        return true;
    }

    // Attaches a captured identity as field 4. Returns false only when nothing sendable was captured or the
    // request lacks its prefix fields; callers log that case rather than dropping the identity silently.
    inline bool AttachToRequest(std::string& request, const EventIdentity& identity)
    {
        const auto identityJson = Serialize(identity);
        return !identityJson.empty() && AppendField4(request, identityJson);
    }

    // Strict standard base64 (RFC 4648 alphabet, '=' padding only at the end). Returns nullopt on any defect.
    inline std::optional<std::string> DecodeBase64(std::string_view text)
    {
        if (text.empty() || text.size() % 4 != 0) {
            return std::nullopt;
        }
        auto value = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };
        std::string out;
        out.reserve(text.size() / 4 * 3);
        for (std::size_t index = 0; index < text.size(); index += 4) {
            const bool last = index + 4 == text.size();
            const int padding = last ? (text[index + 3] == '=' ? (text[index + 2] == '=' ? 2 : 1) : 0) : 0;
            unsigned bits = 0;
            for (int offset = 0; offset < 4; ++offset) {
                const int digit = offset >= 4 - padding ? 0 : value(text[index + offset]);
                if (digit < 0) return std::nullopt;
                bits = (bits << 6) | static_cast<unsigned>(digit);
            }
            // Non-canonical trailing bits would let two encodings carry one payload.
            if ((padding == 1 && (bits & 0xFF)) || (padding == 2 && (bits & 0xFFFF))) return std::nullopt;
            out += static_cast<char>((bits >> 16) & 0xFF);
            if (padding < 2) out += static_cast<char>((bits >> 8) & 0xFF);
            if (padding < 1) out += static_cast<char>(bits & 0xFF);
        }
        return out;
    }

    // Response identity v1: the optional fourth field of a streamed label|queue|payload line (agent guide).
    // The server names the physical (or typed reserved) actor it selected; the label is presentation only.
    inline constexpr int ResponseIdentityVersion = 1;
    inline constexpr std::uint32_t PlayerRefId = 0x14;

    struct ResponseEndpoint {
        enum class Kind { None, Narrator, Player, Physical };
        Kind kind = Kind::None;
        std::string id;           // Canonical actor key; "narrator" or "player" for the typed principals.
        std::uint32_t refId = 0;  // Runtime reference; 0 for the narrator, whose engine object is the player.

        bool IsNarrator() const { return kind == Kind::Narrator; }
        bool IsPlayer() const { return kind == Kind::Player; }
        bool IsPhysical() const { return kind == Kind::Physical; }
        bool Present() const { return kind != Kind::None; }
        bool operator==(const ResponseEndpoint&) const = default;
    };

    struct ResponseTarget {
        std::size_t arg = 0;  // Zero-based argument index after the command name.
        ResponseEndpoint actor;
    };

    struct ResponseIdentity {
        bool present = false;       // A fourth field was sent: consumers use it and never the label.
        ResponseEndpoint actor;
        ResponseEndpoint listener;  // None: unresolved or no listener; never filled from a payload label.
        std::vector<ResponseTarget> targets;

        const ResponseEndpoint* TargetFor(std::size_t arg) const
        {
            for (const auto& target : targets) {
                if (target.arg == arg) return &target.actor;
            }
            return nullptr;
        }
    };

    struct ResponseLine {
        std::string label;
        std::string queue;
        std::string payload;
        ResponseIdentity identity;
        std::string invalidReason;  // Non-empty: drop the line; never downgrade it to label routing.
        bool Valid() const { return invalidReason.empty(); }
    };

    inline std::optional<std::uint32_t> ParseRefIdHex(std::string_view text)
    {
        if (text.size() != 8) return std::nullopt;
        std::uint32_t value = 0;
        for (const char c : text) {
            const int digit = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (digit < 0) return std::nullopt;
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    // An endpoint {"id","refid"}: reserved principals by id only, physical actors by canonical key plus a
    // runtime RefID consistent with it (ref: keeps its plugin-local id, dyn: lives in the FF range).
    inline std::optional<ResponseEndpoint> ParseResponseEndpoint(const nlohmann::json& value, bool allowNarrator)
    {
        if (!value.is_object() || value.size() != 2 || !value.contains("id") || !value.contains("refid") ||
            !value["id"].is_string()) {
            return std::nullopt;
        }
        const auto id = value["id"].get<std::string>();
        const auto& refid = value["refid"];
        if (!ActorIdentityUtils::IsActorKey(id)) return std::nullopt;
        ResponseEndpoint endpoint;
        endpoint.id = id;
        if (id == ActorIdentityUtils::NarratorActorKey) {
            if (!allowNarrator || !refid.is_null()) return std::nullopt;
            endpoint.kind = ResponseEndpoint::Kind::Narrator;
            return endpoint;
        }
        if (!refid.is_string()) return std::nullopt;
        const auto ref = ParseRefIdHex(refid.get<std::string>());
        if (!ref || *ref == 0) return std::nullopt;
        endpoint.refId = *ref;
        if (id == ActorIdentityUtils::PlayerActorKey) {
            if (*ref != PlayerRefId) return std::nullopt;
            endpoint.kind = ResponseEndpoint::Kind::Player;
            return endpoint;
        }
        if (*ref == PlayerRefId) return std::nullopt;  // The player's reference is only ever the typed player.
        if (id.starts_with("dyn:")) {
            if (!ActorIdentityUtils::IsDynamicFormId(*ref)) return std::nullopt;
        } else {
            if (ActorIdentityUtils::IsDynamicFormId(*ref)) return std::nullopt;
            const auto local = ParseRefIdHex(std::string_view(id).substr(id.size() - 8));
            const bool light = (*ref >> 24) == 0xFE;
            if (!local || (light ? (*local > 0xFFF || (*ref & 0xFFF) != *local) : (*ref & 0xFFFFFF) != *local)) {
                return std::nullopt;
            }
        }
        endpoint.kind = ResponseEndpoint::Kind::Physical;
        return endpoint;
    }

    // Parses the base64 JSON fourth field. Unknown versions or keys, malformed endpoints and conflicting
    // duplicate target arguments are rejected; nothing is filled from display names.
    inline std::optional<ResponseIdentity> ParseResponseIdentity(std::string_view field, std::string* reason = nullptr)
    {
        auto fail = [&](const char* why) -> std::optional<ResponseIdentity> {
            if (reason) *reason = why;
            return std::nullopt;
        };
        const auto bytes = DecodeBase64(field);
        if (!bytes) return fail("identity_base64_invalid");
        const auto json = nlohmann::json::parse(*bytes, nullptr, false);
        if (json.is_discarded() || !json.is_object()) return fail("identity_json_invalid");
        for (const auto& item : json.items()) {
            const auto& key = item.key();
            if (key != "response_identity_version" && key != "actor" && key != "listener" && key != "targets") {
                return fail("identity_key_unsupported");
            }
        }
        const auto version = json.find("response_identity_version");
        if (version == json.end() || !version->is_number_integer() || *version != ResponseIdentityVersion) {
            return fail("identity_version_unsupported");
        }
        if (!json.contains("actor") || !json.contains("listener") || !json.contains("targets") ||
            !json["targets"].is_array()) {
            return fail("identity_shape_invalid");
        }
        ResponseIdentity identity;
        identity.present = true;
        const auto actor = ParseResponseEndpoint(json["actor"], true);
        if (!actor) return fail("identity_actor_invalid");
        identity.actor = *actor;
        if (!json["listener"].is_null()) {
            const auto listener = ParseResponseEndpoint(json["listener"], true);
            if (!listener) return fail("identity_listener_invalid");
            identity.listener = *listener;
        }
        for (const auto& entry : json["targets"]) {
            if (!entry.is_object() || entry.size() != 3 || !entry.contains("arg") || !entry.contains("id") ||
                !entry.contains("refid") || !entry["arg"].is_number_unsigned() ||
                entry["arg"].get<std::uint64_t>() > 64) {
                return fail("identity_target_invalid");
            }
            const nlohmann::json endpointJson = {{"id", entry["id"]}, {"refid", entry["refid"]}};
            const auto endpoint = ParseResponseEndpoint(endpointJson, false);
            if (!endpoint) return fail("identity_target_invalid");
            const auto arg = static_cast<std::size_t>(entry["arg"].get<std::uint64_t>());
            if (const auto* existing = identity.TargetFor(arg)) {
                if (!(*existing == *endpoint)) return fail("identity_target_conflict");
                continue;
            }
            identity.targets.push_back({arg, *endpoint});
        }
        return identity;
    }

    // Splits one streamed response line. Three fields are the legacy format (text after a third '|' was always
    // ignored, and an empty fourth field stays legacy); a non-empty fourth field must be a valid identity, and
    // a fifth field beside it is rejected. A decorated label must name the same RefID as the envelope actor.
    inline ResponseLine ParseResponseLine(std::string_view line)
    {
        ResponseLine parsed;
        std::vector<std::string_view> fields;
        std::size_t start = 0;
        while (true) {
            const auto separator = line.find('|', start);
            fields.push_back(line.substr(start, separator == std::string_view::npos ? std::string_view::npos
                                                                                     : separator - start));
            if (separator == std::string_view::npos) break;
            start = separator + 1;
        }
        fields.resize(std::max<std::size_t>(fields.size(), 3));  // Short legacy lines keep empty fields.
        parsed.label = std::string(fields[0]);
        parsed.queue = std::string(fields[1]);
        parsed.payload = std::string(fields[2]);
        auto field4 = fields.size() >= 4 ? fields[3] : std::string_view{};
        while (!field4.empty() && (field4.back() == '\r' || field4.back() == '\n' || field4.back() == ' ')) {
            field4.remove_suffix(1);
        }
        // Any non-empty field after the fourth is rejected even beside an empty fourth field, so an envelope
        // shifted by a stray '|' is never downgraded to label routing.
        for (std::size_t extra = 4; extra < fields.size(); ++extra) {
            auto rest = fields[extra];
            while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n' || rest.back() == ' ')) {
                rest.remove_suffix(1);
            }
            if (!rest.empty()) {
                parsed.invalidReason = "identity_extra_field";
                return parsed;
            }
        }
        if (field4.empty()) return parsed;
        if (fields.size() > 4) {
            parsed.invalidReason = "identity_extra_field";
            return parsed;
        }
        const auto identity = ParseResponseIdentity(field4, &parsed.invalidReason);
        if (!identity) return parsed;
        parsed.identity = *identity;
        if (const auto label = ActorTargetIdentifierUtils::Parse(parsed.label); label.hasRefIdMarker) {
            if (!label.hasRefId || parsed.identity.actor.IsNarrator() || label.refId != parsed.identity.actor.refId) {
                parsed.invalidReason = "identity_label_mismatch";
            }
        }
        return parsed;
    }

    // ScriptProxy actor identity v1, inside the ScriptProxy command JSON itself:
    //   "actor_identity_version": 1, "actor_targets": {"<parameter>": {"id": <actor key>, "refid": "XXXXXXXX"}}
    // Each named parameter must exist in the command and hold that RefID as Papyrus' json getters read it.
    inline constexpr int ScriptProxyIdentityVersion = 1;
    inline constexpr std::string_view ScriptProxyBindingKey = "chim_binding";  // Native-only; stripped from input.

    struct ScriptProxyIdentity {
        bool declared = false;  // Either metadata key was present: every actor parameter must then be bound.
        std::vector<std::pair<std::string, ResponseEndpoint>> targets;
        std::string invalidReason;  // Non-empty: drop the whole command.
        bool Valid() const { return invalidReason.empty(); }
        const ResponseEndpoint* TargetFor(std::string_view key) const
        {
            for (const auto& target : targets) {
                if (target.first == key) return &target.second;
            }
            return nullptr;
        }
    };

    inline bool IsScriptProxyReservedKey(std::string_view key)
    {
        return key == "cmdID" || key == "actor_identity_version" || key == "actor_targets" ||
               key == ScriptProxyBindingKey;
    }

    // The reference a ScriptProxy parameter names, read exactly as AIAgentFunctions.jsonGetActor/jsonGetReference
    // do: only strings, std::stoul base 0 (0x hex, leading-0 octal, decimal), else base 16. Numbers name nothing.
    inline std::uint32_t ScriptProxyRefValue(const nlohmann::json& value)
    {
        if (!value.is_string()) return 0;
        const auto text = value.get<std::string>();
        try {
            return static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
        } catch (...) {
        }
        try {
            return static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
        } catch (...) {
        }
        return 0;
    }

    inline ScriptProxyIdentity ParseScriptProxyIdentity(const nlohmann::json& command)
    {
        ScriptProxyIdentity identity;
        if (!command.is_object()) {
            identity.invalidReason = "scriptproxy_json_invalid";
            return identity;
        }
        const auto version = command.find("actor_identity_version");
        const auto targets = command.find("actor_targets");
        if (version == command.end() && targets == command.end()) return identity;
        identity.declared = true;
        const auto fail = [&](const char* why) {
            identity.invalidReason = why;
            identity.targets.clear();
            return identity;
        };
        if (version == command.end() || !version->is_number_integer() || *version != ScriptProxyIdentityVersion) {
            return fail("scriptproxy_identity_version_unsupported");
        }
        if (targets == command.end() || !targets->is_object()) return fail("scriptproxy_identity_shape_invalid");
        for (const auto& entry : targets->items()) {
            const auto& key = entry.key();
            if (IsScriptProxyReservedKey(key) || !command.contains(key)) return fail("scriptproxy_target_parameter_invalid");
            const auto endpoint = ParseResponseEndpoint(entry.value(), false);
            if (!endpoint) return fail("scriptproxy_target_invalid");
            if (ScriptProxyRefValue(command[key]) != endpoint->refId) return fail("scriptproxy_target_mismatch");
            identity.targets.emplace_back(key, *endpoint);
        }
        return identity;
    }

    // One active AI agent as a rechat request observed it: the canonical key the agent entry captured on the game
    // thread when its actor was assigned, the RefID it was captured for, and whether the handle bound for that
    // RefID still holds the same reference (and, for an FF actor, the same registry key) at dispatch.
    struct ActiveAgentCapture {
        bool typedNarrator = false;
        std::string label;           // Bare display name, legacy active_agents only.
        std::uint32_t formId = 0;
        std::string actorKey;
        std::uint32_t boundFormId = 0;  // RefID behind the bound handle; 0 when the actor did not resolve.
        bool boundStill = false;
    };

    struct ActiveAgentLists {
        std::vector<std::string> labels;  // Legacy human labels, unchanged.
        std::vector<std::string> keys;    // Exact physical canonical keys; may be empty (nobody active).
    };

    // A key is only sent for a physical agent whose capture verified: the typed narrator is never an active
    // physical key, and an agent whose actor changed (recycled FF slot, deleted, rebound) is dropped, not guessed.
    inline ActiveAgentLists BuildActiveAgentLists(const std::vector<ActiveAgentCapture>& captures)
    {
        ActiveAgentLists lists;
        for (const auto& capture : captures) {
            if (capture.typedNarrator) continue;
            if (!capture.label.empty()) lists.labels.push_back(capture.label);
            const auto& key = capture.actorKey;
            if (key == ActorIdentityUtils::NarratorActorKey || key == ActorIdentityUtils::PlayerActorKey ||
                !ActorIdentityUtils::IsActorKey(key) || capture.formId == 0 || capture.boundFormId != capture.formId ||
                !capture.boundStill || key.starts_with("dyn:") != ActorIdentityUtils::IsDynamicFormId(capture.formId) ||
                std::find(lists.keys.begin(), lists.keys.end(), key) != lists.keys.end()) {
                continue;
            }
            lists.keys.push_back(key);
        }
        return lists;
    }

    // Rechat payload, identity version 1. Canonical keys appear only here (the last "|" field); presentation
    // labels never replace them.
    struct RechatPayloadFields {
        std::string speaker;
        std::string speakerKey;
        std::string listenerHint;
        std::string listenerKey;  // Omitted when empty (listener unresolved by key).
        std::string rechatTargetHint;
        std::string resolvedRechatTarget;
        std::string originLine;
        int rechatDepth = 0;
        std::string chainId;
        ActiveAgentLists activeAgents;
    };

    inline nlohmann::json BuildRechatPayload(const RechatPayloadFields& fields)
    {
        nlohmann::json payload = nlohmann::json::object();
        payload["speaker"] = fields.speaker;
        payload["rechat_identity_version"] = 1;
        payload["speaker_key"] = fields.speakerKey;
        payload["listener_hint"] = fields.listenerHint;
        if (!fields.listenerKey.empty()) payload["listener_key"] = fields.listenerKey;
        payload["rechat_target_hint"] = fields.rechatTargetHint;
        payload["resolved_rechat_target"] = fields.resolvedRechatTarget;
        payload["origin_line"] = fields.originLine;
        payload["rechat_depth"] = fields.rechatDepth;
        payload["chain_id"] = fields.chainId;
        payload["active_agents"] = fields.activeAgents.labels;
        // Always an array, so an empty list explicitly means nobody is active.
        payload["active_agent_keys"] = nlohmann::json(fields.activeAgents.keys);
        return payload;
    }
}
