#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace ActorTargetIdentifierUtils
{
    struct ParsedTarget
    {
        std::string fallbackName;
        std::uint32_t refId = 0;
        bool hasRefId = false;
    };

    inline std::string Trim(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\n\r\f\v");
        if (first == std::string::npos) {
            return "";
        }

        const auto last = value.find_last_not_of(" \t\n\r\f\v");
        return value.substr(first, last - first + 1);
    }

    inline ParsedTarget Parse(std::string_view input)
    {
        ParsedTarget result{};
        std::string raw(input);
        result.fallbackName = Trim(raw);

        std::string lower = raw;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });

        const auto marker = lower.find("refid:");
        if (marker == std::string::npos) {
            return result;
        }

        auto valueStart = marker + 6;
        while (valueStart < raw.size() && std::isspace(static_cast<unsigned char>(raw[valueStart]))) {
            ++valueStart;
        }
        if (valueStart + 1 < raw.size() && raw[valueStart] == '0' &&
            (raw[valueStart + 1] == 'x' || raw[valueStart + 1] == 'X')) {
            valueStart += 2;
        }

        auto valueEnd = valueStart;
        while (valueEnd < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[valueEnd]))) {
            ++valueEnd;
        }

        const auto digitCount = valueEnd - valueStart;
        if (digitCount == 0 || digitCount > 8) {
            return result;
        }

        try {
            result.refId = static_cast<std::uint32_t>(
                std::stoul(raw.substr(valueStart, digitCount), nullptr, 16));
        } catch (...) {
            return result;
        }
        result.hasRefId = result.refId != 0;

        auto eraseStart = marker;
        auto scan = marker;
        while (scan > 0 && std::isspace(static_cast<unsigned char>(raw[scan - 1]))) {
            --scan;
        }
        if (scan > 0 && raw[scan - 1] == '[') {
            eraseStart = scan - 1;
        }

        auto eraseEnd = valueEnd;
        while (eraseEnd < raw.size() && std::isspace(static_cast<unsigned char>(raw[eraseEnd]))) {
            ++eraseEnd;
        }
        if (eraseEnd < raw.size() && raw[eraseEnd] == ']') {
            ++eraseEnd;
        }

        raw.erase(eraseStart, eraseEnd - eraseStart);
        result.fallbackName = Trim(raw);
        return result;
    }
}
