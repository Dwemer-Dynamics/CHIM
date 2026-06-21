#pragma once

namespace RE
{
    struct TESGrabReleaseEvent;
}

namespace VRItemAwareness
{
    void Initialize();
    void Tick();
    void HandleFlatGrabReleaseEvent(const RE::TESGrabReleaseEvent* event);
}
