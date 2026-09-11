#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace SpatialDoorStatePolicy
{
    // Event state belongs to one loaded cell and must never survive a save reload.
    // Callers serialize access; storing IDs avoids retaining game references.
    class ObservedDoors
    {
    public:
        void SetCell(std::uint32_t cellId)
        {
            if (cellId != cellId_) {
                Reset();
                cellId_ = cellId;
            }
        }

        void Record(std::uint32_t doorId, bool opened)
        {
            if (cellId_ == 0 || doorId == 0) {
                return;
            }
            if (doors_.size() >= 1024 && !doors_.contains(doorId)) {
                doors_.erase(doors_.begin());
            }
            doors_[doorId] = opened;
        }

        std::optional<bool> Get(std::uint32_t cellId, std::uint32_t doorId) const
        {
            if (cellId != cellId_) {
                return std::nullopt;
            }
            const auto it = doors_.find(doorId);
            return it == doors_.end() ? std::nullopt : std::optional<bool>(it->second);
        }

        void Reset()
        {
            doors_.clear();
            cellId_ = 0;
        }

        void Forget(std::uint32_t doorId)
        {
            doors_.erase(doorId);
        }

    private:
        std::uint32_t cellId_ = 0;
        std::unordered_map<std::uint32_t, bool> doors_;
    };
}
