#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

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

    inline std::string BuildPromptIdentifier(std::string_view displayName, std::uint32_t refId)
    {
        if (displayName.empty() || refId == 0) {
            return std::string(displayName);
        }
        return std::format("{} [RefID: {:08X}]", displayName, refId);
    }
}
