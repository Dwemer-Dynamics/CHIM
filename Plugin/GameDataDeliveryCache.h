#pragma once

#include "ActorIdentityUtils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Per-actor gamedata change-hash cache that only remembers a payload after its POST was acknowledged
// (HTTP 2xx). Each send gets a process-wide ticket. Sends for one FormID and one identity/load scope (the
// "key|refid|load#" prefix of ActorIdentityUtils::GameDataChangeHash) form a lane:
// - Begin never starts a second request while one is in flight; a different or forced payload is deferred
//   and marks the lane dirty, so HTTP delivery within a lane is serialized (older applies before newer).
// - Issue (inventory, which has completion waiters) may overlap; any overlap marks the lane dirty.
// - A success is recorded only for the newest ticket of a clean lane with nothing else in flight. Otherwise
//   the hash stays undelivered and NeedsRetry lets the existing refresh cadence resend current state once
//   the lane is idle, so the latest state is what the server applies last.
// A new scope (recycled FormID, replacement actor, new load) starts a fresh lane; acknowledgements from an
// older lane are ignored and can neither record nor clear. Failures back off; no retry queue or payload is kept.
// A request still in flight after kPendingWindow counts as lost (a failure): NeedsRetry or the next Begin/Issue
// abandons its lane, so the cadence resends current state and the lost ticket's late callback is ignored.
class GameDataDeliveryCache
{
public:
    using Clock = std::chrono::steady_clock;
    // An in-flight request older than this is treated as lost. The POST has no hard deadline (blocking connect,
    // 5 s socket send/recv per call, thread-pool queueing), so a slower callback is possible and is then ignored.
    static constexpr std::chrono::seconds kPendingWindow{ 60 };
    static constexpr std::chrono::seconds kRetryBase{ 15 };
    static constexpr int kRetryMaxShift = 4;  // failure backoff 15s, 30s, 60s, 120s cap

    bool Delivered(std::uint32_t formID, const std::string& hash) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(formID);
        return it != entries_.end() && !it->second.delivered.empty() && it->second.delivered == hash;
    }

    // Starts a send unconditionally (may overlap an in-flight request of the same lane, marking it dirty).
    std::uint64_t Issue(std::uint32_t formID, const std::string& hash, Clock::time_point now = Clock::now())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = entries_[formID];
        EnterLane(entry, hash, now);
        return IssueLocked(entry, hash, now);
    }

    // Returns 0 when the hash was already delivered, or while the lane has a request in flight (a different or
    // forced payload is then deferred to the next refresh via the dirty flag); otherwise the ticket for Complete.
    std::uint64_t Begin(std::uint32_t formID, const std::string& hash, bool force, Clock::time_point now = Clock::now())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = entries_[formID];
        EnterLane(entry, hash, now);
        if (entry.inFlight > 0) {
            if (force || entry.pending != hash) entry.dirty = true;
            return 0;
        }
        if (!force && !entry.delivered.empty() && entry.delivered == hash) return 0;
        return IssueLocked(entry, hash, now);
    }

    // Returns true only when this ticket is the newest of a clean, otherwise idle lane and the hash was recorded.
    bool Complete(std::uint32_t formID, std::uint64_t ticket, const std::string& hash, bool success,
                  Clock::time_point now = Clock::now())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(formID);
        if (ticket == 0 || it == entries_.end()) return false;
        auto& entry = it->second;
        if (ticket < entry.laneFirst || ticket > entry.latest) return false;  // older lane or lost request
        if (entry.inFlight > 0) --entry.inFlight;
        if (!success) {
            entry.dirty = true;
            entry.failures = std::min(entry.failures + 1, kRetryMaxShift);
            entry.lastAttempt = now;
        }
        if (ticket != entry.latest) return false;
        entry.pending.clear();
        if (!success || entry.dirty || entry.inFlight > 0) return false;
        entry.delivered = hash;
        entry.failures = 0;
        return true;
    }

    // True when the newest state of this FormID is not known to be applied, nothing is in flight, and the backoff
    // after the last failure elapsed. Callers re-harvest current state and send it through Begin/Issue.
    // A request lost beyond kPendingWindow is expired here, so the cadence alone recovers a missing callback.
    bool NeedsRetry(std::uint32_t formID, Clock::time_point now = Clock::now())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(formID);
        if (it == entries_.end()) return false;
        auto& entry = it->second;
        ExpireLost(entry, now);
        if (entry.latest == 0 || entry.inFlight > 0 || !entry.delivered.empty()) return false;
        return entry.failures == 0 || now - entry.lastAttempt >= kRetryBase * (1 << (entry.failures - 1));
    }

    std::uint64_t Latest(std::uint32_t formID) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(formID);
        return it == entries_.end() ? 0 : it->second.latest;
    }

