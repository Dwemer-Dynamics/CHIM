#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
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

    // Physical actor key sent to the server. Placed references keep the same ref: form as
    // BuildProfileKey; dynamic (FF) actors need their persisted UUID. Names never become keys.
    inline std::string BuildActorKey(std::string_view source, std::string_view dynamicUuid = {})
    {
        if (source.find('/') != std::string_view::npos) {
            return BuildProfileKey(std::string(source), 0);
        }
        return IsDynamicUuid(dynamicUuid) ? "dyn:" + std::string(dynamicUuid) : std::string{};
    }

    // Mirrors chimIsActorKey(): player, narrator, ref:<lower plugin>|<8 upper hex <= 00FFFFFF>, dyn:<uuid>.
    inline bool IsActorKey(std::string_view key)
    {
        if (key == PlayerActorKey || key == NarratorActorKey) {
            return true;
        }
        if (key.starts_with("dyn:")) {
            return IsDynamicUuid(key.substr(4));
        }
        if (!key.starts_with("ref:")) {
            return false;
        }
        const auto body = key.substr(4);
        const auto separator = body.rfind('|');
        if (separator == std::string_view::npos || separator == 0 || body.size() - separator != 9) {
            return false;
        }
        const auto plugin = body.substr(0, separator);
        if (plugin.find_first_of("/\\|@#\r\n") != std::string_view::npos ||
            std::any_of(plugin.begin(), plugin.end(), [](unsigned char c) { return c >= 'A' && c <= 'Z'; })) {
            return false;
        }
        const auto local = body.substr(separator + 1);
        return local.starts_with("00") && std::all_of(local.begin(), local.end(), [](char c) {
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
}
