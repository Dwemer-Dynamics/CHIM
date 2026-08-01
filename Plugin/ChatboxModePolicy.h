#pragma once

#include <string_view>

namespace ChatboxModePolicy
{
    constexpr bool IsOneShot(std::string_view mode)
    {
        return mode == "DIRECTOR";
    }

    constexpr std::string_view ModeAfterSubmission(std::string_view mode)
    {
        return IsOneShot(mode) ? std::string_view("STANDARD") : mode;
    }
}
