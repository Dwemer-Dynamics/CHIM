#pragma once

#include <cstdint>
#include <string>

namespace RE
{
    class Actor;
    struct TESGrabReleaseEvent;
}

namespace VRItemAwareness
{
    void Initialize();
    void Tick();
    void HandleFlatGrabReleaseEvent(const RE::TESGrabReleaseEvent* event);
    std::string BeginHeldItemHandoff(RE::Actor* recipient, std::uint32_t itemRefId, const std::string& itemName);
    void CancelPendingHandoff(const char* reason);
}
