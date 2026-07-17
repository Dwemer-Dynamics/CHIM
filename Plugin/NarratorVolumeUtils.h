#pragma once

#include <algorithm>

namespace NarratorVolumeUtils
{
    constexpr float PercentToMultiplier(float percent)
    {
        return std::clamp(percent, 0.0f, 200.0f) / 100.0f;
    }

    constexpr float ApplyToLine(float lineMultiplier, bool isNarrator, float narratorMultiplier)
    {
        const float baseMultiplier = std::max(0.0f, lineMultiplier);
        return isNarrator ? baseMultiplier * std::max(0.0f, narratorMultiplier) : baseMultiplier;
    }
}
