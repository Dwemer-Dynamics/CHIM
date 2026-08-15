#pragma once

#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace ChatboxModePolicy
{
    struct SymbolModeRule
    {
        std::string_view prefix;
        std::string_view mode;
        std::string_view suffix;
    };

    struct ParsedSubmission
    {
        std::string message;
        std::string mode;
        std::string_view symbol;
        bool symbolOverride = false;
    };

    inline constexpr std::array<SymbolModeRule, 9> kSymbolModeRules{{
        { "((", "INJECTION_LOG", "))" },
        { "~~", "CLOSE", "" },
        { "!!", "SHOUT", "" },
        { "**", "AUTOCHAT", "" },
        { "~", "WHISPER", "" },
        { "@", "NARRATOR", "" },
        { ">", "DIRECTOR", "" },
        { "#", "CHEATMODE", "" },
        { "(", "INJECTION_CHAT", ")" },
    }};

    inline std::string_view Trim(std::string_view value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.remove_suffix(1);
        }
        return value;
    }

    // Parse a leading symbol for local routing; HerikaServer remains the execution authority.
    inline ParsedSubmission ParseSubmission(std::string_view message, std::string_view selectedMode)
    {
        ParsedSubmission result{ std::string(message), std::string(selectedMode), {}, false };
        for (const auto& rule : kSymbolModeRules) {
            if (!message.starts_with(rule.prefix)) {
                continue;
            }

            std::string_view content = Trim(message.substr(rule.prefix.size()));
            if (!rule.suffix.empty() && content.ends_with(rule.suffix)) {
                content.remove_suffix(rule.suffix.size());
                content = Trim(content);
            }

            result.message.assign(content);
            result.mode.assign(rule.mode);
            result.symbol = rule.prefix;
            result.symbolOverride = true;
            return result;
        }
        return result;
    }

    constexpr bool IsOneShot(std::string_view mode)
    {
        return mode == "DIRECTOR";
    }

    constexpr std::string_view ModeAfterSubmission(std::string_view mode)
    {
        return IsOneShot(mode) ? std::string_view("STANDARD") : mode;
    }
}
