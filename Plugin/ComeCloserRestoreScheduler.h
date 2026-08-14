#pragma once

#include <cstdint>

namespace RE
{
    class Actor;
    class TESObjectREFR;
}

namespace ComeCloserRestoreScheduler
{
    // Starts or replaces the bounded restore operation for one actor after Papyrus applies soft follow.
    bool Schedule(RE::Actor* actor, RE::TESObjectREFR* target, std::int32_t generation);
    // Queues one game-thread poll when at least one ComeCloser restore is pending.
    void QueuePoll();
    void Clear();
}
