#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace ChimInteraction {
    bool Enabled();
    bool Syncing();
    bool Failed();
    std::uint64_t Generation();
    void Synchronize();
    void Toggle();
    bool IsTrigger(std::string_view message);
    bool IsGameOutput(std::string_view channel);
}