private:
    struct Entry
    {
        std::string scope;
        std::string delivered;
        std::string pending;
        Clock::time_point pendingSince{};
        Clock::time_point lastAttempt{};
        std::uint64_t latest = 0;
        std::uint64_t laneFirst = 0;  // tickets below this belong to an abandoned lane
        int inFlight = 0;
        int failures = 0;
        bool dirty = false;
    };

public:
    // "actor_key|REFID8HEX|loadGeneration#" prefix written by ActorIdentityUtils::GameDataChangeHash; empty (one
    // lane) for an unprefixed hash. A key never contains '#', so the first '#' ends the prefix; the refid and load
    // are the last two '|' fields before it, and everything left of them must be a canonical actor key (a static
    // ref:<plugin>|00XXXXXX key itself contains '|').
    static std::string ScopeOf(const std::string& hash)
    {
        const auto end = hash.find('#');
        if (end == std::string::npos || end == 0) return {};
        const auto second = hash.rfind('|', end - 1);
        if (second == std::string::npos || second < 9 || second + 1 == end || hash[second - 9] != '|') return {};
        const auto first = second - 9;
        for (auto i = first + 1; i < second; ++i)
            if (!std::isxdigit(static_cast<unsigned char>(hash[i]))) return {};
        for (auto i = second + 1; i < end; ++i)
            if (!std::isdigit(static_cast<unsigned char>(hash[i]))) return {};
        if (!ActorIdentityUtils::IsActorKey(std::string_view(hash).substr(0, first))) return {};
        return hash.substr(0, end + 1);
    }

private:

    static std::uint64_t NextTicket()
    {
        static std::atomic<std::uint64_t> nextTicket{ 0 };
        return ++nextTicket;
    }

    static void AbandonLane(Entry& entry)
    {
        entry.laneFirst = entry.latest + 1;
        entry.inFlight = 0;
        entry.pending.clear();
        entry.delivered.clear();
        entry.dirty = false;
    }

    // A request in flight beyond the pending window is lost: its lane is abandoned (late tickets are ignored) and
    // it counts as a failure measured from the send, so repeated losses back off up to the cap.
    static void ExpireLost(Entry& entry, Clock::time_point now)
    {
        if (entry.inFlight == 0 || now - entry.pendingSince < kPendingWindow) return;
        AbandonLane(entry);
        entry.failures = std::min(entry.failures + 1, kRetryMaxShift);
        entry.lastAttempt = entry.pendingSince;
    }

    // Starts a fresh lane on identity/load change and expires a request lost beyond the pending window.
    static void EnterLane(Entry& entry, const std::string& hash, Clock::time_point now)
    {
        auto scope = ScopeOf(hash);
        if (entry.latest != 0 && scope != entry.scope) {
            AbandonLane(entry);
            entry.failures = 0;
        } else {
            ExpireLost(entry, now);
        }
        entry.scope = std::move(scope);
    }

    static std::uint64_t IssueLocked(Entry& entry, const std::string& hash, Clock::time_point now)
    {
        const std::uint64_t ticket = NextTicket();
        // Nothing else in flight: this send carries current state, so earlier deferrals are satisfied.
        entry.dirty = entry.inFlight > 0;
        entry.latest = ticket;
        ++entry.inFlight;
        entry.delivered.clear();
        entry.pending = hash;
        entry.pendingSince = now;
        entry.lastAttempt = now;
        return ticket;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, Entry> entries_;
};
