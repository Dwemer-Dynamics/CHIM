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
#include <map>

namespace DirectorScene {
namespace {
    std::atomic<std::uint64_t> generation{0};
    std::atomic<int> remaining{0};
    std::mutex sceneMutex;
    std::set<std::string> accepted;
    struct SceneProgress {
        std::set<std::string> pending;
        bool failed = false;
    };
    std::map<std::string, SceneProgress> scenes;
    std::chrono::steady_clock::time_point pendingUntil{};
    std::deque<std::pair<std::uint64_t, nlohmann::json>> pendingCommands;
    thread_local bool dispatchingAction = false;
    std::atomic<bool> dispatchInProgress{false};

    // The scene ends after its speech and immediate action dispatch, without waiting for game actions.
    void FinishScenes() {
        if (!pendingCommands.empty() || dispatchInProgress) return;
        for (auto scene = scenes.begin(); scene != scenes.end();) {
            if (!scene->second.pending.empty()) { ++scene; continue; }
            SKSE::GetTaskInterface()->AddTask([] { RE::DebugNotification("[CHIM] Director scene stopped."); });
            scene = scenes.erase(scene);
        }
    }

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
void Cancel() {
    std::lock_guard lock(sceneMutex);
    for (std::size_t i = 0; i < scenes.size(); ++i) {
        SKSE::GetTaskInterface()->AddTask([] { RE::DebugNotification("[CHIM] Director scene stopped."); });
    }
    scenes.clear();
    ++generation;
    remaining = 0;
    pendingUntil = {};
    pendingCommands.clear();
}
bool ReadyToSpeak() { std::lock_guard lock(sceneMutex); return pendingCommands.empty() && !dispatchInProgress; }
bool IsDispatchingAction() { return dispatchingAction; }

// Use the normal command handlers in authored order before starting another turn.
void ProcessActions() {
    if (!ChimInteraction::Enabled()) { Cancel(); return; }
    for (;;) {
        std::pair<std::uint64_t, nlohmann::json> next;
        {
            std::lock_guard lock(sceneMutex);
            if (pendingCommands.empty()) { FinishScenes(); return; }
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
            
            logger::error("[DIRECTOR] Action could not start (exception): {}", error.what());
        }
        dispatchingAction = false;
        dispatchInProgress = false;
    }
}

// Validate the whole payload before adding any speech to the ordinary playback queue.
void Queue(const std::string& encoded) {
    auto scene = nlohmann::json::parse(HTTPManager::base64_decode(encoded), nullptr, false);
    if (!scene.is_object()) { logger::error("[DIRECTOR] Queue: parsed payload is not a JSON object"); return; }
    try {
        const auto schema = scene.value("schema", "");
        const bool chunked = schema == "chim.director_scene.v2";
        if (!chunked && schema != "chim.director_scene.v1") { logger::error("[DIRECTOR] Queue: unsupported schema '{}'", schema); return; }
        const auto id = scene.at("id").get<std::string>();
        const auto token = scene.at("generation").get<std::uint64_t>();
        const auto& lines = scene.at("lines");
        if (!lines.is_array() || lines.empty() || lines.size() > (chunked ? 128u : 5u) || id.size() != 32) {
            logger::error("[DIRECTOR] Queue: invalid 'lines' or 'id' (lines.is_array: {}, count: {}, id.length: {})",
                lines.is_array(), lines.size(), id.size());
            return; 
        }
        std::lock_guard lock(sceneMutex);
        if (accepted.contains(id)) { logger::warn("[DIRECTOR] Queue: duplicate scene id '{}', already accepted", id); return; }
        if (token != Generation()) { 
            logger::warn("[DIRECTOR] Queue: generation mismatch (scene.gen: {}, current.gen: {})", token, Generation());
            ReportAborted(lines); 
            return; 
        }
        pendingUntil = {};
        std::vector<ScriptLine> speech;
        std::set<std::string> utterances;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto& line = lines[i];
            const auto speaker = line.at("speaker").get<std::string>();
            auto agent = AIAgentManager::getInstance().getAgentByName(speaker);
            if (!agent || !agent->getActor() || agent->isNarrator()) { 
                logger::error("[DIRECTOR] Queue: invalid speaker '{}' (missing agent or narrator)", speaker);
                ReportAborted(lines); 
                continue;
            }
            const auto ref = line.value("actor_refid", "");
            if (!ref.empty() && agent->getActor()->GetFormID() != std::stoul(ref, nullptr, 16)) {
                logger::error("[DIRECTOR] Queue: actor_refid mismatch for speaker '{}' (expected {}, actual {:X})",
                    speaker, ref, agent->getActor()->GetFormID());
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
            if (queued.utteranceId.empty() || !utterances.insert(queued.utteranceId).second
                || queued.subtitle.empty() || queued.subtitle.size() > 2400 || queued.ttsCacheKey.size() != 32
                || queued.ttsCacheKey.find_first_not_of("0123456789abcdef") != std::string::npos) {
                logger::error("[DIRECTOR] Queue: invalid utterance/tts metadata at line {} (utteranceId empty: {}, duplicate: {}, subtitle.len: {}, ttsKey.len: {})",
                    i + 1, queued.utteranceId.empty(), !utterances.insert(queued.utteranceId).second, queued.subtitle.size(), queued.ttsCacheKey.size());
                ReportAborted(lines); return;
            }
            speech.push_back(std::move(queued));
        }
        accepted.insert(id);
        if (accepted.size() > 128) accepted.erase(accepted.begin());
        remaining += static_cast<int>(speech.size());
        for (const auto& line : speech) scenes[id].pending.insert(line.utteranceId);
        SKSE::GetTaskInterface()->AddTask([] { RE::DebugNotification("[CHIM] Director scene started."); });
        for (const auto& line : speech) SpeakManager::getInstance().insertInQueue(line);
    } catch (const std::exception& error) {
        
        logger::error("[DIRECTOR] Rejected scene (exception): {}", error.what());
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
                } else {
                    
                    logger::error("[DIRECTOR] DispatchActions: unexpected command channel '{}' for scene {} line {}", channel, line.directorSceneId, line.directorLine);
                }
            }
        } else {
            
            logger::error("[DIRECTOR] DispatchActions: expected 'commands' array in response for scene {} after_line {}", line.directorSceneId, line.directorLine);
        }
    } catch (const std::exception& error) {
        
        logger::error("[DIRECTOR] Could not dispatch turn (exception) {}: {}", line.directorLine, error.what());
    }
}

