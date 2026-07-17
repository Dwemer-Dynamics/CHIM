#pragma once

#include <algorithm>

namespace HeadVoiceVolumeUtils
{
    constexpr bool IsHeadVoice(bool isNarrator, bool isPlayer)
    {
        return isNarrator || isPlayer;
    }

    constexpr float PercentToMultiplier(float percent)
    {
        return std::clamp(percent, 0.0f, 200.0f) / 100.0f;
    }

    constexpr float ApplyToLine(float lineMultiplier, bool isHeadVoice, float headVoiceMultiplier)
    {
        const float baseMultiplier = std::max(0.0f, lineMultiplier);
        return isHeadVoice ? baseMultiplier * std::max(0.0f, headVoiceMultiplier) : baseMultiplier;
    }
}
