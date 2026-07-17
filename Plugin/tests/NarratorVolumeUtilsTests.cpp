#include "NarratorVolumeUtils.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
    void CheckNear(float actual, float expected, const char* message)
    {
        if (std::abs(actual - expected) > 0.0001f) {
            std::cerr << message << ": expected " << expected << ", got " << actual << std::endl;
            std::exit(1);
        }
    }
}

int main()
{
    using namespace NarratorVolumeUtils;

    CheckNear(PercentToMultiplier(100.0f), 1.0f, "Default narrator volume changed");
    CheckNear(PercentToMultiplier(40.0f), 0.4f, "Narrator attenuation was incorrect");
    CheckNear(PercentToMultiplier(-10.0f), 0.0f, "Negative narrator volume was not clamped");
    CheckNear(PercentToMultiplier(250.0f), 2.0f, "Narrator volume upper bound was not clamped");

    CheckNear(ApplyToLine(1.0f, true, 0.4f), 0.4f, "Narrator multiplier was not applied");
    CheckNear(ApplyToLine(1.3f, true, 0.5f), 0.65f, "Line boost and narrator volume did not compose");
    CheckNear(ApplyToLine(1.3f, false, 0.5f), 1.3f, "NPC volume was changed by narrator setting");
    CheckNear(ApplyToLine(-1.0f, true, 1.0f), 0.0f, "Negative line volume was not clamped");
    return 0;
}
