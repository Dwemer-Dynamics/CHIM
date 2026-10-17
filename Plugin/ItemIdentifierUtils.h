#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>

namespace ItemIdentifierUtils
{
    struct InventoryItemIdentifier
    {
        std::optional<std::uint32_t> baseId;
        std::string name;
    };

    struct HeldItemState
    {
        std::uint32_t refId = 0;
        std::string name;
    };

    inline std::string Trim(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n\f\v");
        if (first == std::string::npos) {
            return "";
        }
        const auto last = value.find_last_not_of(" \t\r\n\f\v");
        return value.substr(first, last - first + 1);
    }

    inline std::optional<std::uint32_t> ParseFormId(std::string value)
    {
        value = Trim(std::move(value));
        if (value.size() >= 2 && value.front() == '`' && value.back() == '`') {
            value = Trim(value.substr(1, value.size() - 2));
        }

        const auto colon = value.find(':');
        auto idText = Trim(value.substr(0, colon));
        if (idText.starts_with("0x") || idText.starts_with("0X")) {
            idText = idText.substr(2);
        }

        if (idText.empty() || idText.size() > 8 ||
            !std::all_of(idText.begin(), idText.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
            return std::nullopt;
        }

        try {
            return static_cast<std::uint32_t>(std::stoul(idText, nullptr, 16));
        } catch (...) {
            return std::nullopt;
        }
    }

    inline InventoryItemIdentifier ParseInventoryItemIdentifier(std::string value)
    {
        value = Trim(std::move(value));
        if (value.size() >= 2 && value.front() == '`' && value.back() == '`') {
            value = Trim(value.substr(1, value.size() - 2));
        }

        InventoryItemIdentifier result{ std::nullopt, value };
        const auto colon = value.find(':');
        if (colon == std::string::npos) {
            return result;
        }

        const auto baseId = ParseFormId(value);
        if (!baseId.has_value()) {
            return result;
        }

        result.baseId = baseId;
        result.name = Trim(value.substr(colon + 1));

        return result;
    }

    inline std::string BuildHeldItemEvent(const std::string& itemName, const std::string& action,
                                          const std::string& slot, std::uint32_t refId)
    {
        return refId != 0
            ? std::format("{}^{}^{}^0x{:08X}", itemName, action, slot, refId)
            : std::format("{}^{}^{}", itemName, action, slot);
    }

    inline bool MatchesRequestedBaseId(const InventoryItemIdentifier& request, std::uint32_t candidateBaseId)
    {
        return request.baseId.has_value() && request.baseId.value() == candidateBaseId;
    }

    class HeldItemTracker
    {
    public:
        void Set(const std::string& slot, std::uint32_t refId, std::string itemName)
        {
            states_[slot] = HeldItemState{ refId, std::move(itemName) };
        }

        HeldItemState Get(const std::string& slot) const
        {
            const auto it = states_.find(slot);
            return it != states_.end() ? it->second : HeldItemState{};
        }

        void Clear(const std::string& slot)
        {
            states_.erase(slot);
        }

    private:
        std::unordered_map<std::string, HeldItemState> states_;
    };
}
