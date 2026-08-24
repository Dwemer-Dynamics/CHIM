#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace ActorIdentityUtils
{
    inline std::string HexEncode(std::string_view value)
    {
        std::string encoded;
        encoded.reserve(value.size() * 2);
        for (const unsigned char character : value) {
            encoded += std::format("{:02x}", character);
        }
        return encoded;
    }

    inline std::string BuildPlacedActorKey(std::string_view pluginName, std::uint32_t localFormId)
    {
        return std::format("skyrim-ref-v1:{}:{:08x}", HexEncode(pluginName), localFormId);
    }

    inline std::string BuildRuntimeActorKey(std::string_view instanceId)
    {
        std::string normalized(instanceId);
        normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](unsigned char character) {
            return !std::isxdigit(character);
        }), normalized.end());
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return "skyrim-runtime-v1:" + normalized;
    }

    inline std::string BuildPromptIdentifier(std::string_view displayName, std::uint32_t refId)
    {
        if (displayName.empty() || refId == 0) {
            return std::string(displayName);
        }
        return std::format("{} [RefID: {:08X}]", displayName, refId);
    }
}
