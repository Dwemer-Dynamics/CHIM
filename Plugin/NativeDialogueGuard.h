#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace NativeDialogue
{
    class Guard
    {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        explicit Guard(
            std::chrono::milliseconds quietWindow = std::chrono::milliseconds(1500),
            std::chrono::milliseconds staleTopicTimeout = std::chrono::minutes(2))
            : quietWindow_(quietWindow), staleTopicTimeout_(staleTopicTimeout)
        {}

        void SetEnabled(bool enabled)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                enabled_ = enabled;
                if (!enabled_) {
                    ClearLocked();
                }
            }
            condition_.notify_all();
        }

        bool IsEnabled() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return enabled_;
        }

        void RecordStart(std::uint32_t speakerFormId, std::uint32_t topicInfoFormId,
                         TimePoint now = Clock::now())
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!enabled_) {
                    return;
                }
                const auto key = MakeKey(speakerFormId, topicInfoFormId);
                if (!activeTopics_.contains(key) && activeTopics_.size() >= kMaxActiveTopics) {
                    auto oldest = activeTopics_.begin();
                    for (auto it = activeTopics_.begin(); it != activeTopics_.end(); ++it) {
                        if (it->second < oldest->second) {
                            oldest = it;
                        }
                    }
                    activeTopics_.erase(oldest);
                }
                activeTopics_[key] = now;
                lastActivity_ = now;
                hasActivity_ = true;
            }
            condition_.notify_all();
        }

        void RecordStop(std::uint32_t speakerFormId, std::uint32_t topicInfoFormId,
                        TimePoint now = Clock::now())
        {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!enabled_) {
                    return;
                }
                changed = activeTopics_.erase(MakeKey(speakerFormId, topicInfoFormId)) > 0;
                if (changed) {
                    lastActivity_ = now;
                    hasActivity_ = true;
                }
            }
            if (changed) {
                condition_.notify_all();
            }
        }

        bool ShouldHold(TimePoint now = Clock::now())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return ShouldHoldLocked(now);
        }

        template <class CancelPredicate>
        bool WaitUntilQuiet(CancelPredicate&& isCancelled)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto cancellationGeneration = cancellationGeneration_;
            while (enabled_ && ShouldHoldLocked(Clock::now())) {
                if (cancellationGeneration_ != cancellationGeneration || isCancelled()) {
                    return false;
                }
                condition_.wait_for(lock, std::chrono::milliseconds(100));
            }
            return cancellationGeneration_ == cancellationGeneration && !isCancelled();
        }

        void CancelAndClear()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++cancellationGeneration_;
                ClearLocked();
            }
            condition_.notify_all();
        }

        std::size_t ActiveTopicCount() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return activeTopics_.size();
        }

    private:
        static constexpr std::size_t kMaxActiveTopics = 128;

        static std::uint64_t MakeKey(std::uint32_t speakerFormId, std::uint32_t topicInfoFormId)
        {
            return (static_cast<std::uint64_t>(speakerFormId) << 32) | topicInfoFormId;
        }

        void PruneStaleTopicsLocked(TimePoint now)
        {
            for (auto it = activeTopics_.begin(); it != activeTopics_.end();) {
                if (now - it->second >= staleTopicTimeout_) {
                    it = activeTopics_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        bool ShouldHoldLocked(TimePoint now)
        {
            if (!enabled_) {
                return false;
            }
            PruneStaleTopicsLocked(now);
            if (!activeTopics_.empty()) {
                return true;
            }
            return hasActivity_ && now - lastActivity_ < quietWindow_;
        }

        void ClearLocked()
        {
            activeTopics_.clear();
            hasActivity_ = false;
            lastActivity_ = {};
        }

        mutable std::mutex mutex_;
        std::condition_variable condition_;
        std::unordered_map<std::uint64_t, TimePoint> activeTopics_;
        std::chrono::milliseconds quietWindow_;
        std::chrono::milliseconds staleTopicTimeout_;
        TimePoint lastActivity_{};
        std::uint64_t cancellationGeneration_ = 0;
        bool hasActivity_ = false;
        bool enabled_ = false;
    };

    inline Guard& GetGuard()
    {
        static Guard guard;
        return guard;
    }
}
