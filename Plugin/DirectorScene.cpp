#include "ChimInteraction.h"
#include "Globals.h"
#include "DirectorScene.h"
#include "Commands.h"
#include "HTTPManager.h"
#include "Misc.h"
#include "SPGResponse.h"
#include "SpeakManager.h"
#include "ThreadPool.h"
#include "json.hpp"
#include <atomic>
#include <mutex>
#include <set>
#include <deque>

namespace DirectorScene {
namespace {
    std::atomic<std::uint64_t> generation{0};
    std::atomic<int> remaining{0};
    std::mutex sceneMutex;
    std::set<std::string> accepted;
    std::chrono::steady_clock::time_point pendingUntil{};
    std::deque<std::pair<std::uint64_t, nlohmann::json>> pendingCommands;
    thread_local bool dispatchingAction = false;
    std::atomic<bool> dispatchInProgress{false};

    void ReportAborted(const nlohmann::json& lines) {
        nlohmann::json ids = nlohmann::json::array();
        for (const auto& line : lines) {
            if (line.contains("utterance_id") && line["utterance_id"].is_string()) ids.push_back(line["utterance_id"]);
        }
        HTTPManager::log(std::format("_speech_abort|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
            nlohmann::json{{"utterance_ids", ids}, {"reason", "director_cancelled"}}.dump()));
    }
}
std::uint64_t Generation() { return generation.load(); }
bool Active() {
    std::lock_guard lock(sceneMutex);
    return remaining.load() > 0 || !pendingCommands.empty() || dispatchInProgress || std::chrono::steady_clock::now() < pendingUntil;
}
std::uint64_t BeginRequest() {
    std::lock_guard lock(sceneMutex);
    pendingUntil = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    return generation.load();
}
void RequestFailed(std::uint64_t token) {
    std::lock_guard lock(sceneMutex);
    if (token == generation.load()) pendingUntil = {};
}
void Cancel() { std::lock_guard lock(sceneMutex); ++generation; remaining = 0; pendingUntil = {}; pendingCommands.clear(); }
bool ReadyToSpeak() { std::lock_guard lock(sceneMutex); return pendingCommands.empty() && !dispatchInProgress; }
bool IsDispatchingAction() { return dispatchingAction; }

// Use the normal command handlers in authored order before starting another turn.
void ProcessActions() {
    if (!ChimInteraction::Enabled()) { Cancel(); return; }
    for (;;) {
        std::pair<std::uint64_t, nlohmann::json> next;
        {
            std::lock_guard lock(sceneMutex);
            if (pendingCommands.empty()) return;
            next = std::move(pendingCommands.front());
            pendingCommands.pop_front();
            dispatchInProgress = true;
        }
        if (next.first != Generation()) { dispatchInProgress = false; continue; }
        dispatchingAction = true;
        try {
            const auto channel = next.second.at("channel").get<std::string>();
            auto text = next.second.at("text").get<std::string>();
            if (channel == "approvedcommand") text = "__CHIM_APPROVED__" + text;
            if (channel == "rolecommand") parseRoleCommand(text);
            else parseCommand(text, next.second.at("actor"));
        } catch (const std::exception& error) {
            logger::warn("[DIRECTOR] Action could not start: {}", error.what());
        }
        dispatchingAction = false;
        dispatchInProgress = false;
    }
}

// Validate the whole payload before adding any speech to the ordinary playback queue.
void Queue(const std::string& encoded) {
    auto scene = nlohmann::json::parse(HTTPManager::base64_decode(encoded), nullptr, false);
    if (!scene.is_object()) return;
    try {
        if (scene.value("schema", "") != "chim.director_scene.v1") return;
        const auto id = scene.at("id").get<std::string>();
        const auto token = scene.at("generation").get<std::uint64_t>();
        const auto& lines = scene.at("lines");
        if (!lines.is_array() || lines.empty() || lines.size() > 5 || id.size() != 32) return;
        std::lock_guard lock(sceneMutex);
        if (accepted.contains(id)) return;
        if (token != Generation()) { ReportAborted(lines); return; }
        pendingUntil = {};
        std::vector<ScriptLine> speech;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto& line = lines[i];
            const auto speaker = line.at("speaker").get<std::string>();
            auto agent = AIAgentManager::getInstance().getAgentByName(speaker);
            if (!agent || !agent->getActor() || agent->isNarrator()) { ReportAborted(lines); return; }
            const auto ref = line.value("actor_refid", "");
            if (!ref.empty() && agent->getActor()->GetFormID() != std::stoul(ref, nullptr, 16)) {
                ReportAborted(lines); return;
            }
            ScriptLine queued(line.at("text"), "", line.at("listener"), "", speaker, "", 1.0f, -1,
                              "explicit_disable_rechat", line.at("utterance_id"));
            queued.directorSceneId = id;
            queued.directorGeneration = token;
            queued.directorLine = static_cast<int>(i + 1);
            queued.directorHasActions = std::any_of(scene.at("actions").begin(), scene.at("actions").end(),
                [i](const auto& action) { return action.at("after_line") == i + 1; });
            queued.ttsCacheKey = line.at("tts_cache_key");
            if (queued.subtitle.empty() || queued.subtitle.size() > 2400 || queued.ttsCacheKey.size() != 32
                || queued.ttsCacheKey.find_first_not_of("0123456789abcdef") != std::string::npos) {
                ReportAborted(lines); return;
            }
            speech.push_back(std::move(queued));
        }
        accepted.insert(id);
        if (accepted.size() > 128) accepted.erase(accepted.begin());
        remaining += static_cast<int>(speech.size());
        for (const auto& line : speech) SpeakManager::getInstance().insertInQueue(line);
        logger::info("[DIRECTOR] Queued scene {} with {} turns", id, speech.size());
    } catch (const std::exception& error) {
        logger::warn("[DIRECTOR] Rejected scene: {}", error.what());
        if (scene.contains("lines") && scene["lines"].is_array()) ReportAborted(scene["lines"]);
    }
}

