#include "ComeCloserRestoreScheduler.h"

#include "RE/Skyrim.h"
#include <SKSE/API.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace logger = SKSE::log;

namespace
{
    constexpr auto kRestoreTimeout = std::chrono::seconds(20);
    constexpr float kRestoreDistance = 300.0f;

    struct PendingRestore
    {
        RE::ObjectRefHandle actor;
        RE::ObjectRefHandle target;
        std::int32_t generation = 0;
        std::chrono::steady_clock::time_point startedAt{};
        std::chrono::steady_clock::time_point deadline{};
    };

    struct ReadyRestore
    {
        RE::ObjectRefHandle actor;
        std::int32_t generation = 0;
        float elapsedSeconds = 0.0f;
        float distance = 0.0f;
    };

    std::mutex g_restoreMutex;
    std::unordered_map<RE::FormID, PendingRestore> g_pendingRestores;
    std::atomic_bool g_pollQueued{ false };

    // Resolves actor handles and dispatches only restore operations that reached the player or timed out.
    void PollOnGameThread()
    {
        const auto now = std::chrono::steady_clock::now();
        std::vector<ReadyRestore> ready;

        {
            std::lock_guard lock(g_restoreMutex);
            for (auto it = g_pendingRestores.begin(); it != g_pendingRestores.end();)
            {
                auto actorRef = it->second.actor.get();
                auto targetRef = it->second.target.get();
                auto* actor = actorRef ? actorRef->As<RE::Actor>() : nullptr;
                if (!actor || !targetRef)
                {
                    logger::warn("[ComeCloser] Dropped pending restore for actor {:08X}; reference unloaded",
                                 it->first);
                    it = g_pendingRestores.erase(it);
                    continue;
                }

                const float distance = actor->GetPosition().GetDistance(targetRef->GetPosition());
                if (distance <= kRestoreDistance || now >= it->second.deadline)
                {
                    ready.push_back(ReadyRestore{
                        it->second.actor,
                        it->second.generation,
                        std::chrono::duration<float>(now - it->second.startedAt).count(),
                        distance });
                }
                ++it;
            }
        }

        for (const auto& pending : ready)
        {
            auto actorRef = pending.actor.get();
            auto* actor = actorRef ? actorRef->As<RE::Actor>() : nullptr;
            if (!actor)
            {
                continue;
            }

            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto actorArg = actor;
            auto generation = pending.generation;
            auto elapsedSeconds = pending.elapsedSeconds;
            auto distance = pending.distance;
            auto args = RE::MakeFunctionArguments(
                std::move(actorArg), std::move(generation), std::move(elapsedSeconds), std::move(distance));
            const bool dispatched = vm && vm->DispatchStaticCall(
                "AIAgentAIMind", "RestorePlayerFollowAfterComeCloser", args, callback);
            if (!dispatched)
            {
                logger::error("[ComeCloser] Failed to dispatch player follow restore for {:08X}", actor->GetFormID());
                continue;
            }

            std::lock_guard lock(g_restoreMutex);
            auto current = g_pendingRestores.find(actor->GetFormID());
            if (current != g_pendingRestores.end() && current->second.generation == pending.generation)
            {
                g_pendingRestores.erase(current);
            }
        }
    }
}

namespace ComeCloserRestoreScheduler
{
    bool Schedule(RE::Actor* actor, RE::TESObjectREFR* target, std::int32_t generation)
    {
        if (!actor || !target || generation < 1)
        {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(g_restoreMutex);
            g_pendingRestores.insert_or_assign(
                actor->GetFormID(),
                PendingRestore{ actor->GetHandle(), target->GetHandle(), generation, now, now + kRestoreTimeout });
        }
        logger::info("[ComeCloser] Scheduled generation {} restore for {}", generation, actor->GetDisplayFullName());
        return true;
    }

    void QueuePoll()
    {
        {
            std::lock_guard lock(g_restoreMutex);
            if (g_pendingRestores.empty())
            {
                return;
            }
        }

        bool expected = false;
        if (!g_pollQueued.compare_exchange_strong(expected, true))
        {
            return;
        }

        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface)
        {
            g_pollQueued.store(false);
            logger::error("[ComeCloser] Cannot queue restore poll; SKSE task interface unavailable");
            return;
        }

        taskInterface->AddTask([]() {
            try
            {
                PollOnGameThread();
            }
            catch (const std::exception& error)
            {
                logger::error("[ComeCloser] Restore poll failed: {}", error.what());
            }
            g_pollQueued.store(false);
        });
    }

    void Clear()
    {
        std::lock_guard lock(g_restoreMutex);
        g_pendingRestores.clear();
    }
}
