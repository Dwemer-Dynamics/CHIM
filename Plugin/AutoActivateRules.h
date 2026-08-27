#pragma once

#include <RE/Skyrim.h>

#include <string>
#include <string_view>

#include "json.hpp"

namespace AutoActivateRules {
    inline constexpr std::size_t kMaxRules = 32;
    inline constexpr std::size_t kMaxTextLength = 64;
    inline constexpr std::size_t kMaxSerializedBytes = 64 * 1024;

    // Applies the first matching configured rule, then falls back to the existing automatic eligibility result.
    bool ShouldAutoActivate(RE::Actor* actor, RE::Actor* player, bool legacyEligible);

    bool ReplaceFromJsonText(std::string_view raw, std::string& error);
    std::string Serialize();
    nlohmann::json BuildPrismaSnapshot();
    void Clear();
}
