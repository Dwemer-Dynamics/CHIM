#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ActorIdentityUtils.h"
#include "EventIdentityUtils.h"
#include "json.hpp"

// Physical diary identity carried as the optional sixth spawnBook argument (base64 JSON, version 1).
// Five-argument spawnBook stays the legacy title-cached path for generic notes, letters and containers.
namespace DiaryBookIdentityUtils
{
    inline constexpr int IdentityVersion = 1;
    inline constexpr std::uint32_t RecordVersion = 2;  // v2 adds the dropped world reference per instance
    inline constexpr std::size_t MaxKeyLength = 512;
    inline constexpr std::size_t MaxContentLength = 1u << 20;

    struct DiaryIdentity {
        std::string bookKey;
        std::string authorKey;
        std::string recipientKey;
        bool operator==(const DiaryIdentity&) const = default;
    };

    // Physical keys only: the player, narrator and keeper are never a diary author or recipient.
    inline bool IsPhysicalActorKey(std::string_view key)
    {
        return key != ActorIdentityUtils::PlayerActorKey && key != ActorIdentityUtils::NarratorActorKey &&
               ActorIdentityUtils::IsActorKey(key);
    }

    inline std::optional<DiaryIdentity> ParseIdentity(const nlohmann::json& value)
    {
        if (!value.is_object() || value.size() != 4) {
            return std::nullopt;
        }
        const auto version = value.find("identity_version");
        if (version == value.end() || !version->is_number_integer() || version->get<int>() != IdentityVersion) {
            return std::nullopt;
        }
        auto text = [&](const char* name) -> std::optional<std::string> {
            const auto field = value.find(name);
            if (field == value.end() || !field->is_string()) {
                return std::nullopt;
            }
            return field->get<std::string>();
        };
        const auto bookKey = text("book_key");
        const auto authorKey = text("author_key");
        const auto recipientKey = text("recipient_key");
        if (!bookKey || !authorKey || !recipientKey || !IsPhysicalActorKey(*authorKey) ||
            !IsPhysicalActorKey(*recipientKey) || *bookKey != "diary:" + *authorKey) {
            return std::nullopt;
        }
        return DiaryIdentity{*bookKey, *authorKey, *recipientKey};
    }

    // nullopt means malformed; a malformed sixth argument must never fall back to the legacy title cache.
    inline std::optional<DiaryIdentity> ParseIdentityArgument(std::string_view argument)
    {
        constexpr std::string_view prefix = "b64:";
        if (argument.starts_with(prefix)) {
            argument.remove_prefix(prefix.size());
        }
        const auto decoded = EventIdentityUtils::DecodeBase64(argument);
        if (!decoded) {
            return std::nullopt;
        }
        const auto json = nlohmann::json::parse(*decoded, nullptr, false);
        return json.is_discarded() ? std::nullopt : ParseIdentity(json);
    }

    // The bound recipient's canonical key (and the response envelope target, when present) must match.
    inline bool RecipientMatches(const DiaryIdentity& identity, std::string_view boundRecipientKey,
                                 std::string_view envelopeTargetKey = {})
    {
        return !boundRecipientKey.empty() && identity.recipientKey == boundRecipientKey &&
               (envelopeTargetKey.empty() || envelopeTargetKey == boundRecipientKey);
    }

    // Captured when an identified diary rolecommand is dispatched. The deferred game-thread delivery must stay in
    // that load and, when the queued rolecommand bound its recipient argument, reach that same physical reference
    // (a static NPC keeps its key across loads, so the key alone cannot reject an old load's book).
    struct DispatchBinding {
        std::uint64_t loadEpoch = 0;
        std::uint32_t recipientFormId = 0;
        bool recipientBound = false;  // The queued item bound argument 2; its handle is checked again at delivery.
        std::uint32_t boundFormId = 0;
    };

    // boundStillSame: the bound handle still names the same reference and key (ignored when nothing was bound).
    inline bool DispatchStillCurrent(const DispatchBinding& binding, std::uint64_t currentLoad, bool boundStillSame)
    {
        if (binding.recipientFormId == 0 || binding.loadEpoch != currentLoad) return false;
        return !binding.recipientBound || (boundStillSame && binding.boundFormId == binding.recipientFormId);
    }

    // FNV-1a 64: immutable content-version tag stored beside the content itself.
    inline std::uint64_t ContentHash(std::string_view content)
    {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (const unsigned char c : content) {
            hash = (hash ^ c) * 0x100000001b3ull;
        }
        return hash;
    }

    // One spawned physical copy: the engine ExtraUniqueID (owner container form + 16-bit id) bound to an
    // immutable logical instance, its identity and the exact text it was rendered with.
    struct Instance {
        std::uint32_t instanceId = 0;
        std::uint32_t ownerFormId = 0;
        std::uint16_t uniqueId = 0;
        DiaryIdentity identity;
        std::uint64_t contentHash = 0;
        std::string content;
        // Non-zero while the copy lies in the world: the dropped reference keeps the association.
        std::uint32_t referenceFormId = 0;
        bool operator==(const Instance&) const = default;
    };

