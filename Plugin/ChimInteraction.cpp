#include "ChimInteraction.h"
#include "HTTPManager.h"
#include "PrismaUIBridge.h"
#include "DirectorScene.h"
#include "SpeakManager.h"
#include "SPGResponse.h"
#include "ThreadPool.h"
#include <atomic>
#include <mutex>
#include <optional>

namespace ChimInteraction {
namespace {
    std::atomic<bool> enabled{false}, syncing{false}, failed{false}, known{false};
    std::atomic<std::uint64_t> generation{0};
    std::mutex syncMutex;
    std::optional<bool> pendingState;
    std::chrono::steady_clock::time_point nextAttempt{};

    // Cancel pending work without interrupting the sentence already playing.
    void DiscardPending() {
        PrismaUIBridge::BumpDialogueStopGeneration();
        for (const auto* type : {"HTTPStream", "HTTPStreamRechat", "HTTPStreamGodMode", "DirectorAction", "CombatBark", "CombatBarkStart"})
            ThreadPool::getInstance().cancelTasksByType(type);
        DirectorScene::Cancel();
        SPGResponse::getInstance().clearGameOutput();
        SpeakManager::getInstance().discardPendingInteraction();
    }

    void Sync(bool toggle, std::optional<bool> requested = {}) {
        if (syncing.load()) return;
        std::lock_guard lock(syncMutex);
        if (syncing || (!toggle && known && !failed)
            || (!toggle && std::chrono::steady_clock::now() < nextAttempt)) return;
        const bool desired = requested.value_or(!enabled.load());
        if (toggle && known && !failed && desired == enabled.load()) return;
        syncing = true;
        failed = false;
        if (toggle) {
            pendingState = desired;
            enabled = false;
            DiscardPending();
        }
        PrismaUIBridge::UpdateChimInteractionState();
        const auto target = pendingState;
        ThreadPool::getInstance().enqueue("ChimInteractionSync", [target]() {
            auto response = HTTPManager::postGameDataJson("chim_interaction.php", nlohmann::json::object(), 5000);
            if (target && response.is_object() && response.contains("generation") && response["generation"].is_number_unsigned()) {
                response = HTTPManager::postGameDataJson("chim_interaction.php",
                    {{"enabled", *target}, {"generation", response["generation"]}}, 5000);
            }
            {
                std::lock_guard lock(syncMutex);
                if (response.is_object() && response.contains("enabled") && response["enabled"].is_boolean()
                    && response.contains("generation") && response["generation"].is_number_unsigned()) {
                    generation = response["generation"].get<std::uint64_t>();
                    enabled = response["enabled"].get<bool>();
                    known = true;
                    pendingState.reset();
                } else {
                    enabled = false;
                    failed = true;
                }
                syncing = false;
                nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            PrismaUIBridge::UpdateChimInteractionState();
        });
    }
}
bool Enabled() { return enabled.load(); }
bool Syncing() { return syncing.load(); }
bool Failed() { return failed.load(); }
std::uint64_t Generation() { return generation.load(); }
void Synchronize() { Sync(false); }
void Toggle() { Sync(true); }
void SetEnabled(bool value) { Sync(true, value); }

bool IsTrigger(std::string_view message) {
    const auto type = message.substr(0, message.find('|'));
    return type.starts_with("diary") || type.starts_with("player_menu_tts_")
        || type == "inputtext" || type == "inputtext_s" || type == "ginputtext" || type == "ginputtext_s"
        || type == "narrator_inputtext" || type == "bored" || type == "rechat" || type == "continue"
        || type == "continue_group" || type == "instruction" || type == "suggestion" || type == "narration"
        || type == "narrator_welcome" || type == "combatbark" || type == "just_say" || type == "cheatmode"
        || type == "vision" || type == "force_current_task" || type == "recover_last_task";
}

bool IsGameOutput(std::string_view channel) {
    return channel == "ScriptQueue" || channel == "command" || channel == "rolecommand"
        || channel == "approvedcommand" || channel == "confirmcommand";
}
}
