#include "PlaythroughSession.h"
#include "HTTPManager.h"
#include "Misc.h"
#include "PrismaUIBridge.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

namespace PlaythroughSession {
namespace {
    std::atomic<std::uint64_t> generation{1};
    std::atomic<std::uint64_t> started{0};
    std::atomic<bool> ready{false};
    thread_local std::uint64_t context = 0;
    std::mutex stateMutex;
    std::string character, token;
    bool newCharacter = false;

    std::string NewId() {
        std::random_device random;
        constexpr char hex[] = "0123456789abcdef";
        std::string id(32, '0');
        for (char& c : id) c = hex[random() & 15];
        return id;
    }
    const std::string clientId = NewId();
}

std::uint64_t Generation() { return generation.load(); }
std::uint64_t Context() { return context ? context : Generation(); }
Scope::Scope(std::uint64_t value) : previous(context) { context = value; }
Scope::~Scope() { context = previous; }
bool Allowed(std::uint64_t value) { return ready.load() && value == Generation(); }
std::string Header(std::uint64_t value) {
    std::lock_guard lock(stateMutex);
    // Even a revoked request is tagged, so it cannot fall through a browser-only endpoint.
    if (!Allowed(value)) return "X-CHIM-Playthrough: blocked\r\n";
    if (token.empty()) return "X-CHIM-Playthrough: legacy\r\n";
    return "X-CHIM-Playthrough: " + token + "\r\n";
}
void ResetCharacter() { std::lock_guard lock(stateMutex); character.clear(); newCharacter = false; }
void BeginLoad() {
    {
        // Serializes with handshake publication, so an older load's accepted result cannot mark this one ready.
        std::lock_guard lock(stateMutex);
        ready = false;
        ++generation;
        token.clear();
        character.clear();
        newCharacter = false;
    }
    PrismaUIBridge::BumpDialogueStopGeneration();
}
std::string Character(bool createIfMissing) {
    std::lock_guard lock(stateMutex);
    if (character.empty() && createIfMissing) character = NewId();
    return character;
}
bool NewCharacter() { std::lock_guard lock(stateMutex); return newCharacter; }
void RestoreCharacter(const std::string& value, bool isNew) {
    if (value.size() != 32 || value.find_first_not_of("0123456789abcdef") != std::string::npos) return;
    std::lock_guard lock(stateMutex);
    character = value;
    newCharacter = isNew;
}

// Keep restore I/O off Skyrim's thread; only resume initialization for this exact load.
void Connect(std::function<void()> resume, bool newGame) {
    const auto epoch = Generation();
    const auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    if (started.exchange(epoch) == epoch) return;
    std::string identity;
    { std::lock_guard lock(stateMutex); identity = character; newCharacter = newCharacter || newGame; newGame = newCharacter; }
    const nlohmann::json request = {{"client_id",clientId}, {"load_id",epoch},
        {"character_id",identity}, {"new_game",newGame}, {"player_name",player->GetName()},
        {"gamets",GetGameTimeStamp()}};
    std::thread([epoch, request, resume = std::move(resume)]() {
        const Scope scope(epoch);
        nlohmann::json result;
        std::optional<std::chrono::steady_clock::time_point> busyDeadline;
        int transportFailures = 0;
        // Sleeps in short ticks so a newer load cancels promptly; false means this load is stale.
        const auto wait = [epoch](int ticks) {
            for (int tick = 0; tick < ticks; ++tick) {
                if (epoch != Generation()) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return epoch == Generation();
        };
        for (;;) {
            if (epoch != Generation()) return;
            result = HTTPManager::requestPlaythroughSession(request);
            if (epoch != Generation()) return;
            if (result.value("ok",false)) break;
            const auto status = result.value("status",std::string{});
            if (status == "transport_error") {
                // The server may start after the save loads, so keep this load pending until it answers.
                // Only the first retry is immediate; later ones wait so an offline server is not flooded.
                if (++transportFailures == 2) logger::warn("[Playthrough] Server unreachable; retrying handshake every 5 seconds");
                busyDeadline.reset();
                if (transportFailures > 1 && !wait(50)) return;
                continue;
            }
            if (status != "busy") break;
            // Retry only pre-switch contention, retaining this load's identity and cancellation scope.
            // The budget starts at the first busy reply so time spent waiting for the server is not charged to it.
            if (!busyDeadline) busyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            else if (std::chrono::steady_clock::now() >= *busyDeadline) break;
            if (!wait(10)) return;
            if (std::chrono::steady_clock::now() >= *busyDeadline) break;
        }
        if (transportFailures > 1) logger::info("[Playthrough] Server answered handshake after {} transport failures", transportFailures);
        if (epoch != Generation()) return;
        const bool accepted = result.value("ok",false);
        if (accepted) {
            std::lock_guard lock(stateMutex);
            if (epoch != Generation()) return;
            token = result.value("token",std::string{});
            const auto id = result.value("character_id",std::string{});
            if (id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos) character = id;
            if (result.value("status",std::string{}) == "ready") newCharacter = false;
            ready = true;
        }
        SKSE::GetTaskInterface()->AddTask([epoch, accepted, result, resume]() {
            if (epoch != Generation()) return;
            const Scope scope(epoch);
            if (accepted) resume();
            const auto message = result.value("message",std::string{});
            if (!message.empty()) RE::DebugNotification(("[CHIM] " + message).c_str());
            else if (!accepted) RE::DebugNotification("[CHIM] Playthrough unavailable. Open Playthrough Saves, then reload this save.");
        });
    }).detach();
}
}