    // ExtraUniqueID ids are 16-bit and per container; 0 means "none". Never assume a random id fits.
    inline std::optional<std::uint16_t> AllocateUniqueId(const std::set<std::uint16_t>& used,
                                                         std::uint16_t preferred = 1)
    {
        std::uint32_t candidate = preferred == 0 ? 1 : preferred;
        for (std::uint32_t tried = 0; tried < 0xFFFF; ++tried) {
            if (!used.contains(static_cast<std::uint16_t>(candidate))) {
                return static_cast<std::uint16_t>(candidate);
            }
            candidate = candidate == 0xFFFF ? 1 : candidate + 1;
        }
        return std::nullopt;
    }

    namespace detail
    {
        inline void PutU32(std::string& out, std::uint32_t v)
        {
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
        inline void PutU64(std::string& out, std::uint64_t v)
        {
            for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
        inline void PutText(std::string& out, std::string_view text)
        {
            PutU32(out, static_cast<std::uint32_t>(text.size()));
            out.append(text);
        }
        struct Reader {
            std::string_view data;
            std::size_t offset = 0;
            std::optional<std::uint64_t> Uint(int bytes)
            {
                if (data.size() - offset < static_cast<std::size_t>(bytes)) return std::nullopt;
                std::uint64_t v = 0;
                for (int i = 0; i < bytes; ++i) {
                    v |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[offset + i])) << (8 * i);
                }
                offset += bytes;
                return v;
            }
            std::optional<std::string> Text(std::size_t limit)
            {
                const auto size = Uint(4);
                if (!size || *size > limit || data.size() - offset < *size) return std::nullopt;
                std::string text(data.substr(offset, static_cast<std::size_t>(*size)));
                offset += static_cast<std::size_t>(*size);
                return text;
            }
        };
    }

    // AIBK record body: count, then per instance id, owner, unique id, keys, content hash and content.
    inline std::string SerializeInstances(const std::vector<Instance>& instances)
    {
        std::string out;
        detail::PutU32(out, static_cast<std::uint32_t>(instances.size()));
        for (const auto& entry : instances) {
            detail::PutU32(out, entry.instanceId);
            detail::PutU32(out, entry.ownerFormId);
            detail::PutU32(out, entry.uniqueId);
            detail::PutText(out, entry.identity.bookKey);
            detail::PutText(out, entry.identity.authorKey);
            detail::PutText(out, entry.identity.recipientKey);
            detail::PutU64(out, entry.contentHash);
            detail::PutText(out, entry.content);
            detail::PutU32(out, entry.referenceFormId);
        }
        return out;
    }

    // Rejects the whole record on any malformed, tampered or ambiguous entry: callers reset to empty state.
    inline std::optional<std::vector<Instance>> ParseInstances(std::string_view data,
                                                             std::uint32_t version = RecordVersion)
    {
        detail::Reader reader{data};
        const auto count = reader.Uint(4);
        if (!count || version == 0 || version > RecordVersion) return std::nullopt;
        std::vector<Instance> instances;
        for (std::uint64_t index = 0; index < *count; ++index) {
            Instance entry;
            const auto instanceId = reader.Uint(4);
            const auto owner = reader.Uint(4);
            const auto unique = reader.Uint(4);
            auto bookKey = reader.Text(MaxKeyLength);
            auto authorKey = reader.Text(MaxKeyLength);
            auto recipientKey = reader.Text(MaxKeyLength);
            const auto hash = reader.Uint(8);
            auto content = reader.Text(MaxContentLength);
            const auto reference = version >= 2 ? reader.Uint(4) : std::optional<std::uint64_t>{0};
            if (!reference || !instanceId || !owner || !unique || !bookKey || !authorKey || !recipientKey || !hash || !content ||
                *instanceId == 0 || *unique == 0 || *unique > 0xFFFF) {
                return std::nullopt;
            }
            entry.instanceId = static_cast<std::uint32_t>(*instanceId);
            entry.ownerFormId = static_cast<std::uint32_t>(*owner);
            entry.uniqueId = static_cast<std::uint16_t>(*unique);
            nlohmann::json identity{{"identity_version", IdentityVersion}, {"book_key", *bookKey},
                                    {"author_key", *authorKey}, {"recipient_key", *recipientKey}};
            const auto parsed = ParseIdentity(identity);
            if (!parsed || ContentHash(*content) != *hash) return std::nullopt;
            entry.identity = *parsed;
            entry.contentHash = *hash;
            entry.content = std::move(*content);
            entry.referenceFormId = static_cast<std::uint32_t>(*reference);
            const bool duplicate = std::any_of(instances.begin(), instances.end(), [&](const Instance& other) {
                return other.instanceId == entry.instanceId ||
                       (other.ownerFormId == entry.ownerFormId && other.uniqueId == entry.uniqueId) ||
                       (entry.referenceFormId != 0 && other.referenceFormId == entry.referenceFormId);
            });
            if (duplicate) return std::nullopt;
            instances.push_back(std::move(entry));
        }
        if (reader.offset != data.size()) return std::nullopt;
        return instances;
    }

    // Read/ownership lookup: exact engine association only. No title, name or generic-book fallback.
    inline const Instance* FindByUniqueId(const std::vector<Instance>& instances, std::uint32_t ownerFormId,
                                          std::uint16_t uniqueId)
    {
        if (uniqueId == 0) return nullptr;
        const auto it = std::find_if(instances.begin(), instances.end(), [&](const Instance& entry) {
            return entry.ownerFormId == ownerFormId && entry.uniqueId == uniqueId;
        });
        return it == instances.end() ? nullptr : &*it;
    }
}