// Both initial dispatch and explicit approval consume stored actions through the server adapter.
static void DispatchActions(const ScriptLine& line, int approvedIndex = -1) {
    if (!ChimInteraction::Enabled() || line.directorGeneration != Generation()) return;
    try {
        nlohmann::json request{{"scene_id", line.directorSceneId}, {"after_line", line.directorLine}, {"gamets", GetGameTimeStamp()}};
        if (approvedIndex >= 0) request["approved_action"] = approvedIndex;
        auto result = HTTPManager::postGameDataJson("director_scene_action.php", request, 15000);
        if (!ChimInteraction::Enabled() || line.directorGeneration != Generation()) return;
        if (result.is_object() && result.contains("commands") && result["commands"].is_array()) {
            for (const auto& command : result["commands"]) {
                const auto channel = command.value("channel", "");
                const auto text = command.value("text", "");
                const auto actor = command.value("actor", "");
                if (channel == "command" || channel == "rolecommand" || channel == "approvedcommand") {
                    std::lock_guard lock(sceneMutex);
                    if (line.directorGeneration == generation.load()) pendingCommands.emplace_back(line.directorGeneration, command);
                }
                else if (channel == "directorconfirm") {
                    request["approved_action"] = command.at("action_index");
                    request["generation"] = line.directorGeneration;
                    const auto payload = request.dump();
                    const auto encoded = HTTPManager::base64_encode(payload.c_str(), payload.size());
                    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                    SPGResponse::getInstance().enqueue("confirmcommand", {text + "@__DIRECTOR_SCENE__" + encoded, now, actor});
                }
            }
        }
    } catch (const std::exception& error) {
        logger::warn("[DIRECTOR] Could not dispatch turn {}: {}", line.directorLine, error.what());
    }
}

bool ApproveAction(const std::string& command) {
    const auto marker = command.find("@__DIRECTOR_SCENE__");
    if (marker == std::string::npos) return false;
    try {
        const auto request = nlohmann::json::parse(HTTPManager::base64_decode(command.substr(marker + 19)));
        ScriptLine line("", "", "", "", "", "");
        line.directorSceneId = request.at("scene_id");
        line.directorGeneration = request.at("generation");
        line.directorLine = request.at("after_line");
        const int index = request.at("approved_action");
        ThreadPool::getInstance().enqueue("DirectorAction", [line, index]() { DispatchActions(line, index); });
    } catch (const std::exception& error) {
        logger::warn("[DIRECTOR] Invalid action approval: {}", error.what());
    }
    return true;
}

// Wait only for server command preparation, never for the resulting game action to finish.
void CompleteLine(const ScriptLine& line, bool spoken) {
    if (line.directorSceneId.empty() || line.directorGeneration != Generation()) return;
    if (spoken && line.directorHasActions) DispatchActions(line);
    else if (!spoken) ReportAborted(nlohmann::json::array({{{"utterance_id", line.utteranceId}}}));
    std::lock_guard lock(sceneMutex);
    if (line.directorGeneration == generation.load() && remaining > 0) --remaining;
}
}