bool ApproveAction(const std::string& command) {
    const auto marker = command.find("@__DIRECTOR_SCENE__");
    if (marker == std::string::npos) { logger::error("[DIRECTOR] ApproveAction: missing director scene marker in command"); return false; }
    try {
        const auto request = nlohmann::json::parse(HTTPManager::base64_decode(command.substr(marker + 19)));
        ScriptLine line("", "", "", "", "", "");
        line.directorSceneId = request.at("scene_id");
        line.directorGeneration = request.at("generation");
        line.directorLine = request.at("after_line");
        const int index = request.at("approved_action");
        ThreadPool::getInstance().enqueue("DirectorAction", [line, index]() { DispatchActions(line, index); });
    } catch (const std::exception& error) {
        
        logger::error("[DIRECTOR] Invalid action approval (exception): {}", error.what());
    }
    return true;
}

// Wait only for server command preparation, never for the resulting game action to finish.
void CompleteLine(const ScriptLine& line, bool spoken) {
    if (line.directorSceneId.empty() || line.directorGeneration != Generation()) { 
        logger::error("[DIRECTOR] CompleteLine: invalid input or generation mismatch (scene_id empty: {}, line.gen: {}, current.gen: {})",
            line.directorSceneId.empty(), line.directorGeneration, Generation());
        return; 
    }
    bool failed = !spoken;
    {
        std::lock_guard lock(sceneMutex);
        const auto scene = scenes.find(line.directorSceneId);
        if (scene == scenes.end()) { logger::error("[DIRECTOR] CompleteLine: scene '{}' not found", line.directorSceneId); return; }
        scene->second.failed |= failed;
        failed = scene->second.failed;
    }
    if (spoken && !failed && line.directorHasActions) DispatchActions(line);
    else if (!spoken) ReportAborted(nlohmann::json::array({{{"utterance_id", line.utteranceId}}}));
    std::lock_guard lock(sceneMutex);
    if (line.directorGeneration != generation.load()) return;
    const auto scene = scenes.find(line.directorSceneId);
    if (scene == scenes.end() || !scene->second.pending.erase(line.utteranceId)) {
        logger::error("[DIRECTOR] CompleteLine: could not remove utterance '{}' from scene '{}'", line.utteranceId, line.directorSceneId);
        return;
    }
    if (remaining > 0) --remaining;
    scene->second.failed |= !spoken;
    FinishScenes();
}
}
