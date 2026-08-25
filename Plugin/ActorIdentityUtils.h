#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace ActorIdentityUtils
{
    inline std::string BuildPromptIdentifier(std::string_view displayName, std::uint32_t refId)
    {
        if (displayName.empty() || refId == 0) {
            return std::string(displayName);
        }
        return std::format("{} [RefID: {:08X}]", displayName, refId);
    }
}
