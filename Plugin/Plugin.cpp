#include <SKSE/Events.h>
#include <SkyrimScripting/Plugin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ThreadPool.h"

#include "Commands.h"
#include "DynamicDiaryBook.h"
#include "Globals.h"
#include "Conf.h"
#include "Misc.h"
#include "Papyrus.h"
#include "RE/Skyrim.h"
#include "RE/T/TESDataHandler.h"
#include "RE/B/BarterMenu.h"
#include "RE/B/BSUIMessageData.h"
#include "RE/B/BSUIScaleformData.h"
#include "RE/D/DialogueMenu.h"
#include "RE/G/GFxEvent.h"
#include "RE/U/UI.h"
#include "RE/U/UIMessageQueue.h"
#include "Replacements.h"
#include "json.hpp"
#include "SpeakManager.h"
#include "HTTPManager.h"
#include "HTTPUploader.h"
#include "SPGResponse.h"
#include "AudioManager.h"
#include "md5.h"
#include "PrismaUIBridge.h"
#include "ResourceFileReader.h"
#include "SpatialAwareness.h"
#include "SpatialSnapshotManager.h"
#include "VRItemAwareness.h"
#include "ServerPluginSync.h"

using json = nlohmann::json;

#define PLUGIN_VERSION "3.2.4"
#define PLUGIN_RELEASE_DATE "2026-08-13"

const char* GetPluginVersion()
{
    return PLUGIN_VERSION;
}

static void AddCachedSpeechAudience(json& speechPayload, const std::string& reason);

/* Plugin Globals */

extern void ProcedureTakeShot();
extern void ProcedureSendShot(const char* a_path);

extern void addAllNPC();
extern bool promoteCrosshairTargetToAI();
extern void PollPlayerGaze();  // HerikaEyes.cpp - SHARMAT gaze/staring detection
extern int setDrivenByAIReal(RE::ObjectRefHandle targetObject, bool salutation, bool warn, bool removewhenexisting, bool isManualAdd);



std::chrono::high_resolution_clock::time_point controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();
namespace
{
    std::int64_t ToPlayerSpeechSuppressTicks(std::chrono::high_resolution_clock::time_point value)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
    }

}

static std::atomic<std::int64_t> controlPlayerSpeechSuppressUntilTicks{
    ToPlayerSpeechSuppressTicks(std::chrono::high_resolution_clock::now())};
static std::atomic<std::int64_t> controlWorldMaintenanceSuppressUntilTicks{
    ToPlayerSpeechSuppressTicks(std::chrono::high_resolution_clock::now())};

void ExtendPlayerSpeechMaintenanceSuppress(std::chrono::milliseconds duration)
{
    if (duration <= std::chrono::milliseconds(0)) {
        return;
    }

    const auto untilTicks =
        ToPlayerSpeechSuppressTicks(std::chrono::high_resolution_clock::now() + duration);
    auto current = controlPlayerSpeechSuppressUntilTicks.load(std::memory_order_acquire);
    while (current < untilTicks &&
           !controlPlayerSpeechSuppressUntilTicks.compare_exchange_weak(
               current, untilTicks, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
}

bool IsPlayerSpeechMaintenanceSuppressed()
{
    const auto nowTicks = ToPlayerSpeechSuppressTicks(std::chrono::high_resolution_clock::now());
    return nowTicks < controlPlayerSpeechSuppressUntilTicks.load(std::memory_order_acquire);
}

static void ExtendWorldMaintenanceSuppress(std::chrono::milliseconds duration)
{
    if (duration <= std::chrono::milliseconds(0)) {
        return;
    }

    const auto untilTicks =
        ToPlayerSpeechSuppressTicks(std::chrono::high_resolution_clock::now() + duration);
    auto current = controlWorldMaintenanceSuppressUntilTicks.load(std::memory_order_acquire);
    while (current < untilTicks &&
           !controlWorldMaintenanceSuppressUntilTicks.compare_exchange_weak(
               current, untilTicks, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
}

static bool IsWorldMaintenanceSuppressed()
{
    const auto nowTicks = ToPlayerSpeechSuppressTicks(std::chrono::high_resolution_clock::now());
    return nowTicks < controlWorldMaintenanceSuppressUntilTicks.load(std::memory_order_acquire);
}

static bool IsActorLoadedInPlayerCell(RE::Actor* actor)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!actor || !player || actor->IsDead() || !actor->Is3DLoaded() ||
        !actor->GetActorRuntimeData().currentProcess) {
        return false;
    }

    auto* actorCell = actor->GetParentCell();
    auto* playerCell = player->GetParentCell();
    return actorCell && playerCell && actorCell == playerCell;
}

static std::chrono::high_resolution_clock::time_point controlLastCombatEndTS = std::chrono::high_resolution_clock::now();
static std::chrono::high_resolution_clock::time_point controlLastLockPickedTS = std::chrono::high_resolution_clock::now();
static std::chrono::high_resolution_clock::time_point controlLastBleedOutTriggerTS = std::chrono::high_resolution_clock::now();
static std::chrono::high_resolution_clock::time_point controlLastInfoSent = std::chrono::high_resolution_clock::now();
static std::chrono::high_resolution_clock::time_point controlLastDynamicProfileTS = std::chrono::high_resolution_clock::now();
static std::chrono::high_resolution_clock::time_point controlLastWalkToTargetCheckTS = std::chrono::high_resolution_clock::now();
static std::unordered_map<uint32_t, std::chrono::high_resolution_clock::time_point> lastReanimateEventByTarget;
static bool playerPartyCombatActive = false;
static std::mutex spatialDoorInvalidationMutex;
static bool spatialDoorInvalidationQueued = false;
static std::uint32_t spatialDoorInvalidationCoalescedEvents = 0;
static RE::FormID spatialDoorInvalidationLastDoor = 0;

static bool QueueCoalescedDoorInvalidation(RE::FormID doorFormId)
{
    bool shouldQueue = false;
    {
        std::lock_guard<std::mutex> lock(spatialDoorInvalidationMutex);
        ++spatialDoorInvalidationCoalescedEvents;
        spatialDoorInvalidationLastDoor = doorFormId;
        if (!spatialDoorInvalidationQueued) {
            spatialDoorInvalidationQueued = true;
            shouldQueue = true;
        }
    }

    if (!shouldQueue) {
        return false;
    }

    ThreadPool::getInstance().enqueue(
        "SpatialDoorInvalidation",
        []() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            std::uint32_t eventCount = 0;
            RE::FormID lastDoorFormId = 0;
            {
                std::lock_guard<std::mutex> lock(spatialDoorInvalidationMutex);
                eventCount = spatialDoorInvalidationCoalescedEvents;
                lastDoorFormId = spatialDoorInvalidationLastDoor;
                spatialDoorInvalidationCoalescedEvents = 0;
                spatialDoorInvalidationLastDoor = 0;
                spatialDoorInvalidationQueued = false;
            }

            SpatialAwareness::InvalidateCache();
            SpatialSnapshotManager::InvalidateDynamicSpatialState();
            logger::debug("[SpatialSnapshot] Coalesced door invalidation completed events={} lastDoor={:08X}",
                          eventCount, lastDoorFormId);
        },
        "coalesced",
        std::chrono::milliseconds(2000));

    return true;
}

static void InvalidateSpatialCachesForDoor(RE::TESObjectREFR* doorRef, const char* source)
{
    if (!doorRef) {
        return;
    }

    const auto doorFormId = doorRef->GetFormID();
    auto* doorCell = doorRef->GetParentCell();
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* playerCell = player ? player->GetParentCell() : nullptr;
    if (!doorCell || !playerCell || !doorCell->IsInteriorCell() || doorCell != playerCell) {
        return;
    }

    const bool queued = QueueCoalescedDoorInvalidation(doorFormId);
    if (queued) {
        // Door state affects refined audibility, not the cheap active-agent/air-distance list.
        // Do not enter world-settling here; that caused cadence freezes in busy interiors.
        SpatialAwareness::InvalidateCache();
        SpatialSnapshotManager::InvalidateDynamicSpatialState();
        logger::info("[SpatialSnapshot] Door activation queued spatial refinement refresh source={} door={:08X}",
                     source ? source : "unknown", doorFormId);
    }
}

static void QueueDelayedSendCellInfo(RE::FormID cellFormID)
{
    ThreadPool::getInstance().enqueue(
        "DelayedSendCellInfo",
        [cellFormID]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(4000));

            auto* taskInterface = SKSE::GetTaskInterface();
            if (!taskInterface) {
                logger::warn("[TESCellFullyLoadedEvent] Skipping delayed SendCellInfo; SKSE task interface unavailable cell <{:#x}>",
                             cellFormID);
                return;
            }

            taskInterface->AddTask([cellFormID]() {
                auto* game = RE::UI::GetSingleton();
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(cellFormID);
                if (!game || game->GameIsPaused() || !player || !cell) {
                    logger::trace("[TESCellFullyLoadedEvent] Skipping delayed SendCellInfo; game not settled cell <{:#x}>",
                                 cellFormID);
                    return;
                }

                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                auto args = RE::MakeFunctionArguments(std::move(cell));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "SendCellInfo", args, callback);
                logger::trace("[TESCellFullyLoadedEvent] Delayed SendCellInfo dispatch for cell <{:#x}>", cellFormID);
            });
        },
        std::format("{:08X}", cellFormID),
        std::chrono::milliseconds(20000));
}


static std::mutex makeShotMutex;

static std::atomic<bool> makeShot(false);

static std::mutex makeShotNativeMutex;
static std::atomic<bool> makeShotNative(false);


static std::mutex shotSendModeMutex;
static std::atomic<int> shotSendMode = 0;


static bool isGameReady = false;

static std::list<std::string> BackGroundDialogueQueue;
static std::list<std::string> pendindNotifications;

RE::TESForm* NullVoiceType = nullptr;

std::list<AudioFileBufferEntry> AudioFilesBufferManager::audioFilesBuffer;



std::string DialogueLastStringSay;
std::string DialogueLastStringResponse;

const long parameterMaxDistanceToListen = 1000000;
const int CLEANING_TIMEOUT = 4;

bool NewActionMode = false;

bool forceCombatEnd = false;


int GlobalBoredEventTimeOut=60;
int GlobalDynamicProfileTimeOut=1200; // 20 minutes default
int GlobalCombatBarksPeriod=30;
int GlobalEndConversationCooldown=60; // Default 60 seconds
static bool dynamicProfileLoggingInitialized = false;
static std::chrono::high_resolution_clock::time_point lastGameLoadTime = std::chrono::high_resolution_clock::now();
static const int GAME_LOAD_COOLDOWN_SECONDS = 60; // Cooldown between game loads to prevent timer abuse

bool inSexMode = false;
bool inSexDescSent = false;
int inSexLastStage = 0;

bool pluginInited = false;
bool pendingLoadedPluginManifestSync = false;
bool importDataDetectionDone = false;  // Track if we've already done import data detection this session

namespace
{
    constexpr auto kQuestProgressionPollInterval = std::chrono::seconds(2);
    constexpr auto kQuestProgressionStatusPollInterval = std::chrono::seconds(10);

    struct QuestActionExecutionResult
    {
        bool success{ false };
        std::string status{ "failed" };
        json result{ json::object() };
    };

    std::mutex g_questProgressionMutex;
    std::unordered_map<std::string, int> g_questProgressionLastStages;
    std::unordered_set<std::string> g_questProgressionDeadActors;
    std::unordered_map<std::string, int> g_questProgressionSuppressedAdds;
    std::unordered_map<std::string, int> g_questProgressionSuppressedRemovals;
    std::unordered_set<std::int64_t> g_questProgressionActionsInFlight;
    RE::FormID g_questProgressionLastLocationFormID = 0;
    bool g_questProgressionResyncScheduled = false;
    bool g_questProgressionResyncIncludeInventory = true;
    std::chrono::steady_clock::time_point g_questProgressionResyncDueAt =
        std::chrono::steady_clock::time_point::max();
    std::chrono::steady_clock::time_point g_questProgressionLastPollAt =
        std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point g_questProgressionLastStatusPollAt =
        std::chrono::steady_clock::time_point::min();
}

void ResetQuestProgressionBridgeState();
void ScheduleQuestProgressionFullResync(const char* reason, int delayMs, bool includeInventory);
static void MaybeRunScheduledQuestProgressionResync();
static void PollQuestProgressionActions();
static void SyncQuestProgressionEnabledFromServer();


RE::TESFaction *AIAgentRoleMasterFaction = nullptr;
RE::TESFaction* AIAgentFollowFaction = nullptr;

namespace
{
    bool EqualsIgnoreCasePlugin(const std::string& left, const std::string& right)
    {
        if (left.size() != right.size()) {
            return false;
        }

        for (std::size_t i = 0; i < left.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(left[i])) !=
                std::tolower(static_cast<unsigned char>(right[i]))) {
                return false;
            }
        }

        return true;
    }

    bool IsPlayerActorName(const std::string& actorName)
    {
        const std::string normalizedActorName = trim(actorName);
        if (normalizedActorName.empty()) {
            return false;
        }

        if (EqualsIgnoreCasePlugin(normalizedActorName, NARRATOR_NAME)) {
            return false;
        }

        if (EqualsIgnoreCasePlugin(normalizedActorName, "Player")) {
            return true;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            const std::string playerName = trim(player->GetName());
            if (!playerName.empty() && EqualsIgnoreCasePlugin(normalizedActorName, playerName)) {
                return true;
            }

            const std::string playerDisplayName = trim(player->GetDisplayFullName());
            if (!playerDisplayName.empty() &&
                !EqualsIgnoreCasePlugin(playerDisplayName, NARRATOR_NAME) &&
                EqualsIgnoreCasePlugin(normalizedActorName, playerDisplayName)) {
                return true;
            }
        }

        const std::string configuredPlayerName = trim(AIAgentManager::getInstance().getPlayerName());
        return !configuredPlayerName.empty() && EqualsIgnoreCasePlugin(normalizedActorName, configuredPlayerName);
    }

    std::string BuildLoadedPluginPrefix(const RE::TESFile* file)
    {
        if (!file) {
            return "";
        }

        if (file->IsLight()) {
            return std::format("FE{:03X}", static_cast<std::uint32_t>(file->GetSmallFileCompileIndex()) & 0x0FFF);
        }

        return std::format("{:02X}", static_cast<std::uint32_t>(file->GetCompileIndex()) & 0xFF);
    }

    void AppendLoadedPluginManifestEntry(json& pluginRows, std::set<std::string>& seenPluginNames, const RE::TESFile* file)
    {
        if (!file) {
            return;
        }

        const std::string pluginName(file->GetFilename());
        if (pluginName.empty()) {
            return;
        }

        const std::string seenKey = pluginName;
        if (!seenPluginNames.insert(seenKey).second) {
            return;
        }

        const std::string formIdPrefix = BuildLoadedPluginPrefix(file);
        if (formIdPrefix.empty()) {
            return;
        }

        pluginRows.push_back({
            {"plugin_name", pluginName},
            {"is_light", file->IsLight()},
            {"compile_index", static_cast<std::uint32_t>(file->GetCompileIndex())},
            {"small_file_compile_index", static_cast<std::uint32_t>(file->GetSmallFileCompileIndex())},
            {"partial_index", static_cast<std::uint32_t>(file->GetPartialIndex())},
            {"formid_prefix", formIdPrefix}
        });
    }

    bool PostLoadedPluginManifest()
    {
        logger::info("[LOADED_PLUGINS] Preparing plugin manifest sync");

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            logger::warn("[LOADED_PLUGINS] TESDataHandler unavailable - skipping plugin manifest sync");
            return false;
        }

        json payload;
        payload["type"] = "loaded_plugins";
        payload["plugins"] = json::array();

        std::set<std::string> seenPluginNames;

        if (REL::Module::IsVR()) {
            if (auto* loadedMods = dataHandler->GetLoadedMods()) {
                const auto loadedModCount = dataHandler->GetLoadedModCount();
                for (std::uint32_t i = 0; i < loadedModCount; ++i) {
                    AppendLoadedPluginManifestEntry(payload["plugins"], seenPluginNames, loadedMods[i]);
                }
            }
        } else {
            const auto& fileCollection = REL::RelocateMember<const RE::TESFileCollection>(dataHandler, 0xD70, 0);
            for (auto* file : fileCollection.files) {
                AppendLoadedPluginManifestEntry(payload["plugins"], seenPluginNames, file);
            }

            for (auto* file : fileCollection.smallFiles) {
                AppendLoadedPluginManifestEntry(payload["plugins"], seenPluginNames, file);
            }
        }

        if (payload["plugins"].empty()) {
            logger::warn("[LOADED_PLUGINS] No plugins discovered for manifest sync");
            return false;
        }

        HTTPManager::postGameData("gamedata.php", payload);
        logger::info("[LOADED_PLUGINS] Synced {} loaded plugins", payload["plugins"].size());
        return true;
    }

    std::string NormalizeServerVersionForCompare(std::string serverVersion)
    {
        serverVersion = trim(serverVersion);
        if (serverVersion.size() >= 3) {
            std::string suffix = serverVersion.substr(serverVersion.size() - 3);
            std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (suffix == "dev") {
                serverVersion = trim(serverVersion.substr(0, serverVersion.size() - 3));
            }
        }
        return serverVersion;
    }

    void ScheduleVersionMismatchStartupCheck()
    {
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(10));

            const std::string rawServerVersion = HTTPManager::getServerVersionRaw();
            if (rawServerVersion.empty()) {
                logger::info("[VersionCheck] Server version unavailable, skipping mismatch warning");
                return;
            }

            const std::string normalizedServerVersion = NormalizeServerVersionForCompare(rawServerVersion);
            const std::string pluginVersion = PLUGIN_VERSION;

            if (normalizedServerVersion.empty()) {
                logger::error("[VersionCheck] Server version was empty after normalization");
                return;
            }

            if (pluginVersion == normalizedServerVersion) {
                logger::info("[VersionCheck] Plugin and server versions match (plugin={}, serverRaw={})",
                             pluginVersion, rawServerVersion);
                return;
            }

            const std::string warning = std::format(
                "[CHIM] Version mismatch: Plugin {} / Server {}",
                pluginVersion,
                rawServerVersion);

            logger::warn("[VersionCheck] {}", warning);

            SKSE::GetTaskInterface()->AddTask([warning]() {
                RE::DebugNotification(warning.c_str());
            });
        }).detach();
    }

    constexpr auto kPlayerMenuTtsPrefetchCooldown = std::chrono::seconds(120);
    constexpr auto kPlayerMenuTtsPrefetchRetention = std::chrono::minutes(15);
    constexpr auto kPlayerMenuTtsPrefetchCleanupInterval = std::chrono::seconds(30);
    constexpr std::size_t kPlayerMenuTtsPrefetchMaxPerEvent = 6;
    constexpr auto kPlayerMenuSelectionCaptureBaselineDelay = std::chrono::milliseconds(50);
    constexpr auto kPlayerMenuSelectionCaptureTimeout = std::chrono::milliseconds(500);
    constexpr auto kPlayerMenuDialogueGateMaxHold = std::chrono::seconds(12);
    constexpr auto kPlayerMenuDialogueGateHoldPadding = std::chrono::seconds(4);
    constexpr auto kPlayerMenuDialogueGateEstimatedMin = std::chrono::milliseconds(1200);
    constexpr auto kPlayerMenuDialogueGateEstimatedMax = std::chrono::seconds(18);
    constexpr std::uint32_t kPlayerMenuReplayUserEventMarker = 0x504D5453;  // "PMTS"
    constexpr bool kPlayerMenuUseActorCommitGate = false;

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_playerMenuTtsPrefetchHistory;
    std::chrono::steady_clock::time_point g_playerMenuTtsLastCleanup = std::chrono::steady_clock::time_point::min();
    std::mutex g_playerMenuDialogueGateMutex;
    bool g_playerMenuDialogueGateActive = false;
    RE::FormID g_playerMenuDialogueGateSpeakerFormID = 0;
    std::chrono::steady_clock::time_point g_playerMenuDialogueGateMinRelease = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point g_playerMenuDialogueGateHardDeadline = std::chrono::steady_clock::time_point::min();
    std::mutex g_playerMenuDelayedSelectionMutex;
    bool g_playerMenuDelayedSelectionActive = false;
    std::string g_playerMenuDelayedUserEventName;
    std::chrono::steady_clock::time_point g_playerMenuDelayedSelectionStartedAt =
        std::chrono::steady_clock::time_point::min();
    RE::MenuTopicManager::Dialogue* g_playerMenuDelayedBaselineDialogue = nullptr;
    bool g_playerMenuDelayedSelectionPlaybackQueued = false;
    bool g_playerMenuReplayUserEventArmed = false;
    bool g_playerMenuReplayInFlight = false;
    std::string g_playerMenuReplayUserEventName;
    bool g_playerMenuDelayScaleformBlockLogged = false;
    std::mutex g_playerMenuDeferredCommitMutex;
    RE::FormID g_playerMenuDeferredCommitSpeakerFormID = 0;
    RE::DialogueResponse* g_playerMenuDeferredCommitResponse = nullptr;
    std::atomic<bool> g_playerMenuForceDeferredCommit{false};
    std::atomic<bool> g_playerMenuSkipNextTopicPlayback{false};
    std::atomic<bool> g_playerMenuReleaseTaskQueued{false};
    std::mutex g_playerMenuEventTimingMutex;
    std::chrono::steady_clock::time_point g_playerMenuLastSelectionUserEvent = std::chrono::steady_clock::time_point::min();
    std::string g_playerMenuLastInterceptedLine;
    std::chrono::steady_clock::time_point g_playerMenuLastInterceptedLineAt = std::chrono::steady_clock::time_point::min();

    struct PlayerMenuReplayUserEventData : RE::IUIMessageData
    {
        union Payload
        {
            bool b;
            std::uint32_t u;
            float f;
            void* p;
        };
        static_assert(sizeof(Payload) == 0x8);

        PlayerMenuReplayUserEventData()
        {
            unk08 = 0;
            pad0A = 0;
            pad0C = 0;
            str = nullptr;
            data.u = 0;
        }

        RE::BSString* str;         // 10
        RE::BSFixedString fixedStr;  // 18
        Payload data;              // 20
    };
    static_assert(sizeof(PlayerMenuReplayUserEventData) == 0x28);

    std::string SanitizePlayerMenuDialogueLine(std::string line)
    {
        for (char& ch : line) {
            if (ch == '\r' || ch == '\n' || ch == '|') {
                ch = ' ';
            }
        }
        return trim(line);
    }

    std::string NormalizePlayerMenuEventName(std::string eventName)
    {
        eventName = trim(eventName);
        std::transform(
            eventName.begin(),
            eventName.end(),
            eventName.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        return eventName;
    }

    std::string CanonicalizePlayerMenuEventName(std::string eventName)
    {
        eventName = NormalizePlayerMenuEventName(std::move(eventName));
        eventName.erase(
            std::remove_if(
                eventName.begin(),
                eventName.end(),
                [](unsigned char c) {
                    return !std::isalnum(c);
                }),
            eventName.end());
        return eventName;
    }

    std::string GetPlayerMenuUserEventName(const RE::UIMessage& message)
    {
        if (message.type != RE::UI_MESSAGE_TYPE::kUserEvent || !message.data) {
            return "";
        }

        const auto* data = static_cast<const RE::BSUIMessageData*>(message.data);
        if (!data->fixedStr.empty()) {
            return std::string(data->fixedStr.c_str());
        }
        if (data->str && !data->str->empty()) {
            return std::string(data->str->c_str());
        }
        return "";
    }

    bool TryStartPlayerMenuDelayedSelection(const std::string& userEventName);
    bool TryQueuePlayerMenuDelayedSelectionPlayback(const RE::MenuTopicManager* tm, const char* triggerSource);
    bool DispatchPlayerMenuDelayedSelection();
    void ClearPlayerMenuDelayedSelectionState();
    void PumpPlayerMenuDelayedSelectionCapture();
    void QueuePlayerMenuDialoguePrefetch(const RE::MenuTopicManager* tm, RE::Actor* listenerActor,
                                         const std::string& fallbackPlayerName);
    void QueuePlayerMenuLocalPlayback(const std::string& line, RE::Actor* listenerActor);
    void ReleasePlayerMenuDialogueGateOnGameThread(bool forceTimeoutRelease);
    void ClearPlayerMenuDeferredDialogueCommit();
    void EndPlayerMenuDialogueGate(bool forceTimeoutRelease);

    void MarkPlayerMenuSelectionUserEventSeen()
    {
        std::lock_guard<std::mutex> lock(g_playerMenuEventTimingMutex);
        g_playerMenuLastSelectionUserEvent = std::chrono::steady_clock::now();
    }

    bool WasPlayerMenuSelectionUserEventSeenRecently(std::chrono::milliseconds window)
    {
        std::lock_guard<std::mutex> lock(g_playerMenuEventTimingMutex);
        if (g_playerMenuLastSelectionUserEvent == std::chrono::steady_clock::time_point::min()) {
            return false;
        }
        return (std::chrono::steady_clock::now() - g_playerMenuLastSelectionUserEvent) <= window;
    }

    bool ShouldSuppressDuplicatePlayerMenuIntercept(const std::string& selectedLine)
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_playerMenuEventTimingMutex);
        constexpr auto duplicateWindow = std::chrono::milliseconds(350);
        if (g_playerMenuLastInterceptedLine == selectedLine &&
            g_playerMenuLastInterceptedLineAt != std::chrono::steady_clock::time_point::min() &&
            (now - g_playerMenuLastInterceptedLineAt) <= duplicateWindow) {
            return true;
        }
        g_playerMenuLastInterceptedLine = selectedLine;
        g_playerMenuLastInterceptedLineAt = now;
        return false;
    }

    bool IsLikelyPlayerMenuSelectionScaleformEvent(const RE::UIMessage& message)
    {
        if (message.type != RE::UI_MESSAGE_TYPE::kScaleformEvent || !message.data) {
            return false;
        }

        const auto* scaleformData = static_cast<const RE::BSUIScaleformData*>(message.data);
        if (!scaleformData->scaleformEvent) {
            return false;
        }

        const auto eventType = scaleformData->scaleformEvent->type.get();
        if (eventType == RE::GFxEvent::EventType::kMouseDown ||
            eventType == RE::GFxEvent::EventType::kMouseUp) {
            const auto* mouseEvent = static_cast<const RE::GFxMouseEvent*>(scaleformData->scaleformEvent);
            return mouseEvent && mouseEvent->button == 0;
        }

        return false;
    }

    bool TryInterceptPlayerMenuSelection(const std::string& triggerEventName, const char* triggerSource)
    {
        return false;

        auto* tm = RE::MenuTopicManager::GetSingleton();
        if (!tm) {
            return false;
        }

        // If NPC dialogue is already active, interception is too late and causes duplicate replay/skip behavior.
        if (tm->currentTopicInfo) {
            return false;
        }

        if (!TryStartPlayerMenuDelayedSelection(triggerEventName)) {
            return false;
        }

        TryQueuePlayerMenuDelayedSelectionPlayback(tm, triggerSource);
        return true;
    }

    bool IsPlayerMenuSelectionUserEvent(const std::string& userEventName)
    {
        const std::string normalized = CanonicalizePlayerMenuEventName(userEventName);
        if (normalized.empty()) {
            return false;
        }
        if (normalized.find("accept") != std::string::npos ||
            normalized.find("select") != std::string::npos ||
            normalized.find("click") != std::string::npos ||
            normalized.find("topic") != std::string::npos ||
            normalized.find("submit") != std::string::npos ||
            normalized.find("activate") != std::string::npos) {
            return true;
        }
        return normalized == "accept" || normalized == "click" || normalized == "select" ||
               normalized == "topicclicked" || normalized == "enter" || normalized == "submit" ||
               normalized == "activate" || normalized == "ok" || normalized == "xbuttona" ||
               normalized == "gamepadaccept";
    }

    bool IsPlayerMenuCancelUserEvent(const std::string& userEventName)
    {
        const std::string normalized = CanonicalizePlayerMenuEventName(userEventName);
        return normalized == "cancel" || normalized == "back" || normalized == "tab";
    }

    bool IsPlayerMenuReplayUserEventMessage(const RE::UIMessage& message)
    {
        if (message.type != RE::UI_MESSAGE_TYPE::kUserEvent || !message.data) {
            return false;
        }

        const auto* data = static_cast<const RE::BSUIMessageData*>(message.data);
        return data->data.u == kPlayerMenuReplayUserEventMarker;
    }

    bool ConsumePlayerMenuReplayUserEvent(const RE::UIMessage& message, const std::string& userEventName)
    {
        std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
        if (!g_playerMenuReplayUserEventArmed) {
            return false;
        }

        if (!IsPlayerMenuReplayUserEventMessage(message)) {
            return false;
        }

        if (!g_playerMenuReplayUserEventName.empty()) {
            const std::string expected = CanonicalizePlayerMenuEventName(g_playerMenuReplayUserEventName);
            const std::string incoming = CanonicalizePlayerMenuEventName(userEventName);
            if (!incoming.empty() && expected != incoming) {
                return false;
            }
        }

        g_playerMenuReplayUserEventArmed = false;
        g_playerMenuReplayInFlight = true;
        g_playerMenuReplayUserEventName.clear();
        return true;
    }

    bool IsPlayerMenuReplayUserEventArmed()
    {
        std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
        return g_playerMenuReplayUserEventArmed;
    }

    bool IsPlayerMenuReplayInFlight()
    {
        std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
        return g_playerMenuReplayInFlight;
    }

    bool HasPlayerMenuDelayedSelectionPending()
    {
        std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
        return g_playerMenuDelayedSelectionActive;
    }

    bool IsPlayerMenuDialogueGateActive()
    {
        std::lock_guard<std::mutex> lock(g_playerMenuDialogueGateMutex);
        return g_playerMenuDialogueGateActive;
    }

    bool TryStartPlayerMenuDelayedSelection(const std::string& userEventName)
    {
        const std::string original = trim(userEventName);
        if (!IsPlayerMenuSelectionUserEvent(original)) {
            return false;
        }

        RE::MenuTopicManager::Dialogue* baselineDialogue = nullptr;
        if (const auto* tm = RE::MenuTopicManager::GetSingleton()) {
            baselineDialogue = tm->lastSelectedDialogue;
        }

        std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
        if (g_playerMenuDelayedSelectionActive || g_playerMenuReplayUserEventArmed ||
            g_playerMenuReplayInFlight) {
            return false;
        }
        g_playerMenuDelayedSelectionActive = true;
        g_playerMenuDelayedUserEventName = original.empty() ? "accept" : original;
        g_playerMenuDelayedSelectionStartedAt = std::chrono::steady_clock::now();
        g_playerMenuDelayedBaselineDialogue = baselineDialogue;
        g_playerMenuDelayedSelectionPlaybackQueued = false;
        g_playerMenuDelayScaleformBlockLogged = false;
        return true;
    }

    bool TryQueuePlayerMenuDelayedSelectionPlayback(const RE::MenuTopicManager* tm, const char* triggerSource)
    {
        if (!tm) {
            return false;
        }

        RE::MenuTopicManager::Dialogue* baselineDialogue = nullptr;
        std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::time_point::min();
        {
            std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
            if (!g_playerMenuDelayedSelectionActive || g_playerMenuReplayUserEventArmed ||
                g_playerMenuReplayInFlight || g_playerMenuDelayedSelectionPlaybackQueued) {
                return false;
            }
            baselineDialogue = g_playerMenuDelayedBaselineDialogue;
            startedAt = g_playerMenuDelayedSelectionStartedAt;
        }

        RE::MenuTopicManager::Dialogue* selectedDialogue = nullptr;
        if (auto* responseNode = tm->selectedResponseNode) {
            selectedDialogue = responseNode->front();
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = startedAt == std::chrono::steady_clock::time_point::min()
                                 ? std::chrono::steady_clock::duration::zero()
                                 : (now - startedAt);

        if (!selectedDialogue && tm->lastSelectedDialogue) {
            if (tm->lastSelectedDialogue != baselineDialogue ||
                elapsed >= kPlayerMenuSelectionCaptureBaselineDelay) {
                selectedDialogue = tm->lastSelectedDialogue;
            }
        }

        if (!selectedDialogue) {
            return false;
        }

        const char* topicTextCStr = selectedDialogue->topicText.c_str();
        std::string selectedLine = topicTextCStr ? topicTextCStr : "";
        selectedLine = SanitizePlayerMenuDialogueLine(selectedLine);
        if (selectedLine.empty()) {
            return false;
        }
        if (ShouldSuppressDuplicatePlayerMenuIntercept(selectedLine)) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
            if (!g_playerMenuDelayedSelectionActive || g_playerMenuReplayUserEventArmed ||
                g_playerMenuReplayInFlight || g_playerMenuDelayedSelectionPlaybackQueued) {
                return false;
            }
            g_playerMenuDelayedSelectionPlaybackQueued = true;
        }

        auto lastSpeaker = tm->speaker.get();
        RE::Actor* menuListenerActor = lastSpeaker ? lastSpeaker->As<RE::Actor>() : nullptr;
        QueuePlayerMenuDialoguePrefetch(tm, menuListenerActor, AIAgentManager::getInstance().getPlayerName());
        g_playerMenuSkipNextTopicPlayback.store(true);
        QueuePlayerMenuLocalPlayback(selectedLine, menuListenerActor);

        return true;
    }

    void PumpPlayerMenuDelayedSelectionCapture()
    {
        std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::time_point::min();
        {
            std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
            if (!g_playerMenuDelayedSelectionActive || g_playerMenuReplayUserEventArmed ||
                g_playerMenuReplayInFlight || g_playerMenuDelayedSelectionPlaybackQueued) {
                return;
            }
            startedAt = g_playerMenuDelayedSelectionStartedAt;
        }

        auto* tm = RE::MenuTopicManager::GetSingleton();
        if (!tm) {
            return;
        }

        if (tm->currentTopicInfo) {
            logger::warn("[PlayerMenuTTS] NPC dialogue advanced before delayed topic capture completed; clearing state");
            ClearPlayerMenuDelayedSelectionState();
            return;
        }

        if (TryQueuePlayerMenuDelayedSelectionPlayback(tm, "menu_advance")) {
            return;
        }

        if (startedAt == std::chrono::steady_clock::time_point::min()) {
            return;
        }

        if ((std::chrono::steady_clock::now() - startedAt) >= kPlayerMenuSelectionCaptureTimeout) {
            logger::warn("[PlayerMenuTTS] Timed out waiting for selected dialogue topic; replaying original click");
            DispatchPlayerMenuDelayedSelection();
        }
    }

    bool DispatchPlayerMenuDelayedSelection()
    {
        std::string userEventName;
        {
            std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
            if (!g_playerMenuDelayedSelectionActive) {
                return false;
            }

            userEventName = g_playerMenuDelayedUserEventName.empty() ? "accept" : g_playerMenuDelayedUserEventName;
            g_playerMenuDelayedSelectionActive = false;
            g_playerMenuDelayedUserEventName.clear();
            g_playerMenuDelayedSelectionStartedAt = std::chrono::steady_clock::time_point::min();
            g_playerMenuDelayedBaselineDialogue = nullptr;
            g_playerMenuDelayedSelectionPlaybackQueued = false;
            g_playerMenuReplayUserEventArmed = false;
            g_playerMenuReplayInFlight = false;
            g_playerMenuReplayUserEventName.clear();
            g_playerMenuDelayScaleformBlockLogged = false;
        }

        {
            std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
            g_playerMenuReplayUserEventArmed = true;
            g_playerMenuReplayUserEventName = userEventName;
        }

        auto* messageQueue = RE::UIMessageQueue::GetSingleton();
        if (!messageQueue) {
            logger::warn("[PlayerMenuTTS] UIMessageQueue unavailable; cannot replay delayed dialogue selection");
            return false;
        }

        auto* userEventData = new PlayerMenuReplayUserEventData();

        userEventData->str = nullptr;
        userEventData->fixedStr = userEventName;
        userEventData->data.u = kPlayerMenuReplayUserEventMarker;
        g_playerMenuSkipNextTopicPlayback.store(true);
        messageQueue->AddMessage(RE::DialogueMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kUserEvent, userEventData);
        return true;
    }

    bool ConsumePlayerMenuSkipNextTopicPlayback()
    {
        return g_playerMenuSkipNextTopicPlayback.exchange(false);
    }

    void ClearPlayerMenuDelayedSelectionState()
    {
        std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
        g_playerMenuDelayedSelectionActive = false;
        g_playerMenuDelayedUserEventName.clear();
        g_playerMenuDelayedSelectionStartedAt = std::chrono::steady_clock::time_point::min();
        g_playerMenuDelayedBaselineDialogue = nullptr;
        g_playerMenuDelayedSelectionPlaybackQueued = false;
        g_playerMenuReplayUserEventArmed = false;
        g_playerMenuReplayInFlight = false;
        g_playerMenuReplayUserEventName.clear();
        g_playerMenuDelayScaleformBlockLogged = false;
    }

    void ClearPlayerMenuPendingPlayerSpeech()
    {
        auto& speakManager = SpeakManager::getInstance();
        ScriptLine queuedLine = speakManager.getFirstItem();
        const bool playerQueued = queuedLine.actor == "Player";
        const bool playerProcessing = playerQueued && speakManager.getProcessing();
        speakManager.clearPlayerPlaybackCompletedCallback();
        speakManager.deleteQueuedPlayerLines();
        if (playerProcessing) {
            speakManager.abortPlay(true);
        }
    }

    void ResetPlayerMenuCustomPlaybackStateForNpcResponse()
    {
        const bool hadCustomPlayback =
            HasPlayerMenuDelayedSelectionPending() || IsPlayerMenuReplayUserEventArmed() ||
            IsPlayerMenuReplayInFlight() || IsPlayerMenuDialogueGateActive();
        if (!hadCustomPlayback && !g_playerMenuSkipNextTopicPlayback.load()) {
            return;
        }

        g_playerMenuSkipNextTopicPlayback.store(false);
        ClearPlayerMenuDeferredDialogueCommit();
        ClearPlayerMenuDelayedSelectionState();
        EndPlayerMenuDialogueGate(false);
        g_playerMenuReleaseTaskQueued.store(false);
        ClearPlayerMenuPendingPlayerSpeech();
    }

    void ArmPlayerMenuDeferredDialogueCommit(RE::Actor* speakerActor, RE::DialogueResponse* response)
    {
        if (!speakerActor || !response) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_playerMenuDeferredCommitMutex);
        g_playerMenuDeferredCommitSpeakerFormID = speakerActor->GetFormID();
        g_playerMenuDeferredCommitResponse = response;
    }

    void ClearPlayerMenuDeferredDialogueCommit()
    {
        std::lock_guard<std::mutex> lock(g_playerMenuDeferredCommitMutex);
        g_playerMenuDeferredCommitSpeakerFormID = 0;
        g_playerMenuDeferredCommitResponse = nullptr;
    }

    bool DispatchPlayerMenuDeferredDialogueCommit()
    {
        RE::FormID speakerFormID = 0;
        RE::DialogueResponse* response = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_playerMenuDeferredCommitMutex);
            speakerFormID = g_playerMenuDeferredCommitSpeakerFormID;
            response = g_playerMenuDeferredCommitResponse;
            g_playerMenuDeferredCommitSpeakerFormID = 0;
            g_playerMenuDeferredCommitResponse = nullptr;
        }

        if (speakerFormID == 0 || !response) {
            return false;
        }

        auto* speakerActor = RE::TESForm::LookupByID<RE::Actor>(speakerFormID);
        if (!speakerActor) {
            logger::warn("[PlayerMenuTTS] Deferred dialogue commit speaker {:08X} no longer exists", speakerFormID);
            return false;
        }

        speakerActor->SetSpeakingDone(false);
        speakerActor->AllowPCDialogue(true);
        g_playerMenuForceDeferredCommit.store(true);
        bool updated = speakerActor->UpdateInDialogue(response, false);
        if (!updated) {
            updated = speakerActor->UpdateInDialogue(response, true);
        }
        g_playerMenuForceDeferredCommit.store(false);
        return updated;
    }

    bool ShouldBlockPlayerMenuScaleformWhileDelayedSelection(bool* logOnce = nullptr)
    {
        std::lock_guard<std::mutex> lock(g_playerMenuDelayedSelectionMutex);
        const bool shouldBlock = g_playerMenuDelayedSelectionActive || g_playerMenuReplayUserEventArmed;
        if (logOnce) {
            *logOnce = false;
            if (shouldBlock && !g_playerMenuDelayScaleformBlockLogged) {
                g_playerMenuDelayScaleformBlockLogged = true;
                *logOnce = true;
            }
        }
        return shouldBlock;
    }

    void CleanupPlayerMenuTtsPrefetchHistory(const std::chrono::steady_clock::time_point& now)
    {
        if (g_playerMenuTtsLastCleanup != std::chrono::steady_clock::time_point::min() &&
            (now - g_playerMenuTtsLastCleanup) < kPlayerMenuTtsPrefetchCleanupInterval) {
            return;
        }

        for (auto it = g_playerMenuTtsPrefetchHistory.begin(); it != g_playerMenuTtsPrefetchHistory.end();) {
            if ((now - it->second) > kPlayerMenuTtsPrefetchRetention) {
                it = g_playerMenuTtsPrefetchHistory.erase(it);
            } else {
                ++it;
            }
        }

        g_playerMenuTtsLastCleanup = now;
    }

    bool ShouldQueuePlayerMenuTtsPrefetch(const std::string& sanitizedLine, const std::chrono::steady_clock::time_point& now)
    {
        auto it = g_playerMenuTtsPrefetchHistory.find(sanitizedLine);
        if (it != g_playerMenuTtsPrefetchHistory.end() &&
            (now - it->second) < kPlayerMenuTtsPrefetchCooldown) {
            return false;
        }

        g_playerMenuTtsPrefetchHistory[sanitizedLine] = now;
        return true;
    }

    void QueuePlayerMenuTtsEvent(const std::string& eventType, const std::string& line, RE::Actor* listenerActor,
                                 const std::string& fallbackPlayerName)
    {
        std::string sanitizedLine = SanitizePlayerMenuDialogueLine(line);
        if (sanitizedLine.empty()) {
            return;
        }

        std::string playerName = trim(fallbackPlayerName);
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            std::string displayName = trim(player->GetDisplayFullName());
            if (!displayName.empty()) {
                playerName = displayName;
            }
        }
        if (playerName.empty()) {
            playerName = "Player";
        }

        std::string payload = std::format(
            "{}|{}|{}|{}: {}",
            eventType,
            getCurrentTimeMillis(),
            GetGameTimeStamp(),
            playerName,
            sanitizedLine);

        if (listenerActor) {
            HTTPManager::log(payload, listenerActor);
        } else {
            HTTPManager::log(payload);
        }
    }

    void SuppressActorDialogueWhilePlayerMenuGate(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        // Keep dialogue paused without flagging it as finished, otherwise Skyrim may skip the NPC line.
        actor->AllowPCDialogue(false);
        actor->PauseCurrentDialogue();
        actor->SetSpeakingDone(false);
    }

    void QueuePlayerMenuDialoguePrefetch(const RE::MenuTopicManager* tm, RE::Actor* listenerActor,
                                         const std::string& fallbackPlayerName)
    {
        if (!PlayerTtsTraditionalDialogueEnabled) {
            return;
        }

        if (!tm || !tm->dialogueList) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        CleanupPlayerMenuTtsPrefetchHistory(now);

        std::size_t queuedCount = 0;
        for (auto iter = tm->dialogueList->begin();
             iter != tm->dialogueList->end() && queuedCount < kPlayerMenuTtsPrefetchMaxPerEvent;
             ++iter) {
            auto* dialogue = *iter;
            if (!dialogue) {
                continue;
            }

            const char* topicTextCStr = dialogue->topicText.c_str();
            if (!topicTextCStr) {
                continue;
            }

            std::string sanitizedLine = SanitizePlayerMenuDialogueLine(topicTextCStr);
            if (sanitizedLine.empty()) {
                continue;
            }

            if (!ShouldQueuePlayerMenuTtsPrefetch(sanitizedLine, now)) {
                continue;
            }

            QueuePlayerMenuTtsEvent("player_menu_tts_prefetch", sanitizedLine, listenerActor, fallbackPlayerName);
            queuedCount++;
        }
    }

    std::chrono::milliseconds EstimatePlayerMenuDialogueHoldDuration(const std::string& line)
    {
        if (line.empty()) {
            return kPlayerMenuDialogueGateEstimatedMin;
        }

        std::size_t visibleChars = 0;
        std::size_t wordCount = 0;
        std::size_t strongPauseCount = 0;
        std::size_t softPauseCount = 0;
        bool inWord = false;

        for (char ch : line) {
            const auto c = static_cast<unsigned char>(ch);
            if (!std::isspace(c)) {
                visibleChars++;
            }

            if (std::isspace(c)) {
                inWord = false;
            } else if (!inWord) {
                wordCount++;
                inWord = true;
            }

            switch (ch) {
                case '.':
                case '!':
                case '?':
                case ';':
                case ':':
                    strongPauseCount++;
                    break;
                case ',':
                    softPauseCount++;
                    break;
                default:
                    break;
            }
        }

        const double secondsByChars = static_cast<double>(visibleChars) / 14.0;
        const double secondsByWords = static_cast<double>(wordCount) / 2.6;
        const double punctuationPauseSeconds =
            static_cast<double>(softPauseCount) * 0.08 + static_cast<double>(strongPauseCount) * 0.2;
        const double estimatedSeconds =
            std::max(secondsByChars, secondsByWords) + punctuationPauseSeconds + 0.35;

        auto estimatedDuration = std::chrono::milliseconds(static_cast<int>(estimatedSeconds * 1000.0));
        if (estimatedDuration < kPlayerMenuDialogueGateEstimatedMin) {
            estimatedDuration = kPlayerMenuDialogueGateEstimatedMin;
        } else if (estimatedDuration > kPlayerMenuDialogueGateEstimatedMax) {
            estimatedDuration = std::chrono::duration_cast<std::chrono::milliseconds>(kPlayerMenuDialogueGateEstimatedMax);
        }
        return estimatedDuration;
    }

    void BeginPlayerMenuDialogueGate(RE::Actor* listenerActor, std::chrono::milliseconds minimumHoldDuration)
    {
        RE::FormID previousSpeakerFormID = 0;
        const RE::FormID listenerSpeakerFormID = listenerActor ? listenerActor->GetFormID() : 0;
        const auto now = std::chrono::steady_clock::now();
        const auto defaultHardHoldDuration = std::chrono::duration_cast<std::chrono::milliseconds>(kPlayerMenuDialogueGateMaxHold);
        const auto holdPaddingDuration = std::chrono::duration_cast<std::chrono::milliseconds>(kPlayerMenuDialogueGateHoldPadding);
        const auto hardHoldDuration = std::max(defaultHardHoldDuration, minimumHoldDuration + holdPaddingDuration);
        {
            std::lock_guard<std::mutex> lock(g_playerMenuDialogueGateMutex);
            if (g_playerMenuDialogueGateActive &&
                g_playerMenuDialogueGateSpeakerFormID != listenerSpeakerFormID) {
                previousSpeakerFormID = g_playerMenuDialogueGateSpeakerFormID;
            }
            g_playerMenuDialogueGateActive = true;
            g_playerMenuDialogueGateSpeakerFormID = listenerSpeakerFormID;
            g_playerMenuDialogueGateMinRelease = now + minimumHoldDuration;
            g_playerMenuDialogueGateHardDeadline = now + hardHoldDuration;
        }

        if (previousSpeakerFormID != 0) {
            if (auto* previousActor = RE::TESForm::LookupByID<RE::Actor>(previousSpeakerFormID)) {
                previousActor->SetSpeakingDone(false);
                previousActor->AllowPCDialogue(true);
            }
        }

        if (listenerActor) {
            SuppressActorDialogueWhilePlayerMenuGate(listenerActor);
        }
    }

    void EndPlayerMenuDialogueGate(bool forceTimeoutRelease = false)
    {
        RE::FormID speakerFormID = 0;
        {
            std::lock_guard<std::mutex> lock(g_playerMenuDialogueGateMutex);
            if (!g_playerMenuDialogueGateActive) {
                return;
            }
            speakerFormID = g_playerMenuDialogueGateSpeakerFormID;
            g_playerMenuDialogueGateActive = false;
            g_playerMenuDialogueGateSpeakerFormID = 0;
            g_playerMenuDialogueGateMinRelease = std::chrono::steady_clock::time_point::min();
            g_playerMenuDialogueGateHardDeadline = std::chrono::steady_clock::time_point::min();
        }

        if (speakerFormID != 0) {
            if (auto* speakerActor = RE::TESForm::LookupByID<RE::Actor>(speakerFormID)) {
                speakerActor->SetSpeakingDone(false);
                speakerActor->AllowPCDialogue(true);
            }
        }

        if (forceTimeoutRelease) {
            logger::warn("[PlayerMenuTTS] Released dialogue gate due to timeout");
        }
    }

    void ReleasePlayerMenuDialogueGateOnGameThread(bool forceTimeoutRelease)
    {
        if (g_playerMenuReleaseTaskQueued.exchange(true)) {
            return;
        }

        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            EndPlayerMenuDialogueGate(forceTimeoutRelease);
            if (kPlayerMenuUseActorCommitGate) {
                DispatchPlayerMenuDeferredDialogueCommit();
                ClearPlayerMenuDelayedSelectionState();
            } else {
                if (HasPlayerMenuDelayedSelectionPending()) {
                    DispatchPlayerMenuDelayedSelection();
                } else {
                    ClearPlayerMenuDelayedSelectionState();
                }
            }
            g_playerMenuReleaseTaskQueued.store(false);
            return;
        }

        taskInterface->AddTask([forceTimeoutRelease]() {
            EndPlayerMenuDialogueGate(forceTimeoutRelease);
            if (kPlayerMenuUseActorCommitGate) {
                DispatchPlayerMenuDeferredDialogueCommit();
                ClearPlayerMenuDelayedSelectionState();
            } else {
                if (HasPlayerMenuDelayedSelectionPending()) {
                    DispatchPlayerMenuDelayedSelection();
                } else {
                    ClearPlayerMenuDelayedSelectionState();
                }
            }
            g_playerMenuReleaseTaskQueued.store(false);
        });
    }

    void UpdatePlayerMenuDialogueGate()
    {
        bool gateActive = false;
        std::chrono::steady_clock::time_point gateMinRelease = std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point gateHardDeadline = std::chrono::steady_clock::time_point::min();
        {
            std::lock_guard<std::mutex> lock(g_playerMenuDialogueGateMutex);
            gateActive = g_playerMenuDialogueGateActive;
            gateMinRelease = g_playerMenuDialogueGateMinRelease;
            gateHardDeadline = g_playerMenuDialogueGateHardDeadline;
        }

        if (!gateActive) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (gateHardDeadline != std::chrono::steady_clock::time_point::min() && now > gateHardDeadline) {
            ReleasePlayerMenuDialogueGateOnGameThread(true);
            return;
        }

        if (gateMinRelease != std::chrono::steady_clock::time_point::min() && now < gateMinRelease) {
            RE::FormID speakerFormID = 0;
            {
                std::lock_guard<std::mutex> lock(g_playerMenuDialogueGateMutex);
                speakerFormID = g_playerMenuDialogueGateSpeakerFormID;
            }
            if (speakerFormID != 0) {
                if (auto* speakerActor = RE::TESForm::LookupByID<RE::Actor>(speakerFormID)) {
                    SuppressActorDialogueWhilePlayerMenuGate(speakerActor);
                }
            }
            return;
        }

        auto& speakManager = SpeakManager::getInstance();
        if (!speakManager.getProcessing()) {
            ScriptLine firstItem = speakManager.getFirstItem();
            if (firstItem.actor != "Player") {
                ReleasePlayerMenuDialogueGateOnGameThread(false);
            }
        }
    }

    void QueuePlayerMenuLocalPlayback(const std::string& line, RE::Actor* listenerActor)
    {
        if (!PlayerTtsTraditionalDialogueEnabled) {
            return;
        }

        std::string sanitizedLine = SanitizePlayerMenuDialogueLine(line);
        if (sanitizedLine.empty()) {
            return;
        }

        auto& speakManager = SpeakManager::getInstance();
        if (speakManager.getProcessing()) {
            ScriptLine firstItem = speakManager.getFirstItem();
            if (firstItem.actor == "Player") {
                speakManager.abortPlay(true);
            }
        }
        speakManager.clearPlayerPlaybackCompletedCallback();
        speakManager.deleteQueuedPlayerLines();

        if (HasPlayerMenuDelayedSelectionPending()) {
            speakManager.setPlayerPlaybackCompletedCallback([](const ScriptLine&, int) {
                ReleasePlayerMenuDialogueGateOnGameThread(false);
            });
        }

        ScriptLine playerLine(sanitizedLine, "", "__player_menu_tts", "", "Player", "", 1.0f);
        speakManager.insertInQueue(playerLine);
    }
}

void SkipNextPlayerMenuTopicLocalPlayback()
{
    g_playerMenuSkipNextTopicPlayback.store(true);
}

    /* Debug things*/

void MutexSetScreenShotSendMode(int newVal) {
    shotSendModeMutex.lock();
    shotSendMode = newVal;
    shotSendModeMutex.unlock();
}

int  MutexGetScreenShotSendMode() {
    shotSendModeMutex.lock();
    int mode = shotSendMode;
    shotSendModeMutex.unlock();
    return mode;
}

bool MutexIsMakeShotActivated() {
    makeShotMutex.lock();
    bool isMakeShot = makeShot;
    makeShotMutex.unlock();
    return isMakeShot;
}

void MutexSetMakeShotActive(bool newVal) {
    makeShotMutex.lock();
    makeShot = newVal;
    makeShotMutex.unlock();
}

bool MutexIsMakeShotNativeActivated() {
    makeShotNativeMutex.lock();
    bool isMakeShotNative = makeShotNative;
    makeShotNativeMutex.unlock();
    return isMakeShotNative;
}

void MutexSetMakeShotNativeActive(bool newVal) {
    makeShotNativeMutex.lock();
    makeShotNative = newVal;
    makeShotNativeMutex.unlock();
}

void ProcedureListenToScene() {
    // Cell transitions can storm scene/subtitle events; skip until world maintenance settles.
    if (IsWorldMaintenanceSuppressed()) return;

    // 200ms throttle: fires from TESSceneEvent which storms in towns; was hitting 17/sec.
    static std::chrono::steady_clock::time_point lastRun;
    static std::mutex lastRunMtx;
    {
        std::lock_guard<std::mutex> lk(lastRunMtx);
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRun < std::chrono::milliseconds(200)) return;
        lastRun = now;
    }
    auto* sm = RE::SubtitleManager::GetSingleton();
    for (auto s : sm->subtitles) {
        if (!s.speaker.get()) return;
        RE::Actor* actor = (s.speaker.get().get()->As<RE::Actor>());
        if (!actor) return;
        
        
        AIAgentManager& aiam = AIAgentManager::getInstance();
        bool skipThisSubtitle = false;
        for (const auto& agent : aiam.getAgents()) {
            // AI AGent chat
            auto localactor = agent->getActorByFormId();
            if (localactor) {
                if (localactor->GetFormID() == actor->GetFormID() &&
                    (s.pad04 == 0xabcd || SpeakManager::getInstance().isRecentAiSubtitle(actor->GetFormID(), s.subtitle.c_str()))) {
                    logger::info("[ProcedureListenToScene] Skipped recognized AI dialogue, actor:{},text:{}", localactor->GetDisplayFullName(),
                        s.subtitle);
                    skipThisSubtitle = true;
                    break;
                }
            }
        }

        if (skipThisSubtitle)  // Was AI generated subtitle, skip to avoid duplicates with AI Agent chat system
            continue;
        
        if (actor->GetFormID() == RE::PlayerCharacter::GetSingleton()->As<RE::Actor>()->GetFormID()) return;
        if (s.targetDistance < parameterMaxDistanceToListen) {
            auto it =
                std::find(BackGroundDialogueQueue.begin(), BackGroundDialogueQueue.end(), std::string(actor->GetName()) + s.subtitle.c_str());
            if (it == BackGroundDialogueQueue.end()) {
                std::string subtitleString(s.subtitle);

                if (actor->IsGhost())
                    HTTPManager::log(std::format("chat|{}|{}|(Context location: {} background chat) Ghost of {}: {}", getCurrentTimeMillis(),
                                 GetGameTimeStamp(), GetPlayerLocation(), actor->GetDisplayFullName(), subtitleString));
                else
                    HTTPManager::log(std::format("chat|{}|{}|(Context location: {} background chat) {}: {}",
                                                 getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                                 actor->GetDisplayFullName(), subtitleString));

                try {
                    json sData;
                    sData["speaker"] = actor->GetName();
                    sData["location"] = GetPlayerLocation();
                    sData["speech"] = s.subtitle.c_str();
                    sData["listener"] = "unknown";
                    AddCachedSpeechAudience(sData, "background_dialogue_speech");
                    HTTPManager::log(std::format("_speech|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), sData.dump()));
                } catch (nlohmann::json_abi_v3_11_2::detail::type_error& ex) {
                    logger::info("Error sending speech. Review encoding, {}", ex.what());
                }

                // Push to chatbox in real-time
                if (PrismaUIBridge::IsAvailable()) {
                    char timeDateString[200];
                    RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, false);
                    logger::info("[Chatbox] Pushing subtitle: {} says: {}", 
                               actor->GetDisplayFullName(), 
                               subtitleString.substr(0, 50));
                    PrismaUIBridge::PushChatboxMessage(
                        actor->GetDisplayFullName(), 
                        subtitleString,
                        std::string(timeDateString),
                        "npc",
                        "subtitle"
                    );
                    // NOTE: Don't push to history here - the all-subtitles monitor handles it to avoid duplicates
                }

                BackGroundDialogueQueue.push_back(std::string(actor->GetName()) + s.subtitle.c_str());
            }
        }
    }
}

// New function to monitor ALL subtitles for chatbox
static std::unordered_map<std::string, std::string> g_lastSubtitleCache; // speaker+text -> timestamp

void MonitorAllSubtitlesForChatbox() {
    if (IsWorldMaintenanceSuppressed()) return;
    if (!PrismaUIBridge::IsAvailable()) return;
    
    auto* sm = RE::SubtitleManager::GetSingleton();
    if (!sm) return;
    
    for (auto& s : sm->subtitles) {
        if (!s.speaker.get()) continue;
        RE::Actor* actor = s.speaker.get().get()->As<RE::Actor>();
        if (!actor) continue;
        
        std::string subtitle(s.subtitle);
        if (subtitle.empty()) continue;
        const bool aiGeneratedSubtitle =
            s.pad04 == 0xabcd || SpeakManager::getInstance().isRecentAiSubtitle(actor->GetFormID(), subtitle);
        const std::string source = aiGeneratedSubtitle ? "llm" : "subtitle";
        
        // Create unique key to prevent duplicate pushes
        std::string cacheKey = std::string(actor->GetDisplayFullName()) + "|" + subtitle;
        
        // Check if we've already pushed this exact subtitle
        if (g_lastSubtitleCache.find(cacheKey) != g_lastSubtitleCache.end()) {
            continue; // Already pushed
        }
        
        // Add to cache
        char timeDateString[200];
        RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, false);
        g_lastSubtitleCache[cacheKey] = std::string(timeDateString);
        
        // Clean up old cache entries (keep last 100)
        if (g_lastSubtitleCache.size() > 100) {
            auto it = g_lastSubtitleCache.begin();
            g_lastSubtitleCache.erase(it);
        }
        
        // Determine speaker type
        std::string speakerType = "npc";
        if (actor->GetFormID() == RE::PlayerCharacter::GetSingleton()->GetFormID()) {
            const auto narratorDisplayName = SpeakManager::getInstance().getNarratorDisplayName();
            const std::string visibleSpeakerName = actor->GetDisplayFullName() ? actor->GetDisplayFullName() : "";
            speakerType = aiGeneratedSubtitle && visibleSpeakerName == narratorDisplayName
                ? "narrator"
                : "player";
        }
        
        logger::debug("[Chatbox] All subtitles monitor: {} says: {}", 
                     actor->GetDisplayFullName(), 
                     subtitle.substr(0, 50));
        
        PrismaUIBridge::PushChatboxMessage(
            actor->GetDisplayFullName(), 
            subtitle,
            std::string(timeDateString),
            speakerType,
            source
        );
        // Also push to conversation history panel
        PrismaUIBridge::PushDialogueEntry(
            actor->GetDisplayFullName(), 
            subtitle,
            std::string(timeDateString),
            "chat",
            source
        );
    }
}

void ProcessInstantiateActors() {

    AIAgentManager& agentManager = AIAgentManager::getInstance();

    auto agents = agentManager.getAgents();

    if (agents.size() == 0) {
        // No agents
    } else {
    
    
    }
}

static std::unordered_map<uint32_t, RE::NiPoint3> g_lastActivityPosition;
static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> g_lastActivitySampleTime;
static std::mutex g_activityStatusMutex;

std::string ToLowerCopyPlugin(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool FurnitureHasMarkerAnimation(RE::TESObjectREFR* furnitureRef, RE::BSFurnitureMarker::AnimationType desiredType)
{
    if (!furnitureRef) {
        return false;
    }

    auto* root = furnitureRef->Get3D();
    if (!root) {
        return false;
    }

    auto* extra = root->GetExtraData("FRN");
    auto* markerNode = extra ? netimmerse_cast<RE::BSFurnitureMarkerNode*>(extra) : nullptr;
    if (!markerNode) {
        return false;
    }

    for (auto it = markerNode->markers.begin(); it != markerNode->markers.end(); ++it) {
        const auto& marker = *it;
        if (marker.animationType.get() == desiredType) {
            return true;
        }
    }

    return false;
}

std::string ClassifyFurnitureUseType(
    const std::string& furnitureName,
    const std::string& editorId,
    RE::TESFurniture* furnitureForm,
    RE::TESObjectREFR* furnitureRef = nullptr,
    bool* prefersLean = nullptr)
{
    if (prefersLean) {
        *prefersLean = false;
    }

    std::string combined = ToLowerCopyPlugin(furnitureName + " " + editorId);

    auto contains = [&combined](std::initializer_list<const char*> needles) {
        for (const auto* needle : needles) {
            if (combined.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    if (contains({"alchemy"})) {
        return "alchemy_lab";
    }
    if (contains({"enchant"})) {
        return "enchanting_table";
    }
    if (contains({"grindstone"})) {
        return "grindstone";
    }
    if (contains({"workbench"})) {
        return "workbench";
    }
    if (contains({"forge"})) {
        return "forge";
    }
    if (contains({"smelter"})) {
        return "smelter";
    }
    if (contains({"tanning"})) {
        return "tanning_rack";
    }
    if (contains({"cook", "cooking", "spit"})) {
        return "cooking_pot";
    }
    if (contains({"chop", "wood"})) {
        return "chopping_block";
    }
    if (contains({"bed", "sleep"})) {
        return "bed";
    }

    if (furnitureForm) {
        const auto benchType = static_cast<RE::TESFurniture::WorkBenchData::BenchType>(
            furnitureForm->workBenchData.benchType.underlying());
        if (benchType == RE::TESFurniture::WorkBenchData::BenchType::kAlchemy ||
            benchType == RE::TESFurniture::WorkBenchData::BenchType::kAlchemyExperiment) {
            return "alchemy_lab";
        }
        if (benchType == RE::TESFurniture::WorkBenchData::BenchType::kEnchanting ||
            benchType == RE::TESFurniture::WorkBenchData::BenchType::kEnchantingExperiment) {
            return "enchanting_table";
        }
        if (benchType == RE::TESFurniture::WorkBenchData::BenchType::kSmithingWeapon ||
            benchType == RE::TESFurniture::WorkBenchData::BenchType::kSmithingArmor ||
            benchType == RE::TESFurniture::WorkBenchData::BenchType::kCreateObject) {
            return "workbench";
        }
    }

    if (FurnitureHasMarkerAnimation(furnitureRef, RE::BSFurnitureMarker::AnimationType::kLean)) {
        if (prefersLean) {
            *prefersLean = true;
        }
        return "furniture";
    }
    if (FurnitureHasMarkerAnimation(furnitureRef, RE::BSFurnitureMarker::AnimationType::kSleep)) {
        return "bed";
    }
    if (FurnitureHasMarkerAnimation(furnitureRef, RE::BSFurnitureMarker::AnimationType::kSit)) {
        return "chair";
    }

    if (furnitureForm) {
        if (furnitureForm->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanLean)) {
            if (prefersLean) {
                *prefersLean = true;
            }
            return "furniture";
        }
        if (furnitureForm->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep)) {
            return "bed";
        }
        if (furnitureForm->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSit)) {
            return "chair";
        }
    }

    if (contains({"chair", "stool", "bench", "throne"})) {
        return "chair";
    }

    return combined.empty() ? "" : "furniture";
}

bool IsActorMovingForStatus(RE::Actor* actor, bool isRunning, RE::ActorState* actorState)
{
    if (!actor) {
        return false;
    }

    bool isMoving = isRunning;
    if (actorState && actorState->IsWalking()) {
        isMoving = true;
    }

    const auto formId = actor->GetFormID();
    const auto currentPos = actor->GetPosition();
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(g_activityStatusMutex);
        auto positionIt = g_lastActivityPosition.find(formId);
        auto timeIt = g_lastActivitySampleTime.find(formId);
        if (positionIt != g_lastActivityPosition.end() && timeIt != g_lastActivitySampleTime.end()) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - timeIt->second).count();
            if (elapsedMs >= 250) {
                const float movedDistance = currentPos.GetDistance(positionIt->second);
                if (movedDistance > 24.0f) {
                    isMoving = true;
                }
            }
        }

        g_lastActivityPosition[formId] = currentPos;
        g_lastActivitySampleTime[formId] = now;
    }

    return isMoving;
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

json BuildTransformationStatePayload(RE::Actor* actor, const std::string& actorName, const std::string& actorType = "npc")
{
    if (!actor || actor->IsDeleted() || actor->IsDisabled()) {
        return json();
    }

    const auto normalizedName = trim(actorName);
    if (normalizedName.empty()) {
        return json();
    }

    auto* race = actor->GetRace();
    const std::string raceName = race && race->GetFullName() ? trim(race->GetFullName()) : "";
    const std::string raceEditorId = race && race->GetFormEditorID() ? trim(race->GetFormEditorID()) : "";
    const std::string normalizedRaceName = ToLowerAscii(raceName);
    const std::string normalizedRaceEditorId = ToLowerAscii(raceEditorId);

    const bool isWerewolfForm =
        normalizedRaceEditorId.find("werewolf") != std::string::npos || normalizedRaceName == "werewolf";
    const bool isVampireLordForm =
        normalizedRaceEditorId.find("vampirebeast") != std::string::npos ||
        normalizedRaceEditorId.find("vampirelord") != std::string::npos ||
        normalizedRaceName.find("vampire lord") != std::string::npos;

    std::string state = "normal";
    if (isWerewolfForm) {
        state = "werewolf";
    } else if (isVampireLordForm) {
        state = "vampire_lord";
    }

    json payload;
    payload["type"] = "transformation_state";
    payload["actor_name"] = normalizedName;
    payload["actor_type"] = actorType;
    payload["timestamp"] = getCurrentTimeMillis();
    payload["gamets"] = GetGameTimeStamp();
    payload["state"] = state;
    payload["is_werewolf_form"] = isWerewolfForm;
    payload["is_vampire_lord_form"] = isVampireLordForm;
    payload["race_name"] = raceName;
    payload["race_editor_id"] = raceEditorId;

    return payload;
}

json BuildActivityStatusPayload(RE::Actor* npc, const std::string& agentName, const std::string* furnitureOverride = nullptr)
{
    if (!npc || npc->IsDeleted() || npc->IsDisabled() || !npc->GetActorRuntimeData().currentProcess) {
        return json();
    }

    const auto normalizedName = trim(agentName);
    if (normalizedName.empty()) {
        return json();
    }

    auto* actorState = npc->AsActorState();
    const bool isDead = npc->IsDead();
    const bool isUnconscious = actorState && actorState->IsUnconscious();
    const bool isInCombat = !isDead && npc->IsInCombat();
    const bool isAttacking = !isDead && npc->IsAttacking();
    const bool isRunning = !isDead && npc->IsRunning();
    const bool isSneaking = !isDead && npc->IsSneaking();
    const bool isWeaponDrawn = actorState ? actorState->IsWeaponDrawn() : false;

    auto sitSleepState = actorState ? actorState->GetSitSleepState() : RE::SIT_SLEEP_STATE::kNormal;
    const bool isSitting = sitSleepState == RE::SIT_SLEEP_STATE::kIsSitting;
    const bool isSleeping = sitSleepState == RE::SIT_SLEEP_STATE::kIsSleeping;
    const bool isMoving = !isDead && IsActorMovingForStatus(npc, isRunning, actorState);

    std::string furnitureName;
    std::string furnitureEditorId;
    std::string useType;
    bool prefersLean = false;

    auto furnitureHandle = npc->GetOccupiedFurniture();
    if (furnitureHandle) {
        auto furnitureRef = furnitureHandle.get().get();
        if (furnitureRef) {
            furnitureName = trim(furnitureRef->GetDisplayFullName());
            auto* furnitureBase = furnitureRef->GetBaseObject();
            furnitureEditorId = furnitureBase && furnitureBase->GetFormEditorID()
                ? trim(furnitureBase->GetFormEditorID())
                : "";
            auto* furnitureForm = furnitureBase ? furnitureBase->As<RE::TESFurniture>() : nullptr;
            useType = ClassifyFurnitureUseType(furnitureName, furnitureEditorId, furnitureForm, furnitureRef, &prefersLean);
        }
    }

    if (useType.empty() && furnitureOverride != nullptr) {
        furnitureName = trim(*furnitureOverride);
        if (!furnitureName.empty()) {
            useType = ClassifyFurnitureUseType(furnitureName, "", nullptr, nullptr, &prefersLean);
        }
    }

    const bool hasNonSeatUse = !useType.empty() && useType != "chair" && useType != "bed";
    const bool isLeaningUse = prefersLean || (useType == "furniture" && isSitting);

    std::string currentAction = "idle";
    if (isDead) {
        currentAction = "dead";
    } else if (isUnconscious) {
        currentAction = "unconscious";
    } else if (isSleeping || useType == "bed") {
        currentAction = "sleeping";
    } else if (isAttacking) {
        currentAction = "attacking";
    } else if (isInCombat) {
        currentAction = "combat";
    } else if (hasNonSeatUse) {
        currentAction = isLeaningUse ? "leaning" : "using";
    } else if (isSitting || useType == "chair") {
        currentAction = "sitting";
    } else if (!useType.empty()) {
        currentAction = "using";
    } else if (isSneaking) {
        currentAction = "sneaking";
    } else if (isRunning) {
        currentAction = "running";
    } else if (isMoving) {
        currentAction = "moving";
    }

    std::string attackTargetName;
    auto agent = AIAgentManager::getInstance().getAgentByFormId(npc->GetFormID());
    if (agent) {
        auto* attackTarget = agent->getAttackTarget();
        if (attackTarget) {
            attackTargetName = trim(attackTarget->GetDisplayFullName());
        }
    }

    json payload;
    payload["type"] = "activity_status";
    payload["actor_name"] = normalizedName;
    payload["actor_type"] = "npc";
    payload["timestamp"] = getCurrentTimeMillis();
    payload["gamets"] = GetGameTimeStamp();
    payload["current_action"] = currentAction;
    payload["current_use"] = useType;
    payload["use_type"] = useType;
    payload["furniture_name"] = furnitureName;
    payload["is_in_combat"] = isInCombat;
    payload["is_attacking"] = isAttacking;
    payload["is_moving"] = isMoving;
    payload["is_running"] = isRunning;
    payload["is_sneaking"] = isSneaking;
    payload["is_sitting"] = isSitting;
    payload["is_sleeping"] = isSleeping;
    payload["is_unconscious"] = isUnconscious;
    payload["is_dead"] = isDead;
    payload["is_weapon_drawn"] = isWeaponDrawn;

    if (!attackTargetName.empty()) {
        payload["attack_target"] = attackTargetName;
    }

    return payload;
}

// Forward declarations for metadata refresh functions
void RefreshAIAgentEquipment(RE::Actor* npc, const std::string& agentName, bool forceUpdate = false);
void RefreshAIAgentInventory(RE::Actor* npc, const std::string& agentName, bool forceUpdate, bool synchronous);
void RefreshAIAgentSkills(RE::Actor* npc, const std::string& agentName, bool forceUpdate = false);
void RefreshAIAgentStats(RE::Actor* npc, const std::string& agentName, bool forceUpdate = false);
void RefreshAIAgentActivityStatus(RE::Actor* npc, const std::string& agentName, const std::string* furnitureOverride = nullptr);
void RefreshAIAgentTransformationState(RE::Actor* npc, const std::string& agentName);
void RefreshAIAgentFurniture(RE::Actor* npc, const std::string& agentName, std::string furniture);
void PostNearbyActivityStatus(RE::PlayerCharacter* player, float radius);

static void AddCachedSpeechAudience(json& speechPayload, const std::string& reason)
{
    auto snapshot = SpatialSnapshotManager::GetPlayerSnapshot(false, reason);
    std::set<std::string> seen;
    json companions = json::array();

    auto appendCompanion = [&](const std::string& rawName) {
        const std::string name = trim(rawName);
        if (name.empty() || seen.find(name) != seen.end()) {
            return;
        }
        seen.insert(name);
        companions.push_back(name);
    };

    for (const auto& audibleActor : snapshot.audibleActors) {
        appendCompanion(audibleActor.label);
    }

    if (speechPayload.contains("listener") && speechPayload["listener"].is_string()) {
        appendCompanion(speechPayload["listener"].get<std::string>());
    }
    if (speechPayload.contains("speaker") && speechPayload["speaker"].is_string()) {
        appendCompanion(speechPayload["speaker"].get<std::string>());
    }
    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
        appendCompanion(player->GetName());
    }

    if (companions.empty()) {
        return;
    }

    speechPayload["companions"] = companions;
    if (!snapshot.audibleActors.empty()) {
        speechPayload["spatial_reason"] = "cached_dialogue_scope";
        speechPayload["spatial_volume"] = 1.0f;
    }
}

    // Forward declarations for player metadata refresh functions
void RefreshPlayerEquipment(bool forceUpdate = false);
void RefreshPlayerInventory(bool forceUpdate = false);
void RefreshPlayerSkills(bool forceUpdate = false);
void RefreshPlayerStats(bool forceUpdate = false);
void RefreshPlayerTransformationState(bool forceUpdate = false);

// Forward declarations for spell tracking functions
void RefreshAIAgentSpells(RE::Actor* npc, const std::string& agentName, bool forceUpdate = false);
void RefreshPlayerSpells(bool forceUpdate = false);

// Track last known equipment/inventory/skills/stats hashes (declared here for use in ManagerMainQueue)
extern std::unordered_map<uint32_t, std::string> lastEquipmentHash;
extern std::unordered_map<uint32_t, std::string> lastInventoryHash;
extern std::unordered_map<uint32_t, std::string> lastSkillsHash;
extern std::unordered_map<uint32_t, std::string> lastStatsHash;
extern std::unordered_map<uint32_t, std::string> lastSpellsHash;
extern std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> lastSkillsUpdate;
extern std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> lastStatsUpdate;




class ManagerMainQueue {
public:
    static ManagerMainQueue& getInstance() {
        static ManagerMainQueue instance;
        return instance;
    }

    void startThread(int poolinterval) {
        logger::info("[ManagerMainQueue] Attempting to start with pool interval: {}", poolinterval);
        poolInterval = poolinterval;
        if (!threadRunning) {
            threadRunning = true;
            try {
                // Use timeout of 0ms to indicate no timeout for the main queue processor
                ThreadPool::getInstance().enqueue("ManagerMainQueue", [this]() {
                    this->threadFunction();
                }, "", std::chrono::milliseconds(0));  // No timeout
                logger::info("[ManagerMainQueue] Thread started successfully via ThreadPool (no timeout)");
            } catch (const std::exception& e) {
                threadRunning = false;
                logger::error("[ManagerMainQueue] Failed to start thread: {}", e.what());
                throw;
            }
        } else {
            logger::warn("[ManagerMainQueue] Thread already running, ignoring start request");
        }
    }

    void stopThread() {
        logger::info("[ManagerMainQueue] Attempting to stop thread");
        threadRunning = false;
        logger::info("[ManagerMainQueue] Thread stop signal sent");
    }

    bool isRunning() { 
        return threadRunning;
    }

private:
    ManagerMainQueue() : threadRunning(false) {
        logger::info("[ManagerMainQueue] Instance created");
    }

    void threadFunction() {
        logger::info("[ManagerMainQueue] Thread function started");
        auto lastHealthCheck = std::chrono::high_resolution_clock::now();
        auto lastAgentMaintenanceAt = std::chrono::steady_clock::now() - std::chrono::seconds(20);
        auto lastAgentMaintenanceDeferLogAt = std::chrono::steady_clock::now() - std::chrono::seconds(5);
        auto lastAutoAddMaintenanceAt = std::chrono::steady_clock::now();
        auto lastBoredBusyLogAt = std::chrono::steady_clock::now() - std::chrono::seconds(30);
        std::size_t agentMaintenanceCursor = 0;
        int consecutiveErrors = 0;
        
        auto lastStatusReport = std::chrono::steady_clock::now();
        // Full thread-pool dumps write one line per worker thread. Keep them out of normal gameplay cadence;
        // queue-full paths still dump immediately when there is an actual overload.
        const auto statusReportInterval = std::chrono::minutes(10);
        while (threadRunning) {
            // Report thread pool status periodically
            auto now = std::chrono::steady_clock::now();
            if (now - lastStatusReport >= statusReportInterval) {
                ThreadPool::getInstance().logStatus();
                lastStatusReport = now;
            }

           try {
                // Health check logging every 5 minutes
                auto currentTime = std::chrono::high_resolution_clock::now();
                if (std::chrono::duration_cast<std::chrono::minutes>(currentTime - lastHealthCheck).count() >= 5) {
                    // Perform health checks
                    bool isHealthy = true;
                    std::stringstream healthStatus;
                    healthStatus << "[ManagerMainQueue] Health Status:\n";


                    // Check consecutive errors
                    if (consecutiveErrors > 10) {
                        isHealthy = false;
                        healthStatus << "- WARNING: High number of consecutive errors: " << consecutiveErrors << "\n";
                    }

                    // Check thread pool status
                    auto& threadPool = ThreadPool::getInstance();
                    size_t activeThreads = threadPool.getActiveThreadCount();
                    size_t queueSize = threadPool.getQueueSize();
                    size_t completedTasks = threadPool.getTotalTasksCompleted();
                    size_t timedOutTasks = threadPool.getTotalTasksTimedOut();

                    // Check for thread pool health issues
                    if (queueSize > 50) {  // High queue size might indicate processing bottleneck
                        isHealthy = false;
                        healthStatus << "- WARNING: High task queue size: " << queueSize << "\n";
                    }

                    if (timedOutTasks > completedTasks / 10) {  // More than 10% timeout rate
                        isHealthy = false;
                        healthStatus << "- WARNING: High task timeout rate. Completed: " << completedTasks << ", Timeouts: " << timedOutTasks << "\n";
                    }

                    if (activeThreads > 8) {  // High thread utilization
                        isHealthy = false;
                        healthStatus << "- WARNING: High thread utilization: " << activeThreads << " active threads\n";
                    }

                    // Log appropriate message based on health status
                    if (isHealthy) {
                        healthStatus << "- All systems operating normally\n";
                        healthStatus << "- Active Threads: " << activeThreads << "\n";
                        healthStatus << "- Queue Size: " << queueSize << "\n";
                        healthStatus << "- Total Tasks Completed: " << completedTasks << "\n";
                        logger::info("{}", healthStatus.str());
                        consecutiveErrors = 0; // Reset error count if everything is healthy
                    } else {
                        logger::warn("{}", healthStatus.str());
                    }

                    lastHealthCheck = currentTime;
                }

                // Check if Skyrim is exiting.
                if (RE::PlayerCharacter::GetSingleton() == NULL) {
                    logger::warn("[ManagerMainQueue] PlayerCharacter singleton is null, possible game exit");
                    std::this_thread::sleep_for(std::chrono::seconds(poolInterval));
                    
                    continue;
                }

                if (isGameReady) {
                }

                auto game = RE::UI::GetSingleton();
                if (!game) {
                    logger::error("[ManagerMainQueue] UI singleton is null");
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }

                UpdatePlayerMenuDialogueGate();
                SyncQuestProgressionEnabledFromServer();
                MaybeRunScheduledQuestProgressionResync();
                PollQuestProgressionActions();

                const bool recordingActive = VoiceRecordControl::getInstance().getRecording();
                const bool hardWorldMaintenanceSuppressed = IsWorldMaintenanceSuppressed();
                const bool spatialRefinementSuppressed = SpatialSnapshotManager::IsPlayerSpatialSettling();
                const bool worldMaintenanceSuppressed =
                    hardWorldMaintenanceSuppressed || spatialRefinementSuppressed;

                if (!game->GameIsPaused()) {
                    if (!RE::UI::GetSingleton()->IsApplicationMenuOpen()) {
                        // logger::debug("[ManagerMainQueue] Processing cycle starting - Game active and menu closed");
                        VRItemAwareness::Tick();
                        PollPlayerGaze();
                        
                        if (recordingActive) {
                            // STT capture is latency-sensitive in VR. Do not run overlay refresh,
                            // context logging, item scans, status posts, or spatial refresh while
                            // the mic is held; the voice thread owns the player-input path until it
                            // flips recording false after STT dispatch.
                            logger::trace("[ManagerMainQueue] Recording active; skipping background context maintenance");
                        } else {
                            // Cheap Prisma/status reads stay live during cell-entry settling. The snapshot manager
                            // only queues expensive LOS/navmesh refinement after the settle window drains.
                            if (PrismaUIBridge::IsAvailable()) {
                                PrismaUIBridge::CheckAndUpdateCrosshairTarget();
                                PrismaUIBridge::CheckAndUpdateAIView();
                                PrismaUIBridge::CheckAndUpdateDebugger();
                                PrismaUIBridge::CheckAndUpdateStatusHUDTarget();
                                PrismaUIBridge::CheckAndUpdateChatboxControls();
                            }

                            if (!SpeakManager::getInstance().getProcessing()) {
                                SpatialSnapshotManager::UpdatePlayerSnapshotIncremental("manager_incremental_refresh", 2);
                            }

                            if (!worldMaintenanceSuppressed) {
                                // logger::trace("[ManagerMainQueue] Sending HTTP request for location: {}", GetPlayerLocation());
                                HTTPManager::log(std::format("request|{}|{}|(Context location: {}, {})|{}", getCurrentTimeMillis(),
                                                             GetGameTimeStamp(), GetPlayerLocation(), BuildCurrentWorldContextDetails(),
                                                             RE::PlayerCharacter::GetSingleton()->GetParentCell()->GetFormID()));

                                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - controlLastInfoSent);

                                // infonpc_close is a compatibility/context feed. Use the legacy nearby-NPC
                                // scan so it describes everyone around the player, not only managed agents.
                                if (elapsed > std::chrono::seconds(8)) {
                                    logger::debug("[ManagerMainQueue] Performing periodic NPC inspection");
                                    auto player = RE::PlayerCharacter::GetSingleton();
                                    auto result = InspectSurroundingsNavmesh(player->AsReference(), true, 3000, "/");
                                    if (REL::Module::GetRuntime() != REL::Module::Runtime::VR) {
                                        // Send nearby items context BEFORE infonpc_close (so it gets logged in same request)
                                        std::string itemsResult = InspectNearbyItems(player->AsReference(), 256.0f);
                                        if (!itemsResult.empty()) {
                                            HTTPManager::log(std::format("infoitems|{}|{}|{}", getCurrentTimeMillis(),
                                                                         GetGameTimeStamp(), "(items in range:" + itemsResult + ")"));
                                        }
                                    }

                                    if (!result.empty()) {
                                        result.append("/");
                                    }
                                    result.append(AIAgentManager::getInstance().getPlayerName());
                                    HTTPManager::log(std::format("infonpc_close|{}|{}|{}", getCurrentTimeMillis(),
                                                                 GetGameTimeStamp(), result));
                                    PostNearbyActivityStatus(player, 3000.0f);

                                    controlLastInfoSent = std::chrono::high_resolution_clock::now();
                                }
                            } else if (!hardWorldMaintenanceSuppressed) {
                                logger::trace("[ManagerMainQueue] Spatial settling; deferring broad context maintenance");
                            }
                        }
                    }

                    gameIsPausedPrinted = false;
                    auto& spgResponse = SPGResponse::getInstance();
                    auto newResponse = spgResponse.getFirstItem(std::string("ScriptQueue").c_str());

                    if (!newResponse.text.empty()) {
                        ScriptLine l = ScriptLine::parse(newResponse.text, newResponse.actor.c_str());
                        l.rechatGenerated = newResponse.rechatGenerated;

                        if (IsPlayerActorName(l.actor)) {  // Player has talk. Remove NPC speech.
                            SpeakManager::getInstance().deleteQueue();
                            if (SpeakManager::getInstance().getProcessing())
                                SpeakManager::getInstance().abortPlay(true);
                        }
                        SpeakManager::getInstance().insertInQueue(l);

                        responsePop("ScriptQueue");
                    }

                    SpeakManager::getInstance().refreshPendingPlayerSubtitle();

                    ScriptLine l = SpeakManager::getInstance().getFirstItem();

                    if (!l.actor.empty()) {
                        if (!SpeakManager::getInstance().getProcessing()) {
                            speakerManagerIsBusyPrinted = false;
                            
                            // Make a copy of the data needed by the thread
                            auto threadData = l;  // Copy the ScriptLine data
                            logger::info("[ManagerMainQueue] Creating processing thread for actor: {}", threadData.actor);
                            
                            try {
                                ThreadPool::getInstance().enqueue("ProcessActor", [threadData]() {
                                    auto recoverSpeakManagerState = [&threadData]() {
                                        SpeakManager::getInstance().recoverFromProcessingFailure(threadData.actor);
                                    };

                                    try {
                                        logger::info("[ProcessingThread] Starting processing for actor: {}", threadData.actor);
                                        auto actorname = threadData.actor;

                                        if (actorname == "rolemaster") {
                                            logger::info("[ProcessingThread] Processing rolemaster action: {}", threadData.action);
                                            SpeakManager::getInstance().dequeueFirstItem();
                                        } else if (IsPlayerActorName(actorname)) {
                                            logger::info("[ProcessingThread] Processing player action");
                                            SpeakManager::getInstance().processPlayer();
                                        } else {
                                            AIAgentManager& aiam = AIAgentManager::getInstance();
                                            auto agentPtr = aiam.getAgentByName(actorname);
                                            if (agentPtr) {
                                                if (agentPtr->getAvailable()) {
                                                    // Allow speech in bleedout (wounded/dying), but skip if unconscious (knocked out)
                                                    auto act = agentPtr->getActor();
                                                    if (act && act->AsActorState() && act->AsActorState()->IsUnconscious()) {
                                                        logger::info("[ProcessingThread] Skipping {} - unconscious (cannot speak)", agentPtr->getActorName());
                                                        SpeakManager::getInstance().dequeueFirstItem();
                                                        return;
                                                    }
                                                    logger::info("[ProcessingThread] Processing agent: {}", agentPtr->getActorName());
                                                    SpeakManager::getInstance().process(agentPtr.get());
                                                    logger::debug("[ProcessingThread] Processed agent: {}", agentPtr->getActorName());
                                                } else {
                                                    logger::warn("[ProcessingThread] Agent not available: {}", agentPtr->getActorName());
                                                }
                                            } else {
                                                logger::error("[ProcessingThread] Unknown actor: {}", actorname);
                                                auto agents = aiam.getAgents();
                                                logger::debug("[ProcessingThread] Available actors:");
                                                for (const auto& agent : agents) {
                                                    logger::debug("[ProcessingThread] - {}", agent->getActorName());
                                                }
                                                SpeakManager::getInstance().dequeueFirstItem();
                                            }
                                        }
                                        logger::info("[ProcessingThread] Completed processing for actor: {}", threadData.actor);
                                    } catch (const std::exception& e) {
                                        logger::error("[ProcessingThread] Error in processing thread: {}", e.what());
                                        recoverSpeakManagerState();
                                    } catch (...) {
                                        logger::error("[ProcessingThread] Unknown error in processing thread");
                                        recoverSpeakManagerState();
                                    }
                                }, threadData.actor, std::chrono::seconds(90));
                                logger::debug("[ManagerMainQueue] Successfully queued processing task for actor: {}", threadData.actor);
                            } catch (const std::exception& e) {
                                logger::error("[ManagerMainQueue] Failed to queue processing task: {}", e.what());
                            }
                        } else {
                            if (!speakerManagerIsBusyPrinted) {
                                logger::info("[ManagerMainQueue] Speaker manager is busy, actor: {}", l.actor);
                                speakerManagerIsBusyPrinted = true;
                            }
                        }

                    } else {
                        if (!l.subtitle.empty()) logger::info("Audio line with no actor");
                    }

                    processApprovedCommandQueue();
                    processActionConfirmationQueue();

                    newResponse = spgResponse.getFirstItem("command");
                    if (!newResponse.text.empty()) {
                        logger::info("[COMMAND_QUEUE] Processing command: {} for actor: {}", newResponse.text, newResponse.actor);
                        parseCommand(newResponse.text, newResponse.actor);
                    }

                    newResponse = spgResponse.getFirstItem("rolecommand");
                    if (!newResponse.text.empty()) {
                        logger::info("Rolemaster in action {}", newResponse.text);
                        parseRoleCommand(newResponse.text);
                    }

                    auto boredElapsedSeconds =
                        std::chrono::duration_cast<std::chrono::seconds>(now - controlLastBoredTriggerTS);

                    bool avoidBored = false;
                    bool playerInDialog=false;
                    auto player = RE::PlayerCharacter::GetSingleton();
                    

                    if (CheckScene(player->GetCurrentScene())) {
                        auto scene = player->GetCurrentScene();
                        
                        logger::info("[BORED] Avoiding bored as player in on scene {:#x}", player->GetCurrentScene()->GetFormID());
                        
                    }

                    if (RE::MenuTopicManager::GetSingleton()->unkB1) {
                        logger::info("[BORED] Avoiding bored event because player is in dialogue");
                        playerInDialog = true;
                    }

                    const bool playerSpeechSuppressActive = recordingActive || IsPlayerSpeechMaintenanceSuppressed();
                    avoidBored = playerSpeechSuppressActive || player->IsInCombat() || player->IsAttacking() || player->IsSneaking()
                        || CheckScene(player->GetCurrentScene()) || playerInDialog;

                    if (boredElapsedSeconds >= std::chrono::seconds(GlobalBoredEventTimeOut) && !avoidBored) {
                        controlLastBoredTriggerTS = currentTime;
                        logger::info("[BORED_TIMER] Timer triggered after {}s", boredElapsedSeconds.count());

                        auto lastTalkTime = SpeakManager::getInstance().getLastUsedTime();
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastTalkTime);
                        if (elapsed <= std::chrono::seconds(CLEANING_TIMEOUT+5)) {
                            logger::info("[BORED_TIMER] Skipped - recent dialogue ({}s ago)", elapsed.count());
                        }  else {
                            AIAgentManager& aiam = AIAgentManager::getInstance();
                            auto player = RE::PlayerCharacter::GetSingleton();
                            auto localPlayerCell = player->GetParentCell();
                            float maxDistance = DISTANCE_ACTIVATING_NPC_OUT;
                            if (localPlayerCell->IsInteriorCell()) maxDistance = DISTANCE_ACTIVATING_NPC_IN;

                            std::string beings = InspectManagedAgents(player->AsReference(), maxDistance, ",",
                                                                      DISTANCE_ACTIVATING_NPC_OUT);

                            auto randomActor = aiam.getLessBoredAgentNearby(beings);
                            if (randomActor) {
                                auto selectedActor = randomActor->getActor();
                                randomActor->incBoredEventsFired();
                                if (!SpeakManager::getInstance().hasItems() && selectedActor) {
                                    const char* displayName = selectedActor->GetDisplayFullName();
                                    const std::string selectedActorName = displayName ? displayName : "";
                                    logger::info("[BORED_TIMER] Event sent to {}", selectedActorName);
                                    ThreadPool::getInstance().enqueue("BoredEvent", [selectedActor, selectedActorName]() {
                                        SpeakManager::getInstance().startRechatChainForAutonomousEvent();
                                        HTTPManager::stream(std::format("bored|{}|{}|{}|{}", getCurrentTimeMillis(),
                                                                        GetGameTimeStamp(), GetPlayerLocation(),
                                                                        selectedActorName),
                                                            selectedActor);
                                    });
                                } else {
                                    logger::info("[BORED_TIMER] Skipped - no available NPCs or speech queue busy");
                                }
                            } else {
                                logger::info("[BORED_TIMER] Skipped - no suitable NPCs nearby");
                            }
                        }
                    } else if (avoidBored) {
                        if (now - lastBoredBusyLogAt >= std::chrono::seconds(30)) {
                            lastBoredBusyLogAt = now;
                            logger::debug("[BORED_TIMER] Skipped - player busy (combat:{} attack:{} sneak:{} scene:{} dialogue:{})",
                                          player->IsInCombat(), player->IsAttacking(), player->IsSneaking(),
                                          CheckScene(player->GetCurrentScene()), playerInDialog);
                        }
                    }

                    // DYNAMIC PROFILE TIMER LOGIC
                    auto dynamicProfileElapsedSeconds =
                        std::chrono::duration_cast<std::chrono::seconds>(now - controlLastDynamicProfileTS);

                    // One-time initialization log
                    if (!dynamicProfileLoggingInitialized) {
                        dynamicProfileLoggingInitialized = true;
                        logger::info("[DYNAMIC_TIMER] System active - {}s timeout ({}min)", 
                                   GlobalDynamicProfileTimeOut, GlobalDynamicProfileTimeOut / 60);
                    }

                    // Log timer status every 5 minutes for debugging
                    static auto lastTimerLog = std::chrono::high_resolution_clock::now();
                    auto timeSinceLastLog = std::chrono::duration_cast<std::chrono::minutes>(now - lastTimerLog);
                    if (timeSinceLastLog >= std::chrono::minutes(5)) {
                        lastTimerLog = now;
                        auto remaining = GlobalDynamicProfileTimeOut - dynamicProfileElapsedSeconds.count();
                        logger::info("[DYNAMIC_TIMER] Status: {}s elapsed, {}s remaining", 
                                   dynamicProfileElapsedSeconds.count(), remaining);
                    }

                    if (dynamicProfileElapsedSeconds >= std::chrono::seconds(GlobalDynamicProfileTimeOut)) {
                        // Reset timer immediately to prevent multiple triggers
                        controlLastDynamicProfileTS = currentTime;
                        logger::info("[DYNAMIC_TIMER] Timer triggered after {}s", dynamicProfileElapsedSeconds.count());

                        AIAgentManager& aiam = AIAgentManager::getInstance();
                        auto player = RE::PlayerCharacter::GetSingleton();
                        std::string beings = InspectManagedAgents(player->AsReference(), HERIKA_MAX_VISION_RANGE,
                                                                  ",", DISTANCE_ACTIVATING_NPC_OUT);
                        
                        // Collect all nearby NPC names for batch processing
                        std::vector<std::string> nearbyNPCs;
                        for (const auto& agent : aiam.getAgents()) {
                            if (agent->getActorName() == NARRATOR_NAME) {
                                continue; // Skip The Narrator (processed separately)
                            }
                            
                            if (beings.find(agent->getActorName()) != std::string::npos) {
                                nearbyNPCs.push_back(agent->getActorName());
                            }
                        }
                        
                        // Always include The Narrator in dynamic profile updates if configured
                        // The server will check if narrator has dynamic_profile enabled
                        nearbyNPCs.push_back(NARRATOR_NAME);

                        // Periodic skills update (every 5 minutes for nearby NPCs)
                        static auto lastSkillsRefreshTime = currentTime;
                        auto skillsElapsed = std::chrono::duration_cast<std::chrono::minutes>(currentTime - lastSkillsRefreshTime);
                        if (skillsElapsed >= std::chrono::minutes(5)) {
                            logger::info("[SKILLS_PERIODIC] Updating skills for {} nearby AI Agents", nearbyNPCs.size());
                            for (const auto& agent : aiam.getAgents()) {
                                if (beings.find(agent->getActorName()) != std::string::npos) {
                                    RefreshAIAgentSkills(agent->getActor(), agent->getActorName());
                                }
                            }
                            
                            // Also refresh player skills periodically
                            RefreshPlayerSkills();
                            
                            lastSkillsRefreshTime = currentTime;
                        }
                        
                        // Periodic spells update (every 5 minutes for nearby NPCs)
                        static auto lastSpellsRefreshTime = currentTime;
                        auto spellsElapsed = std::chrono::duration_cast<std::chrono::minutes>(currentTime - lastSpellsRefreshTime);
                        if (spellsElapsed >= std::chrono::minutes(5)) {
                            logger::info("[SPELLS_PERIODIC] Updating spells for {} nearby AI Agents", nearbyNPCs.size());
                            for (const auto& agent : aiam.getAgents()) {
                                if (beings.find(agent->getActorName()) != std::string::npos) {
                                    RefreshAIAgentSpells(agent->getActor(), agent->getActorName());
                                }
                            }
                            
                            // Also refresh player spells periodically
                            RefreshPlayerSpells();
                            
                            lastSpellsRefreshTime = currentTime;
                        }
                        
                        // Periodic stats update (every 15 seconds for nearby NPCs)
                        // Catches regeneration to full HP/MP/SP after combat
                        static auto lastStatsRefreshTime = currentTime;
                        auto statsElapsed = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastStatsRefreshTime);
                        if (statsElapsed >= std::chrono::seconds(15)) {
                            logger::trace("[STATS_PERIODIC] Checking stats for {} nearby AI Agents", nearbyNPCs.size());
                            int statsUpdated = 0;
                            int fullyHealed = 0;
                            
                            for (const auto& agent : aiam.getAgents()) {
                                if (beings.find(agent->getActorName()) != std::string::npos) {
                                    auto npc = agent->getActor();
                                    if (!npc || npc->IsDead()) continue;
                                    
                                    // Check if NPC is at full health before update
                                    auto stats = npc->AsActorValueOwner();
                                    if (stats) {
                                        float health = stats->GetActorValue(RE::ActorValue::kHealth);
                                        float healthMax = stats->GetBaseActorValue(RE::ActorValue::kHealth);
                                        float magicka = stats->GetActorValue(RE::ActorValue::kMagicka);
                                        float magickaMax = stats->GetBaseActorValue(RE::ActorValue::kMagicka);
                                        float stamina = stats->GetActorValue(RE::ActorValue::kStamina);
                                        float staminaMax = stats->GetBaseActorValue(RE::ActorValue::kStamina);
                                        
                                        bool wasFullyHealed = (health >= healthMax - 1.0f && 
                                                              magicka >= magickaMax - 1.0f && 
                                                              stamina >= staminaMax - 1.0f);
                                        
                                        // RefreshAIAgentStats has hash diffing - only sends if changed
                                        auto beforeHash = lastStatsHash[npc->GetFormID()];
                                        RefreshAIAgentStats(npc, agent->getActorName(), false);
                                        auto afterHash = lastStatsHash[npc->GetFormID()];
                                        
                                        if (beforeHash != afterHash) {
                                            statsUpdated++;
                                            
                                            // Check if they just reached full health
                                            if (wasFullyHealed) {
                                                fullyHealed++;
                                                logger::info("[STATS_PERIODIC] {} fully regenerated to max HP/MP/SP!", 
                                                           agent->getActorName());
                                            }
                                        }
                                    }
                                }
                            }
                            
                            if (statsUpdated > 0) {
                                logger::info("[STATS_PERIODIC] Updated stats for {}/{} nearby AI Agents (regen/changes detected, {} fully healed)", 
                                           statsUpdated, nearbyNPCs.size(), fullyHealed);
                            }
                            
                            // Also refresh player stats periodically
                            RefreshPlayerStats();
                            RefreshPlayerTransformationState();
                            
                            lastStatsRefreshTime = currentTime;
                        }

                        if (!nearbyNPCs.empty()) {
                            // Create comma-separated list of NPC names
                            std::string npcList;
                            for (size_t i = 0; i < nearbyNPCs.size(); ++i) {
                                if (i > 0) npcList += ",";
                                npcList += nearbyNPCs[i];
                            }
                            
                            logger::info("[DYNAMIC_TIMER] Updating {} NPCs: {}", nearbyNPCs.size(), npcList);
                            
                            // Send fire-and-forget async batch request to server
                            ThreadPool::getInstance().enqueue("DynamicProfileBatch", [npcList]() {
                                try {
                                    HTTPManager::log(std::format("updateprofiles_batch_async|{}|{}|{}", 
                                                                getCurrentTimeMillis(), GetGameTimeStamp(), npcList));
                                    logger::debug("[DYNAMIC_TIMER] Batch request sent successfully");
                                } catch (const std::exception& e) {
                                    logger::error("[DYNAMIC_TIMER] Failed to send batch request: {}", e.what());
                                }
                            });
                        } else {
                            logger::info("[DYNAMIC_TIMER] Skipped - no nearby AI agents found");
                        }
                    }

                    // COMBAT BARKS TIMER LOGIC
                    extern bool CombatBarksEnabled;
                    extern bool CombatDialogueEnabled;
                    // logger::trace("[COMBAT_BARK_DEBUG] Checking: CombatBarksEnabled={} CombatDialogueEnabled={}", CombatBarksEnabled, CombatDialogueEnabled);
                    if (!worldMaintenanceSuppressed && CombatBarksEnabled && CombatDialogueEnabled) {
                        static auto lastCombatBarkCheck = currentTime;
                        auto combatBarkElapsed = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastCombatBarkCheck);
                        //logger::trace("[COMBAT_BARK_DEBUG] Timer check: elapsed={}s, period={}s", combatBarkElapsed.count(), GlobalCombatBarksPeriod);
                        
                        if (combatBarkElapsed >= std::chrono::seconds(GlobalCombatBarksPeriod)) {
                            lastCombatBarkCheck = currentTime;
                            
                            // Find AI agents currently in combat
                            AIAgentManager& aiam = AIAgentManager::getInstance();
                            std::vector<AIAgent*> combatAgents;
                               for (const auto& agent : aiam.getAgents()) {
                                   auto actor = agent->getActor();
                                      if (actor && actor->IsInCombat() && IsActorLoadedInPlayerCell(actor)) {
                                        // Skip player/narrator
                                        if (actor->GetFormID() == RE::PlayerCharacter::GetSingleton()->GetFormID()) {
                                            continue;
                                        }

                                        combatAgents.push_back(agent.get());
                                      }
                                }
                            
                            if (!combatAgents.empty() && !SpeakManager::getInstance().hasItems()) {
                                // Pick random combat agent
                                int randomIndex = rand() % combatAgents.size();
                                auto selectedAgent = combatAgents[randomIndex];
                                auto selectedActor = selectedAgent->getActor();
                                
                                logger::info("[COMBAT_BARK] Triggering bark for {} ({} agents in combat)", 
                                            selectedAgent->getActorName(), combatAgents.size());
                                
                                auto selectedActorHandle = selectedActor->GetHandle();
                                ThreadPool::getInstance().enqueue("CombatBark", [selectedActorHandle]() {
                                    auto selectedActorRef = selectedActorHandle.get();
                                    auto* resolvedActor = selectedActorRef.get() ? selectedActorRef.get()->As<RE::Actor>() : nullptr;
                                    if (!resolvedActor || resolvedActor->IsDead() || !resolvedActor->IsInCombat() ||
                                        !IsActorLoadedInPlayerCell(resolvedActor)) {
                                        logger::debug("[COMBAT_BARK] Skipped stale or no-longer-combat actor");
                                        return;
                                    }

                                    HTTPManager::stream(std::format("combatbark|{}|{}|{}", 
                                                                   getCurrentTimeMillis(),
                                                                   GetGameTimeStamp(), 
                                                                   GetPlayerLocation()),
                                                       resolvedActor);
                                });
                            } else {
                                if (!combatAgents.empty()) {
                                    logger::trace("[COMBAT_BARK] Skipped - speech queue busy ({} agents in combat)", combatAgents.size());
                               }
                            }
                        }
                    }
                    
                    // Walk To Target timeout check - runs every 10 seconds
                    auto walkToTargetCheckElapsedSeconds =
                        std::chrono::duration_cast<std::chrono::seconds>(now - controlLastWalkToTargetCheckTS);
                    
                    if (walkToTargetCheckElapsedSeconds >= std::chrono::seconds(10)) {
                        controlLastWalkToTargetCheckTS = currentTime;
                        
                        // Call Papyrus function to check and release NPCs that haven't spoken
                        logger::debug("[WALKTOTARGET_TIMER] Checking WalkToTarget NPCs after {}s",
                                      walkToTargetCheckElapsedSeconds.count());
                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                        auto args = RE::MakeFunctionArguments();
                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                            "AIAgentAIMind", "CheckAndReleaseWalkToTargetNPCs", args, callback);
                    }
                } else {
                    if (!gameIsPausedPrinted) {
                        logger::debug("Game paused - timers paused");
                        gameIsPausedPrinted = true;
                    }
                    // Set the bored trigger to current time to avoid immediate trigger after menu
                    controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now() ;
                }

                const auto maintenanceNow = std::chrono::steady_clock::now();
                bool ranAgentMaintenance = false;
                const bool speechMaintenanceSuppressed = recordingActive || IsPlayerSpeechMaintenanceSuppressed();
                const bool speechProcessing = SpeakManager::getInstance().getProcessing();
                const bool deferHeavyAgentMaintenance =
                    speechMaintenanceSuppressed || speechProcessing || worldMaintenanceSuppressed;

                if (deferHeavyAgentMaintenance) {
                    if (maintenanceNow - lastAgentMaintenanceDeferLogAt >= std::chrono::seconds(5)) {
                        lastAgentMaintenanceDeferLogAt = maintenanceNow;
                        logger::trace("[AGENT_MAINT] Deferred speech={} speaking={} world_settling={}",
                                      speechMaintenanceSuppressed,
                                      speechProcessing,
                                      worldMaintenanceSuppressed);
                    }
                } else {  // Run agent maintenance only when player/NPC speech is not active.
                    // Heavy cleanup touches actor state, packages, and voice types. Keep it slower and budgeted;
                    // cheap auto-add below stays responsive so NPC detection does not depend on this pass.
                    if (maintenanceNow - lastAgentMaintenanceAt >= std::chrono::seconds(20)) {
                        lastAgentMaintenanceAt = maintenanceNow;
                        ranAgentMaintenance = true;
                        AIAgentManager& aiam = AIAgentManager::getInstance();
                        std::string beings = InspectManagedAgents(RE::PlayerCharacter::GetSingleton(), 15000, ",",
                                                                  DISTANCE_ACTIVATING_NPC_OUT);  // Check far far away
                        auto allAgentsForMaintenance = aiam.getAgents();
                        std::vector<std::shared_ptr<AIAgent>> maintenanceAgents;
                        constexpr std::size_t kMaxAgentMaintenanceChecksPerPass = 8;
                        if (!allAgentsForMaintenance.empty()) {
                            if (agentMaintenanceCursor >= allAgentsForMaintenance.size()) {
                                agentMaintenanceCursor = 0;
                            }

                            const std::size_t startCursor = agentMaintenanceCursor;
                            std::size_t visited = 0;
                            while (visited < allAgentsForMaintenance.size() &&
                                   maintenanceAgents.size() < kMaxAgentMaintenanceChecksPerPass) {
                                auto agent = allAgentsForMaintenance[(startCursor + visited) %
                                                                     allAgentsForMaintenance.size()];
                                if (agent) {
                                    maintenanceAgents.push_back(agent);
                                }
                                ++visited;
                            }
                            agentMaintenanceCursor = (startCursor + visited) % allAgentsForMaintenance.size();
                        }

                        constexpr int kPrecleanerMissingPresenceDeletePasses = 2;
                        constexpr std::size_t kMaxPrecleanerDeletesPerPass = 1;
                        std::vector<std::string> agentsToDelete;
                        auto queueAgentDelete = [&](const std::string& agentName) -> bool {
                            if (agentsToDelete.size() < kMaxPrecleanerDeletesPerPass) {
                                agentsToDelete.push_back(agentName);
                                return true;
                            }
                            return false;
                        };

                // Delete dead actors
                //logger::debug("[PRECLEANER] Evaluating");
                for (const auto& agent : maintenanceAgents) {
                        if (agent->isPresent(beings)) {
                            agent->resetMissingPresenceCounter();
                            if (agent->mustBeDeleted()) {
                                if (queueAgentDelete(agent->getActorName())) {
                                    logger::debug("[PRECLEANER] Actor is gonna be deleted because marked: {}",
                                                  agent->getActorName());
                                }
                        } else {
                            if (agent->getActor()) {
                                // We must ensure actor is really dead
                                auto actorForm = RE::TESForm::LookupByID(agent->GetFormId());
                                if (!actorForm) continue;
                                auto actor = RE::TESForm::LookupByID(agent->GetFormId())->As<RE::Actor>();
                                if (actor && actor->IsDead() && !agent->isNarrator()) {
                                    if (queueAgentDelete(agent->getActorName())) {
                                        logger::debug("[PRECLEANER] Actor is gonna be deleted because dead: {}",
                                                      agent->getActorName());
                                    }
                                }
                            }
                        }
                    } else {
                        // Delete non present actors
                        if (ENABLE_AUTOADDNPC) {
                            if (!agent->isNarrator() && agent->isClean() && agent->isRestored() &&
                                !agent->isManuallyAdded()) {
                                if (agent->mustBeDeleted()) {
                                    if (queueAgentDelete(agent->getActorName())) {
                                        logger::debug("[PRECLEANER] Actor is gonna be deleted because auto-managed stale: {}",
                                                      agent->getActorName());
                                    }
                                    continue;
                                }

                                agent->increaseMissingPresenceCounter();
                                const int missingPasses = agent->getMissingPresenceCounter();
                                if (missingPasses >= kPrecleanerMissingPresenceDeletePasses) {
                                    if (queueAgentDelete(agent->getActorName())) {
                                        logger::debug("[PRECLEANER] Actor is gonna be deleted because not present for {} passes: {}",
                                                      missingPasses, agent->getActorName());
                                    }
                                } else if (missingPasses == 1 || missingPasses % 30 == 0) {
                                    logger::debug("[PRECLEANER] Actor not present yet retained ({}/{}): {}",
                                                  missingPasses, kPrecleanerMissingPresenceDeletePasses,
                                                  agent->getActorName());
                                }
                            }
                        }
                    }
                }

                //logger::debug("[CLEANER] Evaluating");
                // Delete the agents by name after collecting their names
                for (const auto& name : agentsToDelete) {
                    aiam.deleteAgentByName(name);
                }

                auto agents = maintenanceAgents;
                bool performedActorCleanupThisPass = !agentsToDelete.empty();

                //logger::debug("[RESTORE] Evaluating {} agents ", agents.size());
                for (const auto& agent : agents) {
                    //logger::debug("Checking agent");
                    if (!agent) {
                        logger::warn("Null agent encountered in thread function");
                        continue;
                    }

                    try {
                        // Verify agent pointer is valid
                        if (!agent.get()) {
                            logger::warn("Invalid agent pointer");
                            continue;
                        }

                        // Add timeout to avoid deadlocks
                        bool isTalking = false;
                        bool isClean = false;
                        bool isRestored = false;

                        
                        
                        try {
                            // logger::debug("Checking agent {}", agent->getActorName());
                            /* isTalking is only used here. Better use isClean, as it has a timeout.
                            * 
                            isTalking = agent->isTalking();
                            if (isTalking) continue;
                            */
                            
                            auto elapsed =
                                std::chrono::duration_cast<std::chrono::seconds>(now - agent->GetLastTimeTalk());

                            isRestored=agent->isRestored();
                            isClean = agent->isClean();
                           
                            if (isClean && !isRestored) {
                                if (performedActorCleanupThisPass) {
                                    agent->SetLastTimeTalk();
                                    continue;
                                }
                                if (!game->GameIsPaused()) {    // We're gonna call some papyrus functions, make sure game is running
                                    if (elapsed >=
                                        std::chrono::seconds(91)) {  // To restore packages override after 30 seconds of no speech
                                        logger::debug("[RESTORE] Throwing out of the conversation : {}",
                                                      agent->getActorName());

                                        auto actorByForm = RE::TESForm::LookupByID(agent->GetFormId());
                                        if (actorByForm) {
                                            auto actor = actorByForm->As<RE::Actor>();

                                            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                            auto args = RE::MakeFunctionArguments(std::move(actor));
                                            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                                "AIAgentAIMind", "ReleaseFromConversation", args, callback);

                                            if (agent->getWasOnScene()) {
                                                // Try to restore scene
                                                
                                                logger::debug(
                                                    "[RESTORE] Calling papyrus function for EndDialogueClearScene");
                                                auto callback =
                                                    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                                auto args = RE::MakeFunctionArguments(std::move(actor));
                                                RE::BSScript::Internal::VirtualMachine::GetSingleton()
                                                    ->DispatchStaticCall("AIAgentAIMind", "EndDialogueClearScene", args,
                                                                         callback);

                                            } else {
                                                logger::debug(
                                                    "[RESTORE] Calling papyrus function for EndDialogueClear");
                                                auto callback =
                                                    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                                auto args = RE::MakeFunctionArguments(std::move(actor));
                                                RE::BSScript::Internal::VirtualMachine::GetSingleton()
                                                    ->DispatchStaticCall("AIAgentAIMind", "EndDialogueClear", args,
                                                                         callback);
                                            }
                                        } else {
                                            logger::debug("[RESTORE] Unable to restore: {}", agent->getActorName());
                                        }
                                        agent->setOnScene(false);
                                        agent->setRestored(true);
                                    }
                                    continue;
                                }
                            }
                            
                            if (isClean) 
                                continue;

                        } catch (const std::exception& e) {
                            logger::error("Error checking agent state: {}", e.what());
                            continue;
                        }

                        auto now = std::chrono::high_resolution_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - agent->GetLastTimeTalk());

                        if (elapsed <= std::chrono::seconds( CLEANING_TIMEOUT)) {
                            continue;
                        } else {
                            if (agent->isNarrator()) {
                                agent->setClean(true);
                                agent->setRestored(true);

                            } else {
                                if (agent->isPresent(beings)) {
                                    if (performedActorCleanupThisPass) {
                                        agent->SetLastTimeTalk();
                                        continue;
                                    }
                                    performedActorCleanupThisPass = true;

                                    if (agent->getActor()) {
                                        if (agent->getActor()->Is3DLoaded()) {  // 1.0.13
                                            auto fgen = agent->getActor()->GetFaceGenAnimationData();
                                            if (fgen) {
                                                RE::BSSpinLockGuard locker(fgen->lock);

                                                fgen->ClearExpressionOverride();
                                                // fgen->Reset(0.0f, true, true, true, false);
                                                for (int i = 0; i <= 15; i++) fgen->phenomeKeyFrame.SetValue(i, 0.0f);

                                                /*
                                                agent->getActor()->GetActorRuntimeData().currentProcess->Update3DModel(
                                                    agent->getActor());
                                                */
                                            }

                                        }

                                        agent->getActor()->AllowPCDialogue(true);

                                        // Moved here 1.0.13
                                        if (agent->getActor() && !agent->isNarrator()) {
                                            if (agent->getActor()->IsDead()) {
                                                agent->markToBeDeleted();
                                                continue;
                                            }
                                        }
                                    }
                                    agent->setClean(true);
                                    agent->setAvailable(true);
                                    auto npc = agent->getActor();
                                    if (npc) {
                                        npc->AllowPCDialogue(true);
                                    }
                                    agent->resetForgetCleanCounter();  // 1,0.14 Forget about this actor
                                    // agent->getActor()->EndDialogue(); // 1.0.13
                                    if (agent->getOriginalVoice() != nullptr) {
                                        agent->getActor()->GetActorBase()->voiceType = agent->getOriginalVoice();
                                    }

                                    logger::info("[CLEANER] Clean: Restoring voice for {} {:#x} ",
                                                 npc->GetDisplayFullName(), agent->getOriginalVoice()->formID);

                                } else {  // Actor not present

                                    agent->increaseForgetCleanCounter();
                                    if (agent->getForgetCleanCounter() > 12) {  // Dirty clean
                                        if (performedActorCleanupThisPass) {
                                            agent->SetLastTimeTalk();
                                            continue;
                                        }
                                        performedActorCleanupThisPass = true;

                                        logger::info("[CLEANER] Dirty clean {}, not present. ", agent->getActorName());
                                        auto actorByForm = RE::TESForm::LookupByID(agent->GetFormId());
                                        if (actorByForm) {
                                            auto actor = actorByForm->As<RE::Actor>();
                                            if (actor) {
                                                if (agent->getOriginalVoice() != nullptr) {
                                                    logger::info("[CLEANER] Dirty: Restoring voice for {} {:#x} ",
                                                                 actor->GetDisplayFullName(),
                                                                 agent->getOriginalVoice()->formID);
                                                    actor->GetActorBase()->voiceType = agent->getOriginalVoice();
                                                    actor->AllowPCDialogue(true);
                                                }
                                            }
                                        }

                                        agent->setClean(true);
                                        //agent->setRestored(true);
                                        agent->setAvailable(true);

                                    } else {
                                        logger::info("[CLEANER]  NOT Cleaning state for {}, not present. Will do later",
                                                     agent->getActorName());
                                        agent->SetLastTimeTalk();  // Will check again in CLEANING_TIMEOUT secs.
                                    }
                                }
                            }
                        }  // agent->getActor()->EvaluatePackage(false, false);

                    } catch (const std::exception& e) {
                        logger::error("Error processing agent: {}", e.what());
                        continue;
                    }
                }

                }

                }  // !deferHeavyAgentMaintenance

                // Crosshair promotion stays outside broad maintenance gates so the chosen NPC can enter the list immediately.
                const bool crosshairPromotionHandled = ENABLE_AUTOADDNPC && promoteCrosshairTargetToAI();

                // Auto-add handles spatial settling internally so close/crosshair NPCs can promote quickly.
                if (!crosshairPromotionHandled && !hardWorldMaintenanceSuppressed &&
                    !ranAgentMaintenance && ENABLE_AUTOADDNPC &&
                    maintenanceNow - lastAutoAddMaintenanceAt >= std::chrono::milliseconds(500)) {
                    lastAutoAddMaintenanceAt = maintenanceNow;
                    addAllNPC();
                }


                //logger::debug("[RESTORE] END OF ITERATION");
                //std::this_thread::sleep_for(std::chrono::seconds(poolInterval));
                std::this_thread::sleep_for(std::chrono::milliseconds(500));  // Slight delay to reduce CPU usage
                consecutiveErrors = 0; // Reset error counter on successful iteration
            } catch (const std::exception& e) {
                consecutiveErrors++;
                logger::error("[ManagerMainQueue] Error in thread function (attempt {}): {}", consecutiveErrors, e.what());
                
                if (consecutiveErrors >= 3) {
                    logger::critical("[ManagerMainQueue] Thread encountered too many consecutive errors, attempting recovery");
                    std::this_thread::sleep_for(std::chrono::seconds(5)); // Longer sleep for recovery
                } else {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        }
        logger::info("[ManagerMainQueue] Thread function ending");
    }

    bool threadRunning;
    int poolInterval;
    bool gameIsPausedPrinted = false;
    bool speakerManagerIsBusyPrinted = false;
};


namespace ProcessorMenu {
    // Structure to hold inventory snapshot for crafting tracking
    struct InventorySnapshot {
        std::unordered_map<RE::FormID, std::pair<std::string, int32_t>> items; // FormID -> {itemName, count}
        bool active = false;
    };

    struct TradeLineItem {
        int32_t count = 0;
        int32_t totalGoldValue = 0;
    };

    struct BarterSession {
        bool active = false;
        RE::FormID merchantFormID = 0;
        std::string merchantName;
        InventorySnapshot playerInventoryAtOpen;
    };

    struct PendingBarterMerchant {
        bool active = false;
        RE::FormID merchantFormID = 0;
        std::string merchantName;
        std::chrono::steady_clock::time_point capturedAt = std::chrono::steady_clock::time_point::min();
    };

    struct RecentConversationTarget {
        bool active = false;
        RE::FormID actorFormID = 0;
        std::string actorName;
        std::chrono::steady_clock::time_point capturedAt = std::chrono::steady_clock::time_point::min();
    };
    
    static InventorySnapshot craftingInventorySnapshot;
    static BarterSession barterSession;
    static PendingBarterMerchant pendingBarterMerchant;
    static RecentConversationTarget recentConversationTarget;
    
    // Helper function to check if player is currently in a crafting session
    bool IsCraftingActive() {
        return craftingInventorySnapshot.active;
    }

    void CaptureInventorySnapshot(InventorySnapshot& snapshot);

    void ResetBarterSession() {
        barterSession = {};
    }

    void ClearPendingBarterMerchant() {
        pendingBarterMerchant = {};
    }

    bool IsMerchantCandidatePlayer(RE::FormID formID, const std::string& name)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && formID != 0 && formID == player->GetFormID()) {
            return true;
        }

        return IsPlayerActorName(name);
    }

    void RememberRecentConversationTarget(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        const std::string actorName = trim(actor->GetDisplayFullName());
        if (actorName.empty() || IsPlayerActorName(actorName)) {
            return;
        }

        recentConversationTarget.active = true;
        recentConversationTarget.actorFormID = actor->GetFormID();
        recentConversationTarget.actorName = actorName;
        recentConversationTarget.capturedAt = std::chrono::steady_clock::now();
    }

    std::pair<RE::FormID, std::string> ResolveConversationMerchantCandidate()
    {
        if (auto* tm = RE::MenuTopicManager::GetSingleton()) {
            if (auto speakerHandle = tm->speaker.get()) {
                if (auto* speakerActor = speakerHandle->As<RE::Actor>()) {
                    const std::string speakerName = trim(speakerActor->GetDisplayFullName());
                    if (!speakerName.empty() && !IsPlayerActorName(speakerName)) {
                        return {speakerActor->GetFormID(), speakerName};
                    }
                }
            }
        }

        constexpr auto kRecentConversationTargetTimeout = std::chrono::seconds(20);
        if (!recentConversationTarget.active) {
            return {0, ""};
        }

        const auto now = std::chrono::steady_clock::now();
        if ((now - recentConversationTarget.capturedAt) > kRecentConversationTargetTimeout) {
            recentConversationTarget = {};
            return {0, ""};
        }

        if (recentConversationTarget.actorName.empty() ||
            IsPlayerActorName(recentConversationTarget.actorName)) {
            return {0, ""};
        }

        return {recentConversationTarget.actorFormID, recentConversationTarget.actorName};
    }

    RE::Actor* ResolveBarterMerchantActor() {
        auto targetHandle = RE::BarterMenu::GetTargetRefHandle();
        if (targetHandle == 0) {
            return nullptr;
        }

        auto targetRef = RE::TESObjectREFR::LookupByHandle(targetHandle);
        if (!targetRef) {
            return nullptr;
        }

        return targetRef.get()->As<RE::Actor>();
    }

    std::pair<RE::FormID, std::string> ConsumePendingBarterMerchant() {
        constexpr auto kPendingBarterMerchantTimeout = std::chrono::seconds(8);

        if (!pendingBarterMerchant.active) {
            return {0, ""};
        }

        const auto now = std::chrono::steady_clock::now();
        if ((now - pendingBarterMerchant.capturedAt) > kPendingBarterMerchantTimeout) {
            ClearPendingBarterMerchant();
            return {0, ""};
        }

        const auto merchantFormID = pendingBarterMerchant.merchantFormID;
        const auto merchantName = pendingBarterMerchant.merchantName;
        ClearPendingBarterMerchant();
        return {merchantFormID, merchantName};
    }

    RE::Actor* ResolveStoredBarterMerchantActor() {
        if (barterSession.merchantFormID == 0) {
            return nullptr;
        }

        auto merchantForm = RE::TESForm::LookupByID(barterSession.merchantFormID);
        if (!merchantForm) {
            return nullptr;
        }

        return merchantForm->As<RE::Actor>();
    }

    std::string GetBarterPlayerLabel()
    {
        const std::string configuredPlayerName = trim(AIAgentManager::getInstance().getPlayerName());
        if (!configuredPlayerName.empty()) {
            return configuredPlayerName;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            const std::string playerDisplayName = trim(player->GetDisplayFullName());
            if (!playerDisplayName.empty() && !EqualsIgnoreCasePlugin(playerDisplayName, NARRATOR_NAME)) {
                return playerDisplayName;
            }

            const std::string playerName = trim(player->GetDisplayFullName());
            if (!playerName.empty()) {
                return playerName;
            }
        }

        return "the player";
    }

    void StartBarterSession() {
        ResetBarterSession();
        barterSession.active = true;

        const auto [pendingMerchantFormID, pendingMerchantName] = ConsumePendingBarterMerchant();
        if (pendingMerchantFormID != 0) {
            barterSession.merchantFormID = pendingMerchantFormID;
            barterSession.merchantName = trim(pendingMerchantName);
        } else if (auto* merchant = ResolveBarterMerchantActor()) {
            barterSession.merchantFormID = merchant->GetFormID();
            barterSession.merchantName = trim(merchant->GetDisplayFullName());
        }

        if (barterSession.merchantName.empty()) {
            barterSession.merchantName = "The merchant";
        }

        if (IsMerchantCandidatePlayer(barterSession.merchantFormID, barterSession.merchantName)) {
            const auto [conversationMerchantFormID, conversationMerchantName] =
                ResolveConversationMerchantCandidate();
            if (conversationMerchantFormID != 0 && !conversationMerchantName.empty()) {
                barterSession.merchantFormID = conversationMerchantFormID;
                barterSession.merchantName = conversationMerchantName;
            }
        }

        CaptureInventorySnapshot(barterSession.playerInventoryAtOpen);

        logger::info("[BARTER_MENU] Started barter session with {}", barterSession.merchantName);
    }

    bool IsGoldForm(const RE::TESForm* itemForm) {
        return itemForm && itemForm->GetFormID() == 0x0000000F;
    }

    void AddTradeItem(std::map<std::string, TradeLineItem>& items, const std::string& itemName, int32_t count, int32_t totalGoldValue) {
        if (count <= 0) {
            return;
        }

        std::string key = trim(itemName);
        if (key.empty()) {
            key = "Unknown item";
        }

        auto& entry = items[key];
        entry.count += count;
        entry.totalGoldValue += std::max(totalGoldValue, 0);
    }

    bool RecordBarterTransfer(RE::FormID oldContainer, RE::FormID newContainer, int32_t itemCount, const RE::TESForm* itemForm, const std::string& itemName) {
        if (!barterSession.active) {
            return false;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }

        const auto playerID = player->GetFormID();
        const bool playerGave = oldContainer == playerID;
        const bool playerReceived = newContainer == playerID;
        if (!playerGave && !playerReceived) {
            return false;
        }

        return true;
    }

    int32_t GetSnapshotCount(const InventorySnapshot& snapshot, RE::FormID formID)
    {
        auto it = snapshot.items.find(formID);
        if (it == snapshot.items.end()) {
            return 0;
        }
        return it->second.second;
    }

    std::string GetSnapshotName(const InventorySnapshot& snapshot, RE::FormID formID)
    {
        auto it = snapshot.items.find(formID);
        if (it == snapshot.items.end()) {
            return "";
        }
        return it->second.first;
    }

    std::string FormatTradeItemList(const std::map<std::string, TradeLineItem>& items) {
        std::vector<std::string> parts;
        parts.reserve(items.size());

        for (const auto& [itemName, lineItem] : items) {
            if (lineItem.count <= 0) {
                continue;
            }

            if (lineItem.count == 1) {
                parts.push_back(std::format("1 {} ({} gold)", itemName, lineItem.totalGoldValue));
            } else {
                parts.push_back(std::format("{} {} ({} gold total)", lineItem.count, itemName, lineItem.totalGoldValue));
            }
        }

        std::ostringstream stream;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) {
                stream << ", ";
            }
            stream << parts[i];
        }

        return stream.str();
    }

    void FinalizeBarterSession() {
        if (!barterSession.active) {
            return;
        }

        InventorySnapshot currentInventorySnapshot;
        CaptureInventorySnapshot(currentInventorySnapshot);

        std::map<std::string, TradeLineItem> merchantSoldItems;
        std::map<std::string, TradeLineItem> playerSoldItems;
        int32_t goldBefore = GetSnapshotCount(barterSession.playerInventoryAtOpen, 0x0000000F);
        int32_t goldAfter = GetSnapshotCount(currentInventorySnapshot, 0x0000000F);

        std::set<RE::FormID> changedFormIDs;
        for (const auto& [formID, _] : barterSession.playerInventoryAtOpen.items) {
            changedFormIDs.insert(formID);
        }
        for (const auto& [formID, _] : currentInventorySnapshot.items) {
            changedFormIDs.insert(formID);
        }

        for (const auto formID : changedFormIDs) {
            if (formID == 0x0000000F) {
                continue;
            }

            const int32_t beforeCount = GetSnapshotCount(barterSession.playerInventoryAtOpen, formID);
            const int32_t afterCount = GetSnapshotCount(currentInventorySnapshot, formID);
            const int32_t delta = afterCount - beforeCount;
            if (delta == 0) {
                continue;
            }

            std::string itemName = GetSnapshotName(currentInventorySnapshot, formID);
            if (itemName.empty()) {
                itemName = GetSnapshotName(barterSession.playerInventoryAtOpen, formID);
            }

            int32_t unitGoldValue = 0;
            if (auto* itemForm = RE::TESForm::LookupByID(formID)) {
                unitGoldValue = std::max(static_cast<int32_t>(itemForm->GetGoldValue()), 0);
            }

            const int32_t count = std::abs(delta);
            const int32_t totalGoldValue = count * unitGoldValue;

            if (delta > 0) {
                AddTradeItem(merchantSoldItems, itemName, count, totalGoldValue);
            } else {
                AddTradeItem(playerSoldItems, itemName, count, totalGoldValue);
            }
        }

        const bool hasItemChanges = !merchantSoldItems.empty() || !playerSoldItems.empty();
        const int32_t netGoldToMerchant = goldBefore - goldAfter;
        const bool hasGoldChange = netGoldToMerchant != 0;

        if (!hasItemChanges && !hasGoldChange) {
            logger::info("[BARTER_MENU] Closed barter session with no completed transaction");
            ResetBarterSession();
            return;
        }

        const std::string merchantName = barterSession.merchantName.empty() ? "The merchant" : barterSession.merchantName;
        const std::string playerLabel = GetBarterPlayerLabel();

        std::vector<std::string> clauses;
        if (!merchantSoldItems.empty()) {
            clauses.push_back(std::format("{} sold: {}", merchantName, FormatTradeItemList(merchantSoldItems)));
        }
        if (!playerSoldItems.empty()) {
            clauses.push_back(std::format("{} sold: {}", playerLabel, FormatTradeItemList(playerSoldItems)));
        }
        if (netGoldToMerchant > 0) {
            clauses.push_back(std::format("{} gave {} {} septims", playerLabel, merchantName, netGoldToMerchant));
        } else if (netGoldToMerchant < 0) {
            clauses.push_back(std::format("{} gave {} {} septims", merchantName, playerLabel, -netGoldToMerchant));
        }

        std::ostringstream summary;
        summary << merchantName << " traded with " << playerLabel;
        if (!clauses.empty()) {
            summary << ". ";
        }
        for (std::size_t i = 0; i < clauses.size(); ++i) {
            if (i > 0) {
                summary << ". ";
            }
            summary << clauses[i];
        }

        std::string message = summary.str();
        if (!message.empty() && message.back() != '.') {
            message.push_back('.');
        }

        const std::string payload =
            std::format("infoaction|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), message);

        if (auto* merchantActor = ResolveStoredBarterMerchantActor()) {
            HTTPManager::log(payload, merchantActor);
        } else if (!barterSession.merchantName.empty() && barterSession.merchantName != "The merchant") {
            HTTPManager::log(payload, barterSession.merchantName);
        } else {
            HTTPManager::log(payload);
        }

        logger::info("[BARTER_TRACKED] {}", message);
        ResetBarterSession();
    }
    
    // Helper function to capture current player inventory
    void CaptureInventorySnapshot(InventorySnapshot& snapshot) {
        snapshot.items.clear();
        snapshot.active = true;
        
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        
        auto inventory = player->GetInventory();
        
        for (const auto& item : inventory) {
            RE::TESBoundObject* boundObject = item.first;
            auto count = item.second.first;
            const std::unique_ptr<RE::InventoryEntryData>& entryData = item.second.second;
            
            if (!boundObject || count == 0) {
                continue;
            }
            
            // Skip non-inventory items
            if (!boundObject->IsInventoryObject() || boundObject->IsIgnored()) {
                continue;
            }
            
            // Get item name
            std::string itemName;
            if (auto baseName = boundObject->GetName(); baseName && baseName[0]) {
                itemName = baseName;
            }
            
            // Check for custom/enchanted names
            if (entryData) {
                if (auto display = entryData->GetDisplayName(); display && display[0]) {
                    itemName = display;
                }
                
                if (auto* xlist = entryData->extraLists; xlist) {
                    for (auto extraList : *xlist) {
                        if (!extraList) continue;
                        
                        if (auto textData = extraList->GetByType<RE::ExtraTextDisplayData>(); textData) {
                            if (!textData->displayName.empty()) {
                                itemName.assign(textData->displayName);
                                break;
                            }
                        }
                    }
                }
            }
            
            // Skip items with missing or invalid names
            if (!itemName.empty() && itemName != "<Missing Name>") {
                RE::FormID formID = boundObject->GetFormID();
                snapshot.items[formID] = {itemName, count};
            }
        }
        
        logger::info("[CRAFTING_SNAPSHOT] Captured {} items in inventory snapshot", snapshot.items.size());
    }
    
    // Helper function to compare inventory and log crafted items
    void LogCraftedItems(const InventorySnapshot& snapshot) {
        if (!snapshot.active) return;
        
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        
        auto inventory = player->GetInventory();
        std::vector<std::string> craftedItems;
        
        // Check for new or increased items
        for (const auto& item : inventory) {
            RE::TESBoundObject* boundObject = item.first;
            auto currentCount = item.second.first;
            const std::unique_ptr<RE::InventoryEntryData>& entryData = item.second.second;
            
            if (!boundObject || currentCount == 0) {
                continue;
            }
            
            if (!boundObject->IsInventoryObject() || boundObject->IsIgnored()) {
                continue;
            }
            
            RE::FormID formID = boundObject->GetFormID();
            
            // Get current item name
            std::string itemName;
            if (auto baseName = boundObject->GetName(); baseName && baseName[0]) {
                itemName = baseName;
            }
            
            if (entryData) {
                if (auto display = entryData->GetDisplayName(); display && display[0]) {
                    itemName = display;
                }
                
                if (auto* xlist = entryData->extraLists; xlist) {
                    for (auto extraList : *xlist) {
                        if (!extraList) continue;
                        
                        if (auto textData = extraList->GetByType<RE::ExtraTextDisplayData>(); textData) {
                            if (!textData->displayName.empty()) {
                                itemName.assign(textData->displayName);
                                break;
                            }
                        }
                    }
                }
            }
            
            if (itemName.empty()) continue;
            
            // Check if this is a new item or quantity increased
            auto snapshotIter = snapshot.items.find(formID);
            if (snapshotIter == snapshot.items.end()) {
                // New item
                craftedItems.push_back(std::format("{}x {}", currentCount, itemName));
            } else {
                // Check if quantity increased
                int32_t oldCount = snapshotIter->second.second;
                if (currentCount > oldCount) {
                    int32_t craftedCount = currentCount - oldCount;
                    craftedItems.push_back(std::format("{}x {}", craftedCount, itemName));
                }
            }
        }
        
        // Log the crafted items if any
        if (!craftedItems.empty()) {
            std::string craftedList;
            for (size_t i = 0; i < craftedItems.size(); ++i) {
                if (i > 0) craftedList += ", ";
                craftedList += craftedItems[i];
            }
            
            std::string playerName = player->GetDisplayFullName();
            HTTPManager::log(std::format("infoaction|{}|{}|{} crafted {}", 
                                        getCurrentTimeMillis(), 
                                        GetGameTimeStamp(),
                                        playerName,
                                        craftedList));
            
            logger::info("[CRAFTING_TRACKED] {} crafted: {}", playerName, craftedList);
        } else {
            logger::info("[CRAFTING_TRACKED] No items crafted (menu opened but nothing created)");
        }
    }

    class ProcessorMenuEvent : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        ProcessorMenuEvent(const ProcessorMenuEvent&) = delete;
        ProcessorMenuEvent(ProcessorMenuEvent&&) = delete;
        ProcessorMenuEvent& operator=(const ProcessorMenuEvent&) = delete;
        ProcessorMenuEvent& operator=(ProcessorMenuEvent&&) = delete;
        ProcessorMenuEvent() = default;
        ~ProcessorMenuEvent() = default;

    public:
        static ProcessorMenuEvent& GetInstance() {
            static ProcessorMenuEvent instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>* source) override {
           
            if (event->menuName == RE::JournalMenu::MENU_NAME) {
                if (event->opening) {
                    ;  // HTTPManager::log(std::format("_questreset|{}|{}|", getCurrentTimeMillis(),
                       // GetGameTimeStamp()));
                    
                }

                else {
                    HTTPManager::log(std::format("_questreset|{}|{}|", getCurrentTimeMillis(), GetGameTimeStamp()));
                    ProcedureSendActiveQuests();
                }
            } else if (event->menuName == RE::BookMenu::MENU_NAME) {
                if (event->opening) {
                    ;
                } else {
                    DynamicDiaryBook::OnBookMenuClosed();
                }
            } else if (event->menuName == RE::BarterMenu::MENU_NAME) {
                if (event->opening) {
                    logger::info("[BARTER_MENU] Barter menu opened");
                    StartBarterSession();
                } else {
                    logger::info("[BARTER_MENU] Barter menu closed");
                    FinalizeBarterSession();
                }
            } else if (event->menuName == "Crafting Menu") {
                // Track all crafting activities: smithing, alchemy, enchanting, cooking, tanning, smelting, etc.
                // Skyrim uses "Crafting Menu" as the universal menu name for ALL crafting stations:
                // - Blacksmith Forge, Grindstone, Workbench, Armorer Workbench
                // - Alchemy Lab (potion/poison creation)
                // - Arcane Enchanter (enchanting weapons/armor)
                // - Cooking Pot, Oven
                // - Tanning Rack, Smelter
                if (event->opening) {
                    logger::info("[CRAFTING_MENU] Crafting Menu opened - capturing inventory snapshot");
                    CaptureInventorySnapshot(craftingInventorySnapshot);
                } else {
                    logger::info("[CRAFTING_MENU] Crafting Menu closed - checking for crafted items");
                    LogCraftedItems(craftingInventorySnapshot);
                    craftingInventorySnapshot.active = false;
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };
}

void SetPendingBarterMerchant(RE::Actor* merchant) {
    if (!merchant) {
        ProcessorMenu::ClearPendingBarterMerchant();
        return;
    }

    const auto merchantFormID = merchant->GetFormID();
    const std::string merchantName = trim(merchant->GetDisplayFullName());
    if (ProcessorMenu::pendingBarterMerchant.active &&
        ProcessorMenu::pendingBarterMerchant.merchantFormID == merchantFormID &&
        ProcessorMenu::pendingBarterMerchant.merchantName == merchantName) {
        ProcessorMenu::pendingBarterMerchant.capturedAt = std::chrono::steady_clock::now();
        return;
    }

    ProcessorMenu::pendingBarterMerchant.active = true;
    ProcessorMenu::pendingBarterMerchant.merchantFormID = merchantFormID;
    ProcessorMenu::pendingBarterMerchant.merchantName = merchantName;
    ProcessorMenu::pendingBarterMerchant.capturedAt = std::chrono::steady_clock::now();
}

RE::Actor* ResolvePendingBarterMerchant(RE::Actor* fallbackMerchant)
{
    auto* playerActor = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();

    if (auto* topicManager = RE::MenuTopicManager::GetSingleton()) {
        if (auto speakerHandle = topicManager->speaker.get()) {
            if (auto* speakerActor = speakerHandle->As<RE::Actor>()) {
                if (!playerActor || speakerActor->GetFormID() != playerActor->GetFormID()) {
                    return speakerActor;
                }
            }
        }
    }

    const auto [conversationMerchantFormID, conversationMerchantName] =
        ProcessorMenu::ResolveConversationMerchantCandidate();
    if (conversationMerchantFormID != 0 && !conversationMerchantName.empty()) {
        if (auto* merchantForm = RE::TESForm::LookupByID(conversationMerchantFormID)) {
            if (auto* merchantActor = merchantForm->As<RE::Actor>()) {
                return merchantActor;
            }
        }
    }

    return fallbackMerchant;
}

namespace ProcessorPlayerMenuModEvent {
    class PlayerMenuModCallbackEventSink : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
        PlayerMenuModCallbackEventSink(const PlayerMenuModCallbackEventSink&) = delete;
        PlayerMenuModCallbackEventSink(PlayerMenuModCallbackEventSink&&) = delete;
        PlayerMenuModCallbackEventSink& operator=(const PlayerMenuModCallbackEventSink&) = delete;
        PlayerMenuModCallbackEventSink& operator=(PlayerMenuModCallbackEventSink&&) = delete;
        PlayerMenuModCallbackEventSink() = default;
        ~PlayerMenuModCallbackEventSink() = default;

    public:
        static PlayerMenuModCallbackEventSink& GetInstance() {
            static PlayerMenuModCallbackEventSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* event,
                                              RE::BSTEventSource<SKSE::ModCallbackEvent>*) override {
            if (!event || event->eventName != "PlayMenuTopic") {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (!PlayerTtsTraditionalDialogueEnabled) {
                Papyrus::releasePlayerMenuTopicTimer("Player TTS for Traditional Dialogue is disabled");
                return RE::BSEventNotifyControl::kContinue;
            }

            const int started = Papyrus::startPlayerMenuDialogueTTSNative(event->strArg.c_str());
            if (started <= 0) {
                Papyrus::releasePlayerMenuTopicTimer("Player TTS for Traditional Dialogue could not start");
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };
}

namespace ProcessorScreenShot {
    struct DXGIPresentHook {
        static void thunk(std::uint32_t a_p1);
        static inline REL::Relocation<decltype(thunk)> func;

        // static constexpr auto id = REL::RelocationID(75461, 77246);
        static constexpr auto id = REL::VariantID(75461, 77246, 0xDBBDD0);
        static constexpr auto offset = REL::VariantOffset(0x9, 0x9, 0x15);

        static inline std::mutex callback_mutex;
    };

    void DXGIPresentHook::thunk(std::uint32_t a_p1) {
        func(a_p1);

        // Pace the per-frame VR camera snapshot for the spatial audio engine (VR-only; no-op in SE/AE).
        SpatialAwareness::UpdatePlayerCameraSnapshot();

        if (MutexIsMakeShotActivated()) {
            MutexSetMakeShotActive(false);
            ProcedureTakeShot();
        }
    }

    template <class T>
    void write_thunk_call() {
        auto& trampoline = SKSE::GetTrampoline();

        static const REL::Relocation<uintptr_t> presentHook{
            REL::VariantID(75461, 77246, 0xDBBDD0)};  // D6A2B0, DA5BE0, DBBDD0
        // trampoline.write_call<5>(presentHook.address() + REL::VariantOffset(0x9, 0x9, 0x15).offset(), Present);
        // SKSE::AllocTrampoline(14);

        T::func =
            trampoline.write_call<5>(presentHook.address() + REL::VariantOffset(0x9, 0x9, 0x15).offset(), T::thunk);
    }

    static void InstallHooks() { 
        write_thunk_call<ProcessorScreenShot::DXGIPresentHook>(); 
    }


}

namespace ProcessorNativeScreenShot {

    struct Hook {
        static void thunk(ID3D11Texture2D* a_texture_2d, char const* a_path,
                          RE::BSGraphics::TextureFileFormat a_format);

        static inline REL::Relocation<decltype(thunk)> func;
    };

    void Hook::thunk(ID3D11Texture2D* a_texture_2d, char const* a_path, RE::BSGraphics::TextureFileFormat a_format) {
        
        func(a_texture_2d, a_path, a_format);
        if (MutexIsMakeShotNativeActivated()) {
            MutexSetMakeShotActive(true);
            logger::info("Making screenshot");
            ProcedureSendShot(a_path);
        }

    }

    void write_thunk_call() {
        auto& trampoline = SKSE::GetTrampoline();

        REL::Relocation<std::uintptr_t> target{RELOCATION_ID(75522, 77316), 0x60};  // Main::Swap

        Hook::func = trampoline.write_call<5>(target.address(), Hook::thunk);
    }

    static void InstallHooks() { write_thunk_call(); }

}

namespace ProcessorActorDialogue {
    bool ShouldHoldDialogueCommit(RE::Actor* actor, RE::DialogueResponse* response)
    {
        if (!kPlayerMenuUseActorCommitGate) {
            return false;
        }

        if (!actor || !response || g_playerMenuForceDeferredCommit.load()) {
            return false;
        }

        bool gateActive = false;
        RE::FormID gateSpeakerFormID = 0;
        {
            std::lock_guard<std::mutex> lock(g_playerMenuDialogueGateMutex);
            gateActive = g_playerMenuDialogueGateActive;
            gateSpeakerFormID = g_playerMenuDialogueGateSpeakerFormID;
        }

        if (gateActive && gateSpeakerFormID != 0 && actor->GetFormID() == gateSpeakerFormID) {
            ArmPlayerMenuDeferredDialogueCommit(actor, response);
            return true;
        }

        return false;
    }

    struct ActorUpdateInDialogueHook {
        static bool thunk(RE::Actor* actor, RE::DialogueResponse* response, bool a_unused);
        static inline REL::Relocation<decltype(thunk)> func;
    };

    struct CharacterUpdateInDialogueHook {
        static bool thunk(RE::Character* actor, RE::DialogueResponse* response, bool a_unused);
        static inline REL::Relocation<decltype(thunk)> func;
    };

    void InstallHooks()
    {
        REL::Relocation<std::uintptr_t> actorVtbl{RE::VTABLE_Actor[0]};
        ActorUpdateInDialogueHook::func = actorVtbl.write_vfunc(0x4C, ActorUpdateInDialogueHook::thunk);

        REL::Relocation<std::uintptr_t> characterVtbl{RE::VTABLE_Character[0]};
        CharacterUpdateInDialogueHook::func = characterVtbl.write_vfunc(0x4C, CharacterUpdateInDialogueHook::thunk);

    }

    bool ActorUpdateInDialogueHook::thunk(RE::Actor* actor, RE::DialogueResponse* response, bool a_unused)
    {
        if (ShouldHoldDialogueCommit(actor, response)) {
            return false;
        }

        return func(actor, response, a_unused);
    }

    bool CharacterUpdateInDialogueHook::thunk(RE::Character* actor, RE::DialogueResponse* response, bool a_unused)
    {
        if (ShouldHoldDialogueCommit(actor, response)) {
            return false;
        }

        return func(actor, response, a_unused);
    }
}

namespace ProcessorVrVisemePump {
    struct PlayerUpdateHook {
        static void thunk(RE::PlayerCharacter* player, float deltaSeconds);
        static inline REL::Relocation<decltype(thunk)> func;
    };

    void InstallHooks()
    {
        REL::Relocation<std::uintptr_t> playerVtbl{RE::VTABLE_PlayerCharacter[0]};
        PlayerUpdateHook::func = playerVtbl.write_vfunc(0xAD, PlayerUpdateHook::thunk);
        logger::info("[SpeakManager] Installed VR player-update viseme pump hook");
    }

    void PlayerUpdateHook::thunk(RE::PlayerCharacter* player, float deltaSeconds)
    {
        func(player, deltaSeconds);
        ProcessVrVisemePumpOnGameThread(deltaSeconds);
    }
}

namespace ProcessorDialogueMenu {
    struct DialogueMenuProcessMessageHook {
        static RE::UI_MESSAGE_RESULTS thunk(RE::DialogueMenu* menu, RE::UIMessage& message);
        static inline REL::Relocation<decltype(thunk)> func;
    };

    struct DialogueMenuAdvanceMovieHook {
        static void thunk(RE::DialogueMenu* menu, float interval, std::uint32_t currentTime);
        static inline REL::Relocation<decltype(thunk)> func;
    };

    void InstallHooks()
    {
        REL::Relocation<std::uintptr_t> vtbl{RE::VTABLE_DialogueMenu[0]};
        DialogueMenuProcessMessageHook::func = vtbl.write_vfunc(0x4, DialogueMenuProcessMessageHook::thunk);
        DialogueMenuAdvanceMovieHook::func = vtbl.write_vfunc(0x5, DialogueMenuAdvanceMovieHook::thunk);
    }

    void DialogueMenuAdvanceMovieHook::thunk(RE::DialogueMenu* menu, float interval, std::uint32_t currentTime)
    {
        func(menu, interval, currentTime);
        PumpPlayerMenuDelayedSelectionCapture();
        UpdatePlayerMenuDialogueGate();
    }

    RE::UI_MESSAGE_RESULTS DialogueMenuProcessMessageHook::thunk(RE::DialogueMenu* menu, RE::UIMessage& message)
    {
        if (!menu) {
            return func(menu, message);
        }

        if (message.type == RE::UI_MESSAGE_TYPE::kScaleformEvent) {
            if (IsLikelyPlayerMenuSelectionScaleformEvent(message)) {
                MarkPlayerMenuSelectionUserEventSeen();
                if (HasPlayerMenuDelayedSelectionPending() || IsPlayerMenuReplayUserEventArmed() ||
                    IsPlayerMenuReplayInFlight()) {
                    return RE::UI_MESSAGE_RESULTS::kHandled;
                }
                if (TryInterceptPlayerMenuSelection("TopicClicked", "scaleform")) {
                    return RE::UI_MESSAGE_RESULTS::kHandled;
                }
            }
            return func(menu, message);
        }

        if (message.type != RE::UI_MESSAGE_TYPE::kUserEvent) {
            return func(menu, message);
        }

        const std::string userEventName = GetPlayerMenuUserEventName(message);
        if (userEventName.empty()) {
            return func(menu, message);
        }

        if (ConsumePlayerMenuReplayUserEvent(message, userEventName)) {
            return func(menu, message);
        }

        if (IsPlayerMenuCancelUserEvent(userEventName)) {
            if (HasPlayerMenuDelayedSelectionPending() || IsPlayerMenuReplayUserEventArmed() ||
                IsPlayerMenuDialogueGateActive()) {
                ClearPlayerMenuDelayedSelectionState();
                ClearPlayerMenuDeferredDialogueCommit();
                EndPlayerMenuDialogueGate(false);
                g_playerMenuReleaseTaskQueued.store(false);
                ClearPlayerMenuPendingPlayerSpeech();
            }
            return func(menu, message);
        }

        if (HasPlayerMenuDelayedSelectionPending() || IsPlayerMenuReplayUserEventArmed() ||
            IsPlayerMenuReplayInFlight() ||
            IsPlayerMenuDialogueGateActive()) {
            return RE::UI_MESSAGE_RESULTS::kHandled;
        }

        if (!IsPlayerMenuSelectionUserEvent(userEventName)) {
            return func(menu, message);
        }

        MarkPlayerMenuSelectionUserEventSeen();

        if (TryInterceptPlayerMenuSelection(userEventName, "user_event")) {
            if (kPlayerMenuUseActorCommitGate) {
                return func(menu, message);
            }
            return RE::UI_MESSAGE_RESULTS::kHandled;
        }
        return func(menu, message);
    }
}

namespace ProcessorSerialization {

    
    inline const auto AgentCountRecord = _byteswap_ulong('AIAC');
    inline const auto NamesCountRecord = _byteswap_ulong('AIAX');


    void OnGameLoaded(SKSE::SerializationInterface* serde) {
        
        std::uint32_t type;
        std::uint32_t size;
        std::uint32_t version;
        
        AIAgentManager& aiam = AIAgentManager::getInstance();
        // Ensure that everything is initialized in HTTPManager
        std::this_thread::sleep_for(std::chrono::seconds(1));

        while (serde->GetNextRecordInfo(type, version, size)) {
            if (type == AgentCountRecord) {
                // First read how many items follow in this record, so we know how many times to iterate.
                std::size_t agentCountsSize;
                serde->ReadRecordData(&agentCountsSize, sizeof(agentCountsSize));
                // Iterate over the remaining data in the record.
                for (; agentCountsSize > 0; --agentCountsSize) {
                    RE::FormID actorFormID;
                    serde->ReadRecordData(&actorFormID, sizeof(actorFormID));
                    RE::FormID newActorFormID;
                    if (!serde->ResolveFormID(actorFormID, newActorFormID)) {
                        logger::warn("Actor ID {:X} could not be found after loading the save.", actorFormID);
                        continue;
                    }
                    
                    auto* actor = RE::TESForm::LookupByID<RE::Actor>(newActorFormID);
                    if (actor) {
                        if (actor->IsDisabled() || actor->IsDeleted() || actor->IsDead()) {
                            logger::info("Actor {} is NOT driven by AI because flags", actor->GetDisplayFullName());
                            continue;
                        }

                        if (actor->IsPlayer()) {
                            logger::info("Actor {} is NOT driven by AI because is player", actor->GetDisplayFullName());
                            continue;
                        }

                        if (newActorFormID == 0x14) {
                            logger::info("Actor {} is NOT driven by AI because is player ref", actor->GetDisplayFullName());
                            continue;
                        }
                        logger::info("Actor {} is driven by AI now, formId {:08X}", actor->GetDisplayFullName(),
                                     newActorFormID);

                        auto already = aiam.getAgentByName(actor->GetDisplayFullName());
                        if (already) {
                            logger::debug("Actor {} already present in AI Agent Manager, skipping",
                                          actor->GetDisplayFullName());
                            continue;
                        } else {
                            auto agent = aiam.createAgent();
                            agent->setActor(actor);
                            agent->setAvailable(true);
                            if (actor->GetActorBase()->voiceType) {
                                agent->setOriginalVoice(actor->GetActorBase()->voiceType);
                                logger::debug("Actor {} , storing original voice {:08X}", agent->getActorName(),
                                              actor->GetActorBase()->voiceType->GetFormID());
                            }

                            agent->setManuallyAdded(true);
                            aiam.addAgent(agent);

                            std::string category;
                            auto baseActor = agent->getActor()->GetActorBase();
                            if (baseActor) {
                                category.assign(baseActor->GetName());
                            }

                            // targetActor->AllowPCDialogue(false);
                            HTTPManager::log(std::format("addnpc|{}|{}|{}@{}", getCurrentTimeMillis(),
                                                         GetGameTimeStamp(), actor->GetDisplayFullName(), category));
                            
                            // Immediately send stats for the new NPC so server has complete info
                            logger::info("[NPC_ADD] Sending initial stats for new NPC: {}", actor->GetDisplayFullName());
                            RefreshAIAgentStats(actor, actor->GetDisplayFullName(), true);
                        }
                    }   

                }

               

            } else if (type == NamesCountRecord) {
                // Read renamedNpcs vector and apply SetDisplayName
                std::size_t renamedNpcsSize = 0;
                serde->ReadRecordData(&renamedNpcsSize, sizeof(renamedNpcsSize));
                
                for (std::size_t i = 0; i < renamedNpcsSize; ++i) {
                    RE::FormID formId;
                    serde->ReadRecordData(&formId, sizeof(formId));
                    
                    std::size_t nameLen = 0;
                    serde->ReadRecordData(&nameLen, sizeof(nameLen));
                    std::string newName;
                    if (nameLen > 0) {
                        newName.resize(nameLen);
                        serde->ReadRecordData(newName.data(), static_cast<uint32_t>(nameLen));
                    }

                    std::size_t modNameLen = 0;
                    serde->ReadRecordData(&modNameLen, sizeof(modNameLen));
                    std::string modName;
                    if (modNameLen > 0) {
                        modName.resize(modNameLen);
                        serde->ReadRecordData(modName.data(), static_cast<uint32_t>(modNameLen));
                    }
                    if (modName.empty()) {
                        

                        RE::FormID newActorFormID;
                        if (!serde->ResolveFormID(formId, newActorFormID)) {
                            logger::warn("Actor ID {:X} could not be found after loading the save.", formId);
                            continue;
                        }


                        
                        logger::info("[SERIALIZE BGL NO MOD] Tracked NPC {:08X} = '{}', OLD {:08X}", newActorFormID,
                                     newName, formId);

                        // Apply SetDisplayName to the actor
                        RE::Actor* actor = RE::TESForm::LookupByID<RE::Actor>(newActorFormID);
                        if (actor) {
                            aiam.addRenamedNpc(newActorFormID, newName);
                        } else {
                            logger::info("Actor ID {:08X} not found in the current load order. Skipping", newActorFormID);
                        }
                        if (actor && !newName.empty()) {
                            if (actor->IsDead() || actor->IsDeleted() || actor->IsDisabled()) {
                                
                                aiam.removeRenamedNpcByFormId(newActorFormID);
                                logger::info("\tSkipped renaming NPC {:08X} to '{}' because dead/disabled/deleted", newActorFormID, newName);
                                continue;
                            }
                            if (AIAgentRoleMasterFaction) actor->AddToFaction(AIAgentRoleMasterFaction, 1);
                            actor->SetDisplayName(newName.c_str(), true);
                            actor->AsReference()->SetDisplayName(newName.c_str(), true);
                            logger::info("\tRenamed NPC {:08X} to '{}'", formId, newName);
                        }
                    } else {
                        
                        // formId is in the form 0x00000000 , where first to digits are the mod index. 
                        // We should find new mod index (name stored in modName) and reconstruct formId
                        // before resolving. Discard the two first digits and replace with new mod index.
                        RE::TESDataHandler* dataHandler = RE::TESDataHandler::GetSingleton();
                        std::optional<std::uint8_t> newIndex = dataHandler->GetModIndex(modName);

                        RE::FormID recalculatedFormId = formId & 0x00FFFFFF;  // Discard first two digits
                        recalculatedFormId |= (static_cast<RE::FormID>(newIndex.value()) << 24);  // Add new mod index
                        logger::warn("Actor ID {:08X} has changed FormID to {:08X}", formId, recalculatedFormId);

                        RE::FormID newActorFormID;
                        if (!serde->ResolveFormID(formId, newActorFormID)) {
                            logger::warn("Actor ID{:08X} could not be found after loading the save.", formId);
                            continue;
                        } else {
                            logger::info(
                                "Actor ID {:08X} has changed FormID to {:08X}, SerializationInterface says (will use this) {:08X}",
                                formId, recalculatedFormId, newActorFormID);
                            recalculatedFormId = newActorFormID;
                        }
                        

                        // Restore array, even if actor not present
                        aiam.addRenamedNpc(recalculatedFormId, newName);
                        logger::info("Tracked NPC {:08X} = '{}'", recalculatedFormId, newName);

                        // Apply SetDisplayName to the actor
                        RE::Actor* actor = RE::TESForm::LookupByID<RE::Actor>(recalculatedFormId);
                        if (actor && !newName.empty()) {
                            actor->SetDisplayName(newName.c_str(), true);
                            actor->AsReference()->SetDisplayName(newName.c_str(), true);
                            logger::info("\tRenamed NPC {:08X} to '{}'", recalculatedFormId, newName);
                        }
                    }
                }
                
            } else  {
                logger::warn("Unknown record type in cosave.");
            }
        }

    }

    void OnGameSaved(SKSE::SerializationInterface* serde) {
        if (!serde->OpenRecord(AgentCountRecord, 0)) {
            logger::error("Unable to open record AgentCountRecord to write cosave data.");
            return;
        }
        AIAgentManager& aiam = AIAgentManager::getInstance();
        auto agentCountsSize = aiam.getAgents().size();
        serde->WriteRecordData(&agentCountsSize, sizeof(agentCountsSize));
        for (const auto& agent : aiam.getAgents()) {
            auto* actor = agent->getActor();
            if (agent->isManuallyAdded()) serde->WriteRecordData(&actor->formID, sizeof(actor->formID));
        }

        if (!serde->OpenRecord(NamesCountRecord, 0)) {
            logger::error("Unable to open record NamesCountRecord to write cosave data.");
            return;
        }

        // Serialize renamedNpcs vector
        auto renamedNpcs=aiam.getRenamedNpcs();
        std::size_t renamedNpcsSize = renamedNpcs.size();
        serde->WriteRecordData(&renamedNpcsSize, sizeof(renamedNpcsSize));
        logger::error("[COSAVE] Size of  CHIM-marked NPCs {}.", renamedNpcsSize);
        for (const auto& entry : renamedNpcs) {
            logger::error("[COSAVE] Storing as CHIM-marked NPC {}.",entry.second.data());

            serde->WriteRecordData(&entry.first, sizeof(entry.first));
            std::size_t nameLen = entry.second.size();
            serde->WriteRecordData(&nameLen, sizeof(nameLen));
            if (nameLen > 0) {
                serde->WriteRecordData(entry.second.data(), static_cast<uint32_t>(nameLen));
            }
            // Find mod name
            std::string modName;
            RE::TESForm *fid = RE::TESForm::LookupByID(entry.first);

            if (fid) {
                RE::TESFile* modFile = fid->GetFile();
                if (modFile) 
                    modName.assign(modFile->GetFilename());
            }
            std::size_t nameModLen = modName.size();
            serde->WriteRecordData(&nameModLen, sizeof(nameModLen));
            if (nameLen > 0) {
                serde->WriteRecordData(modName.data(), static_cast<uint32_t>(nameModLen));
                logger::error("[COSAVE] Storing as CHIM-marked NPC {}. {}", entry.second.data(), modName);
            }
        }
    }

    void OnRevert(SKSE::SerializationInterface*) {

        AIAgentManager& aiam = AIAgentManager::getInstance();
        aiam.removeAllAgents();

        
    }

    void InitializeSerialization() {
        logger::info("Initializing cosave serialization...");
        auto* serde = SKSE::GetSerializationInterface();
        serde->SetUniqueID(_byteswap_ulong('AIAG'));
        serde->SetSaveCallback(OnGameSaved);
        serde->SetRevertCallback(OnRevert);
        serde->SetLoadCallback(OnGameLoaded);
        logger::info("Cosave serialization initialized ;-)");
    }
}

// Fix for TESContainerChangedEventEx
namespace RE {
    struct TESContainerChangedEventEx {
    public:
        // members
        FormID oldContainer;     // 00
        FormID newContainer;     // 04
        FormID baseObj;          // 08
        std::int32_t itemCount;  // 0C
        FormID reference;        // 10
        std::uint16_t uniqueID;  // 14
        std::uint16_t pad16;     // 16
    };
    static_assert(sizeof(TESContainerChangedEventEx) == 0x18);

    // Helper function to cast
    const TESContainerChangedEventEx* castWrongStruct(const RE::TESContainerChangedEvent* event) {
        return reinterpret_cast<const TESContainerChangedEventEx*>(event);
    }

    TESContainerChangedEventEx* castWrongStruct(RE::TESContainerChangedEvent* event) {
        return reinterpret_cast<TESContainerChangedEventEx*>(event);
    }
}

namespace
{
    constexpr auto kPlayerItemPickupBatchDelay = std::chrono::milliseconds(500);
    std::mutex g_playerItemPickupBatchMutex;
    std::vector<json> g_playerItemPickupBatch;
    std::atomic_bool g_playerItemPickupFlushScheduled{false};

    std::string QuestProgressionToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::string GetQuestProgressionPluginName(const RE::TESForm* form)
    {
        if (!form) {
            return {};
        }

        RE::TESFile* sourceFile = form->GetFile(0);
        if (!sourceFile || !sourceFile->fileName || !sourceFile->fileName[0]) {
            return {};
        }

        return sourceFile->fileName;
    }

    std::string FormatQuestProgressionFormID(RE::FormID formID)
    {
        return std::format("0x{:08X}", static_cast<std::uint32_t>(formID));
    }

    bool TryParseQuestProgressionFormID(const std::string& value, RE::FormID& outFormID)
    {
        std::string trimmed = trim(value);
        if (trimmed.empty()) {
            return false;
        }

        int base = 10;
        if (trimmed.rfind("0x", 0) == 0 || trimmed.rfind("0X", 0) == 0) {
            trimmed = trimmed.substr(2);
            base = 16;
        }

        try {
            const unsigned long parsed = std::stoul(trimmed, nullptr, base);
            if (parsed > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            outFormID = static_cast<RE::FormID>(parsed);
            return true;
        } catch (...) {
            return false;
        }
    }

    std::string BuildQuestProgressionFormKey(const std::string& pluginName, const std::string& formID)
    {
        if (pluginName.empty() || formID.empty()) {
            return {};
        }

        return QuestProgressionToLower(pluginName) + "|" + QuestProgressionToLower(formID);
    }

    std::string BuildQuestProgressionFormKey(const RE::TESForm* form)
    {
        if (!form) {
            return {};
        }

        const std::string pluginName = GetQuestProgressionPluginName(form);
        if (pluginName.empty()) {
            return {};
        }

        return BuildQuestProgressionFormKey(
            pluginName,
            FormatQuestProgressionFormID(form->GetLocalFormID()));
    }

    bool PopulateQuestProgressionFormPayload(json& payload, const char* formField, const char* pluginField,
                                             const RE::TESForm* form)
    {
        if (!form || !formField || !pluginField) {
            return false;
        }

        const std::string pluginName = GetQuestProgressionPluginName(form);
        if (pluginName.empty()) {
            return false;
        }

        payload[formField] = FormatQuestProgressionFormID(form->GetLocalFormID());
        payload[pluginField] = pluginName;
        return true;
    }

    RE::FormID ResolveQuestProgressionRuntimeFormID(const std::string& pluginName, const std::string& formID)
    {
        RE::FormID localFormID = 0;
        if (!TryParseQuestProgressionFormID(formID, localFormID)) {
            return 0;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            return 0;
        }

        return dataHandler->LookupFormID(localFormID, pluginName.c_str());
    }

    bool PostQuestProgressionEvent(const std::string& eventType, json payload)
    {
        if (!pluginInited || !AIQuestProgressionEnabled || eventType.empty()) {
            return false;
        }

        payload["event_source"] = "skse_plugin";
        if (!payload.contains("gamets")) {
            payload["gamets"] = GetGameTimeStamp();
        }

        HTTPManager::postGameData("gamedata.php", json{
            {"type", "quest_event"},
            {"event_type", eventType},
            {"payload", payload}
        });
        return true;
    }

    std::unordered_map<std::uint32_t, std::string> BuildQuestProgressionAliasInstanceText(RE::TESQuest* quest)
    {
        std::unordered_map<std::uint32_t, std::string> aliasText;
        if (!quest) {
            return aliasText;
        }

        for (auto iter = quest->instanceData.begin(); iter != quest->instanceData.end(); ++iter) {
            RE::BGSQuestInstanceText* instanceText = *iter;
            if (!instanceText) {
                continue;
            }

            for (auto textIter = instanceText->stringData.begin(); textIter != instanceText->stringData.end(); ++textIter) {
                RE::TESForm* resolved = RE::TESForm::LookupByID(textIter->fullNameFormID);
                if (!resolved || !resolved->GetName() || !resolved->GetName()[0]) {
                    continue;
                }

                aliasText[textIter->aliasID] = std::string(resolved->GetName());
            }
        }

        return aliasText;
    }

    json BuildQuestProgressionAliasPayload(RE::TESQuest* quest)
    {
        json aliases = json::array();
        if (!quest) {
            return aliases;
        }

        const auto instanceText = BuildQuestProgressionAliasInstanceText(quest);

        for (auto iter = quest->aliases.begin(); iter != quest->aliases.end(); ++iter) {
            RE::BGSBaseAlias* alias = *iter;
            if (!alias) {
                continue;
            }

            json aliasPayload;
            aliasPayload["id"] = alias->aliasID;
            aliasPayload["name"] = std::string(alias->aliasName);
            aliasPayload["type"] = "unknown";

            auto textIt = instanceText.find(alias->aliasID);
            if (textIt != instanceText.end() && !textIt->second.empty()) {
                aliasPayload["display_name"] = textIt->second;
                aliasPayload["instance_text"] = textIt->second;
            }

            if (auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(alias)) {
                aliasPayload["type"] = "reference";

                RE::TESObjectREFR* ref = refAlias->GetReference();
                if (ref) {
                    PopulateQuestProgressionFormPayload(aliasPayload, "form_id", "plugin", ref);
                    aliasPayload["is_actor"] = refAlias->GetActorReference() != nullptr;

                    if (auto* displayName = ref->GetDisplayFullName(); displayName && displayName[0]) {
                        aliasPayload["display_name"] = std::string(displayName);
                    } else if (auto* refName = ref->GetName(); refName && refName[0]) {
                        aliasPayload["display_name"] = std::string(refName);
                    }

                    if (auto* baseObject = ref->GetBaseObject()) {
                        PopulateQuestProgressionFormPayload(aliasPayload, "base_form_id", "base_plugin", baseObject);
                        if (auto* baseName = baseObject->GetName(); baseName && baseName[0]) {
                            aliasPayload["base_name"] = std::string(baseName);
                            if (!aliasPayload.contains("display_name")) {
                                aliasPayload["display_name"] = std::string(baseName);
                            }
                        }
                    }
                }
            } else if (skyrim_cast<RE::BGSLocAlias*>(alias)) {
                aliasPayload["type"] = "location";
            }

            aliases.push_back(aliasPayload);
        }

        return aliases;
    }

    void AddSuppressedInventoryDelta(std::unordered_map<std::string, int>& suppressionMap, const std::string& formKey,
                                     int count)
    {
        if (formKey.empty() || count <= 0) {
            return;
        }

        suppressionMap[formKey] += count;
    }

    int ConsumeSuppressedInventoryDelta(std::unordered_map<std::string, int>& suppressionMap, const std::string& formKey,
                                        int count)
    {
        if (formKey.empty() || count <= 0) {
            return 0;
        }

        auto it = suppressionMap.find(formKey);
        if (it == suppressionMap.end()) {
            return 0;
        }

        const int consumed = std::min(it->second, count);
        it->second -= consumed;
        if (it->second <= 0) {
            suppressionMap.erase(it);
        }

        return consumed;
    }

    void PostQuestProgressionQuestStage(RE::TESQuest* quest, int stage)
    {
        if (!quest || stage < 0) {
            return;
        }

        json payload;
        if (!PopulateQuestProgressionFormPayload(payload, "quest_form_id", "quest_plugin", quest)) {
            return;
        }

        std::string questKey = BuildQuestProgressionFormKey(
            payload.value("quest_plugin", std::string()),
            payload.value("quest_form_id", std::string()));

        {
            std::lock_guard<std::mutex> lock(g_questProgressionMutex);
            auto it = g_questProgressionLastStages.find(questKey);
            if (it != g_questProgressionLastStages.end() && it->second == stage) {
                return;
            }
            g_questProgressionLastStages[questKey] = stage;
        }

        if (auto* editorId = quest->GetFormEditorID(); editorId && editorId[0]) {
            payload["quest_editor_id"] = std::string(editorId);
        }
        if (auto* questName = quest->GetName(); questName && questName[0]) {
            payload["quest_name"] = std::string(questName);
        }
        json aliases = BuildQuestProgressionAliasPayload(quest);
        if (!aliases.empty()) {
            payload["aliases"] = aliases;
        }
        payload["stage"] = stage;
        PostQuestProgressionEvent("quest_stage", payload);
    }

    void PostQuestProgressionLocationEntered(RE::TESForm* locationForm)
    {
        if (!locationForm) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_questProgressionMutex);
            if (g_questProgressionLastLocationFormID == locationForm->GetFormID()) {
                return;
            }
            g_questProgressionLastLocationFormID = locationForm->GetFormID();
        }

        json payload;
        if (!PopulateQuestProgressionFormPayload(payload, "form_id", "plugin", locationForm)) {
            return;
        }
        if (locationForm->GetName() && locationForm->GetName()[0]) {
            payload["location_name"] = std::string(locationForm->GetName());
        }
        PostQuestProgressionEvent("location_entered", payload);
    }

    void PostQuestProgressionActorDead(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        const std::string actorKey = BuildQuestProgressionFormKey(actor);
        if (actorKey.empty()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_questProgressionMutex);
            if (g_questProgressionDeadActors.contains(actorKey)) {
                return;
            }
            g_questProgressionDeadActors.insert(actorKey);
        }

        json payload;
        if (!PopulateQuestProgressionFormPayload(payload, "form_id", "plugin", actor)) {
            return;
        }
        if (actor->GetDisplayFullName() && actor->GetDisplayFullName()[0]) {
            payload["actor_name"] = std::string(actor->GetDisplayFullName());
        }
        PostQuestProgressionEvent("actor_dead", payload);
    }

    void PostQuestProgressionInventoryDelta(const char* eventType, RE::TESForm* itemForm, int count)
    {
        if (!eventType || count <= 0 || !itemForm) {
            return;
        }

        json payload;
        if (!PopulateQuestProgressionFormPayload(payload, "form_id", "plugin", itemForm)) {
            return;
        }

        payload["count"] = count;
        if (itemForm->GetName() && itemForm->GetName()[0]) {
            payload["name"] = std::string(itemForm->GetName());
        }

        PostQuestProgressionEvent(eventType, payload);
    }

    std::string GetItemPickupFormTypeName(RE::FormType formType)
    {
        switch (formType) {
        case RE::FormType::Armor:
            return "armor";
        case RE::FormType::Weapon:
            return "weapon";
        case RE::FormType::Ammo:
            return "ammo";
        case RE::FormType::Book:
            return "book";
        case RE::FormType::Scroll:
            return "scroll";
        case RE::FormType::AlchemyItem:
            return "alchemy_item";
        case RE::FormType::Ingredient:
            return "ingredient";
        case RE::FormType::SoulGem:
            return "soul_gem";
        case RE::FormType::KeyMaster:
            return "key";
        case RE::FormType::Misc:
            return "misc";
        default:
            return "other";
        }
    }

    void FlushPlayerItemAcquiredTelemetryBatch()
    {
        while (true) {
            std::this_thread::sleep_for(kPlayerItemPickupBatchDelay);

            std::vector<json> batch;
            {
                std::lock_guard<std::mutex> lock(g_playerItemPickupBatchMutex);
                batch.swap(g_playerItemPickupBatch);
            }

            if (!batch.empty()) {
                if (batch.size() == 1) {
                    HTTPManager::postGameData("gamedata.php", batch.front());
                } else {
                    std::string actorName = "Player";
                    if (batch.front().contains("actor_name") && batch.front()["actor_name"].is_string()) {
                        actorName = batch.front()["actor_name"].get<std::string>();
                    }

                    HTTPManager::postGameData("gamedata.php", json{
                        {"type", "player_items_acquired"},
                        {"actor_type", "player"},
                        {"actor_name", actorName},
                        {"items", batch}
                    });
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_playerItemPickupBatchMutex);
                if (g_playerItemPickupBatch.empty()) {
                    g_playerItemPickupFlushScheduled.store(false);
                    return;
                }
            }
        }
    }

    void QueuePlayerItemAcquiredTelemetry(json payload)
    {
        {
            std::lock_guard<std::mutex> lock(g_playerItemPickupBatchMutex);
            g_playerItemPickupBatch.push_back(std::move(payload));
        }

        bool expected = false;
        if (g_playerItemPickupFlushScheduled.compare_exchange_strong(expected, true)) {
            std::thread(FlushPlayerItemAcquiredTelemetryBatch).detach();
        }
    }

    void PostPlayerItemAcquiredTelemetry(RE::TESForm* itemForm, const std::string& displayName, int count,
                                         RE::FormID source, bool barterSuppressed, bool craftingActive)
    {
        if (!pluginInited || !itemForm || count <= 0) {
            return;
        }

        const std::string pluginName = GetQuestProgressionPluginName(itemForm);
        if (pluginName.empty()) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        std::string itemName = displayName;
        if (itemName.empty() && itemForm->GetName() && itemForm->GetName()[0]) {
            itemName = itemForm->GetName();
        }

        const int goldValue = itemForm->GetGoldValue();
        json payload{
            {"type", "player_item_acquired"},
            {"actor_type", "player"},
            {"actor_name", player ? std::string(player->GetName()) : std::string("Player")},
            {"name", itemName},
            {"count", count},
            {"form_id", FormatQuestProgressionFormID(itemForm->GetLocalFormID())},
            {"plugin", pluginName},
            {"form_type", GetItemPickupFormTypeName(itemForm->GetFormType())},
            {"form_type_id", static_cast<std::uint32_t>(itemForm->GetFormType())},
            {"gold_value", goldValue},
            {"total_value", goldValue * count},
            {"gamets", GetGameTimeStamp()},
            {"barter_suppressed", barterSuppressed},
            {"crafting_active", craftingActive}
        };

        if (source != 0) {
            payload["source_form_id"] = FormatQuestProgressionFormID(source);
            auto* sourceForm = RE::TESForm::LookupByID(source);
            if (sourceForm) {
                payload["source_type"] = GetItemPickupFormTypeName(sourceForm->GetFormType());
                if (sourceForm->GetName() && sourceForm->GetName()[0]) {
                    payload["source_name"] = std::string(sourceForm->GetName());
                }

                if (auto* sourceActor = sourceForm->As<RE::Actor>()) {
                    payload["source_type"] = "actor";
                    if (sourceActor->GetDisplayFullName() && sourceActor->GetDisplayFullName()[0]) {
                        payload["source_name"] = std::string(sourceActor->GetDisplayFullName());
                    }
                } else if (auto* sourceRef = sourceForm->AsReference()) {
                    payload["source_type"] = "reference";
                    if (sourceRef->GetDisplayFullName() && sourceRef->GetDisplayFullName()[0]) {
                        payload["source_name"] = std::string(sourceRef->GetDisplayFullName());
                    }
                    if (auto* owner = sourceRef->GetActorOwner()) {
                        if (owner->GetFullName() && owner->GetFullName()[0]) {
                            payload["source_owner"] = std::string(owner->GetFullName());
                        }
                    }
                }
            }
        }

        QueuePlayerItemAcquiredTelemetry(std::move(payload));
    }

    void PostQuestProgressionInventorySync()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }

        json items = json::array();
        auto inventory = player->GetInventory();
        for (const auto& entry : inventory) {
            RE::TESBoundObject* item = entry.first;
            const auto count = entry.second.first;
            const std::unique_ptr<RE::InventoryEntryData>& entryData = entry.second.second;

            if (!item || count <= 0) {
                continue;
            }

            const std::string pluginName = GetQuestProgressionPluginName(item);
            if (pluginName.empty()) {
                continue;
            }

            std::string itemName;
            if (auto baseName = item->GetName(); baseName && baseName[0]) {
                itemName = baseName;
            }

            if (entryData) {
                if (auto display = entryData->GetDisplayName(); display && display[0]) {
                    itemName = display;
                }
            }

            items.push_back({
                {"name", itemName},
                {"baseid", FormatQuestProgressionFormID(item->GetLocalFormID())},
                {"plugin", pluginName},
                {"count", count}
            });
        }

        PostQuestProgressionEvent("player_inventory_sync", json{
            {"items", items}
        });
    }

    template <class... Args>
    bool DispatchQuestProgressionPapyrusCall(const char* functionName, Args... args)
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            logger::error("[QuestProgression] Papyrus VM unavailable for {}", functionName);
            return false;
        }

        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto packedArgs = RE::MakeFunctionArguments(std::move(args)...);
        const bool dispatched =
            vm->DispatchStaticCall("AIAgentQuestProgressionBridge", functionName, packedArgs, callback);
        if (!dispatched) {
            logger::error("[QuestProgression] Failed dispatching Papyrus bridge call {}", functionName);
        }
        return dispatched;
    }

    struct QuestProgressionActorDialogueStoryEvent
    {
        [[nodiscard]] static std::uint32_t& GetIndex()
        {
            static std::uint32_t index = 0;
            return index;
        }

        RE::ObjectRefHandle actor;
        RE::ObjectRefHandle target;
        RE::BGSLocation* location{ nullptr };
        std::uint32_t value1{ 0 };
        std::uint32_t value2{ 0 };
    };

    struct QuestProgressionChangeLocationStoryEvent
    {
        [[nodiscard]] static std::uint32_t& GetIndex()
        {
            static std::uint32_t index = 0;
            return index;
        }

        RE::ObjectRefHandle actor;
        RE::BGSLocation* oldLocation{ nullptr };
        RE::BGSLocation* newLocation{ nullptr };
    };

    json BuildQuestProgressionQuestState(RE::FormID questFormID)
    {
        json state = json::object();
        state["runtime_quest_form_id"] = questFormID;

        auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(questFormID);
        state["quest_found"] = quest != nullptr;
        if (!quest) {
            return state;
        }

        const char* editorID = quest->formEditorID.c_str();
        if (editorID && editorID[0] != '\0') {
            state["quest_editor_id"] = editorID;
        }
        state["running"] = quest->IsRunning();
        state["starting"] = quest->IsStarting();
        state["stopped"] = quest->IsStopped();
        state["stopping"] = quest->IsStopping();
        state["enabled"] = quest->IsEnabled();
        state["completed"] = quest->IsCompleted();
        state["current_stage"] = quest->GetCurrentStageID();
        return state;
    }

    RE::Actor* ResolveQuestProgressionActor(RE::FormID actorFormID, const std::string& actorName, json& result)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            result["actor_resolution_error"] = "player unavailable";
            return nullptr;
        }

        if (actorFormID) {
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormID)) {
                result["runtime_actor_form_id"] = actor->formID;
                return actor;
            }
            result["requested_actor_form_id"] = actorFormID;
        }

        std::string actorNameCn = trim(actorName);
        if (!actorNameCn.empty()) {
            if (auto* ref = findActorInCell(actorNameCn, player->GetParentCell(), player, 6000.0f, false)) {
                if (auto* actor = ref->As<RE::Actor>()) {
                    result["runtime_actor_form_id"] = actor->formID;
                    result["resolved_actor_name"] = actorNameCn;
                    return actor;
                }
            }
            result["requested_actor_name"] = actorNameCn;
        }

        result["runtime_actor_form_id"] = player->formID;
        result["actor_resolution_fallback"] = "player";
        return player;
    }

    bool WaitForQuestProgressionRunning(RE::FormID questFormID, int waitMs, json& result)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, waitMs));
        while (true) {
            json questState = BuildQuestProgressionQuestState(questFormID);
            result["quest_state"] = questState;
            if (questState.value("quest_found", false) && questState.value("running", false)) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result["error"] = "quest did not start from story event";
                result["verified"] = false;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool WaitForQuestProgressionStage(RE::FormID questFormID, int expectedStage, int waitMs, json& result)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, waitMs));
        while (true) {
            json questState = BuildQuestProgressionQuestState(questFormID);
            result["quest_state"] = questState;
            const bool reachedStage =
                questState.value("quest_found", false) &&
                questState.value("running", false) &&
                questState.value("current_stage", -1) >= expectedStage;
            if (reachedStage) {
                result["verified"] = true;
                result["stage"] = expectedStage;
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result["error"] = "quest did not reach expected stage";
                result["verified"] = false;
                result["expected_stage"] = expectedStage;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool ResolveQuestProgressionStoryEventIndex(RE::QuestEvent eventType, const char uniqueID[4], std::uint32_t& outIndex)
    {
        auto* manager = RE::BGSStoryEventManager::GetSingleton();
        if (!manager) {
            logger::error("[QuestProgression] Story event manager unavailable");
            return false;
        }

        const auto mapped = manager->registeredEventIDs.find(static_cast<std::uint32_t>(eventType));
        if (mapped != manager->registeredEventIDs.end()) {
            outIndex = mapped->second;
            return true;
        }

        for (std::uint32_t index = 0; index < manager->registeredEvents.size(); ++index) {
            const auto& registered = manager->registeredEvents[index];
            if (std::memcmp(registered.uniqueID, uniqueID, 4) == 0) {
                outIndex = index;
                return true;
            }
        }

        logger::error("[QuestProgression] Story event {}{}{}{} is not registered",
                      uniqueID[0], uniqueID[1], uniqueID[2], uniqueID[3]);
        return false;
    }

    bool SendQuestProgressionActorDialogueStoryEvent(RE::FormID actorFormID, const std::string& actorName, json& result)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* actor = ResolveQuestProgressionActor(actorFormID, actorName, result);
        auto* manager = RE::BGSStoryEventManager::GetSingleton();
        if (!actor || !player || !manager) {
            result["error"] = "missing actor, player, or story event manager";
            result["runtime_actor_form_id"] = actorFormID;
            return false;
        }

        std::uint32_t eventIndex = 0;
        constexpr char actorDialogueID[4] = { 'A', 'D', 'I', 'A' };
        if (!ResolveQuestProgressionStoryEventIndex(RE::QuestEvent::kActorDialogue, actorDialogueID, eventIndex)) {
            result["error"] = "actor dialogue story event not registered";
            return false;
        }

        QuestProgressionActorDialogueStoryEvent::GetIndex() = eventIndex;
        QuestProgressionActorDialogueStoryEvent event;
        event.actor = actor->GetHandle();
        event.target = player->GetHandle();
        event.location = actor->GetCurrentLocation();
        if (!event.location) {
            event.location = player->GetCurrentLocation();
        }

        const std::uint32_t queuedID = manager->AddEvent(event);
        logger::info("[QuestProgression] Queued ActorDialogue story event actor={:08X} target={:08X} eventIndex={} queuedID={}",
                     actor->formID, player->formID, eventIndex, queuedID);

        result["runtime_actor_form_id"] = actor->formID;
        result["story_event"] = "ADIA";
        result["story_event_index"] = eventIndex;
        result["story_event_queued_id"] = queuedID;
        if (event.location) {
            result["runtime_location_form_id"] = event.location->formID;
        }
        return true;
    }

    bool SendQuestProgressionChangeLocationStoryEvent(RE::FormID actorFormID, const std::string& actorName,
                                                      RE::FormID locationFormID,
                                                      RE::FormID oldLocationFormID, json& result)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* actor = ResolveQuestProgressionActor(actorFormID, actorName, result);
        auto* manager = RE::BGSStoryEventManager::GetSingleton();
        if (!actor || !player || !manager) {
            result["error"] = "missing actor, player, or story event manager";
            result["runtime_actor_form_id"] = actorFormID;
            return false;
        }

        auto* newLocation = locationFormID ? RE::TESForm::LookupByID<RE::BGSLocation>(locationFormID) : nullptr;
        if (!newLocation) {
            newLocation = actor->GetCurrentLocation();
        }
        if (!newLocation) {
            newLocation = player->GetCurrentLocation();
        }
        if (!newLocation) {
            result["error"] = "missing change-location destination";
            result["runtime_actor_form_id"] = actor->formID;
            return false;
        }

        auto* oldLocation = oldLocationFormID ? RE::TESForm::LookupByID<RE::BGSLocation>(oldLocationFormID) : nullptr;
        std::uint32_t eventIndex = 0;
        constexpr char changeLocationID[4] = { 'C', 'L', 'O', 'C' };
        if (!ResolveQuestProgressionStoryEventIndex(RE::QuestEvent::kChangeLocation, changeLocationID, eventIndex)) {
            result["error"] = "change location story event not registered";
            return false;
        }

        QuestProgressionChangeLocationStoryEvent::GetIndex() = eventIndex;
        QuestProgressionChangeLocationStoryEvent event;
        event.actor = actor->GetHandle();
        event.oldLocation = oldLocation;
        event.newLocation = newLocation;

        const std::uint32_t queuedID = manager->AddEvent(event);
        logger::info("[QuestProgression] Queued ChangeLocation story event actor={:08X} oldLocation={:08X} newLocation={:08X} eventIndex={} queuedID={}",
                     actor->formID, oldLocation ? oldLocation->formID : 0, newLocation->formID, eventIndex, queuedID);

        result["runtime_actor_form_id"] = actor->formID;
        result["story_event"] = "CLOC";
        result["story_event_index"] = eventIndex;
        result["story_event_queued_id"] = queuedID;
        result["runtime_location_form_id"] = newLocation->formID;
        if (oldLocation) {
            result["runtime_old_location_form_id"] = oldLocation->formID;
        }
        return true;
    }

    bool EnsureQuestProgressionQuestStarted(RE::FormID questFormID, int waitMs, json& result)
    {
        auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(questFormID);
        if (!quest) {
            result["error"] = "quest not found for native startup";
            result["runtime_quest_form_id"] = questFormID;
            return false;
        }

        bool ensureQuestResult = false;
        const bool ensureReturned = quest->EnsureQuestStarted(ensureQuestResult, true);
        result["ensure_quest_started_return"] = ensureReturned;
        result["ensure_quest_started_result"] = ensureQuestResult;

        if (auto* storyTeller = RE::BGSStoryTeller::GetSingleton()) {
            if (!quest->IsRunning() || quest->IsStopped()) {
                storyTeller->BeginStartUpQuest(quest);
                result["begin_startup_quest_called"] = true;
            }
        } else {
            result["begin_startup_quest_error"] = "story teller unavailable";
        }

        if (waitMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        }
        result["quest_state_after_native_start"] = BuildQuestProgressionQuestState(questFormID);
        return true;
    }

    QuestActionExecutionResult ExecuteQuestProgressionAction(const json& action)
    {
        QuestActionExecutionResult execution;
        execution.result = json::object();

        if (!action.is_object()) {
            execution.result["error"] = "invalid action payload";
            return execution;
        }

        const std::string actionType = action.value("action_type", std::string());
        const json payload = action.value("payload", json::object());
        execution.result["action_type"] = actionType;

        auto resolvePayloadForm = [&payload](const char* formField, const char* pluginField) -> RE::FormID {
            if (!payload.contains(formField) || !payload[formField].is_string()) {
                return 0;
            }
            return ResolveQuestProgressionRuntimeFormID(
                payload.value(pluginField, std::string()),
                payload.value(formField, std::string()));
        };

        auto finishSuccess = [&execution](const json& extraResult = json::object()) {
            execution.success = true;
            execution.status = "applied";
            execution.result["applied_by"] = "skse_plugin";
            for (auto it = extraResult.begin(); it != extraResult.end(); ++it) {
                execution.result[it.key()] = it.value();
            }
        };

        if (actionType == "set_stage") {
            const RE::FormID questFormID = resolvePayloadForm("quest_form_id", "quest_plugin");
            const int stage = payload.value("stage", -1);
            if (!questFormID || stage < 0) {
                execution.result["error"] = "missing quest or stage";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("SetQuestStage", static_cast<int>(questFormID), stage)) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_quest_form_id", questFormID}, {"stage", stage}});
            return execution;
        }

        if (actionType == "set_objective_completed" || actionType == "cross_quest_set_objective_completed") {
            const RE::FormID questFormID = resolvePayloadForm("quest_form_id", "quest_plugin");
            const int objectiveIndex = payload.value("index", -1);
            const bool completed = payload.value("completed", true);
            if (!questFormID || objectiveIndex < 0) {
                execution.result["error"] = "missing quest or objective index";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("SetQuestObjectiveCompleted", static_cast<int>(questFormID),
                                                     objectiveIndex, completed)) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_quest_form_id", questFormID}, {"objective_index", objectiveIndex}});
            return execution;
        }

        if (actionType == "set_objective_displayed") {
            const RE::FormID questFormID = resolvePayloadForm("quest_form_id", "quest_plugin");
            const int objectiveIndex = payload.value("index", -1);
            const bool displayed = payload.value("displayed", true);
            const bool forceDisplayed = payload.value("force_displayed", false);
            if (!questFormID || objectiveIndex < 0) {
                execution.result["error"] = "missing quest or objective index";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("SetQuestObjectiveDisplayed", static_cast<int>(questFormID),
                                                     objectiveIndex, displayed, forceDisplayed)) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_quest_form_id", questFormID}, {"objective_index", objectiveIndex}});
            return execution;
        }

        if (actionType == "fail_all_objectives") {
            const RE::FormID questFormID = resolvePayloadForm("quest_form_id", "quest_plugin");
            if (!questFormID) {
                execution.result["error"] = "missing quest";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("FailAllQuestObjectives", static_cast<int>(questFormID))) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_quest_form_id", questFormID}});
            return execution;
        }

        if (actionType == "cross_quest_start") {
            const RE::FormID questFormID = resolvePayloadForm("quest_form_id", "quest_plugin");
            if (!questFormID) {
                execution.result["error"] = "missing quest";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("StartQuest", static_cast<int>(questFormID))) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_quest_form_id", questFormID}});
            return execution;
        }

        if (actionType == "start_quest_stage_objective") {
            const RE::FormID questFormID = resolvePayloadForm("quest_form_id", "quest_plugin");
            const int stage = payload.value("stage", -1);
            const int objectiveIndex = payload.value("objective_index", -1);
            const int verifyWaitMs = std::max(0, payload.value("verify_wait_ms", 1500));
            if (!questFormID || stage < 0 || objectiveIndex < 0) {
                execution.result["error"] = "missing quest, stage, or objective index";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("StartQuestStageObjective", static_cast<int>(questFormID), stage,
                                                     objectiveIndex)) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            if (!WaitForQuestProgressionStage(questFormID, stage, verifyWaitMs, execution.result)) {
                return execution;
            }
            finishSuccess({
                {"runtime_quest_form_id", questFormID},
                {"stage", stage},
                {"objective_index", objectiveIndex}
            });
            return execution;
        }

        if (actionType == "actor_dialogue_start_quest_stage_objective") {
            const RE::FormID actorFormID = resolvePayloadForm("actor_form_id", "actor_plugin");
            const std::string actorName = payload.value("actor_name", payload.value("npc_name", std::string()));
            const RE::FormID questFormID = resolvePayloadForm("quest_form_id", "quest_plugin");
            const int stage = payload.value("stage", -1);
            const int objectiveIndex = payload.value("objective_index", -1);
            const int waitMs = std::max(0, payload.value("story_event_wait_ms", 500));
            const int verifyWaitMs = std::max(0, payload.value("verify_wait_ms", 1500));
            if ((!actorFormID && trim(actorName).empty()) || !questFormID || stage < 0 || objectiveIndex < 0) {
                execution.result["error"] = "missing actor, quest, stage, or objective index";
                return execution;
            }
            execution.result["quest_state_before"] = BuildQuestProgressionQuestState(questFormID);
            if (!SendQuestProgressionActorDialogueStoryEvent(actorFormID, actorName, execution.result)) {
                return execution;
            }
            if (waitMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
            }
            execution.result["quest_state_after_story_event"] = BuildQuestProgressionQuestState(questFormID);
            if (!DispatchQuestProgressionPapyrusCall("StartQuestStageObjective", static_cast<int>(questFormID), stage,
                                                     objectiveIndex)) {
                execution.result["error"] = "papyrus dispatch failed after actor dialogue story event";
                return execution;
            }
            if (!WaitForQuestProgressionStage(questFormID, stage, verifyWaitMs, execution.result)) {
                return execution;
            }
            finishSuccess({
                {"runtime_quest_form_id", questFormID},
                {"stage", stage},
                {"objective_index", objectiveIndex}
            });
            return execution;
        }

        if (actionType == "change_location_start_quest_stage_objective") {
            const RE::FormID actorFormID = resolvePayloadForm("actor_form_id", "actor_plugin");
            const std::string actorName = payload.value("actor_name", payload.value("npc_name", std::string()));
            const RE::FormID questFormID = resolvePayloadForm("quest_form_id", "quest_plugin");
            const RE::FormID locationFormID = resolvePayloadForm("location_form_id", "location_plugin");
            const RE::FormID oldLocationFormID = resolvePayloadForm("old_location_form_id", "old_location_plugin");
            const int stage = payload.value("stage", -1);
            const int objectiveIndex = payload.value("objective_index", -1);
            const int waitMs = std::max(0, payload.value("story_event_wait_ms", 1000));
            const int verifyWaitMs = std::max(0, payload.value("verify_wait_ms", 1500));
            const int nativeStartWaitMs = std::max(0, payload.value("native_start_wait_ms", 750));
            if (!questFormID || stage < 0 || objectiveIndex < 0) {
                execution.result["error"] = "missing quest, stage, or objective index";
                return execution;
            }

            execution.result["quest_state_before"] = BuildQuestProgressionQuestState(questFormID);
            if (execution.result["quest_state_before"].value("current_stage", -1) >= stage) {
                execution.result["verified"] = true;
                finishSuccess({
                    {"runtime_quest_form_id", questFormID},
                    {"stage", stage},
                    {"objective_index", objectiveIndex},
                    {"already_at_stage", true}
                });
                return execution;
            }

            if (!SendQuestProgressionChangeLocationStoryEvent(actorFormID, actorName, locationFormID, oldLocationFormID,
                                                               execution.result)) {
                return execution;
            }
            if (waitMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
            }
            execution.result["quest_state_after_story_event"] = BuildQuestProgressionQuestState(questFormID);
            if (payload.value("native_ensure_start", true)) {
                if (!EnsureQuestProgressionQuestStarted(questFormID, nativeStartWaitMs, execution.result)) {
                    return execution;
                }
            }

            if (!DispatchQuestProgressionPapyrusCall("SetQuestStageObjective", static_cast<int>(questFormID), stage,
                                                     objectiveIndex)) {
                execution.result["error"] = "papyrus stage/objective dispatch failed after change location story event";
                return execution;
            }
            if (!WaitForQuestProgressionStage(questFormID, stage, verifyWaitMs, execution.result)) {
                return execution;
            }
            finishSuccess({
                {"runtime_quest_form_id", questFormID},
                {"stage", stage},
                {"objective_index", objectiveIndex}
            });
            return execution;
        }

        if (actionType == "console_command") {
            const std::string command = payload.value("command", std::string());
            if (command.empty()) {
                execution.result["error"] = "missing command";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("ExecuteConsoleCommand", command)) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"command", command}});
            return execution;
        }

        if (actionType == "console_command_sequence") {
            std::vector<std::string> commands;
            if (payload.contains("commands") && payload["commands"].is_array()) {
                for (const auto& commandValue : payload["commands"]) {
                    if (commandValue.is_string()) {
                        std::string command = commandValue.get<std::string>();
                        if (!command.empty()) {
                            commands.push_back(std::move(command));
                        }
                    }
                }
            }

            std::string commandSequence = payload.value("command_sequence", std::string());
            if (commandSequence.empty() && !commands.empty()) {
                for (std::size_t index = 0; index < commands.size(); ++index) {
                    if (index > 0) {
                        commandSequence += "||";
                    }
                    commandSequence += commands[index];
                }
            }

            if (commandSequence.empty()) {
                execution.result["error"] = "missing commands";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("ExecuteConsoleCommandSequence", commandSequence)) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"command_sequence", commandSequence}});
            return execution;
        }

        if (actionType == "stop_quest") {
            const RE::FormID questFormID = resolvePayloadForm("quest_form_id", "quest_plugin");
            if (!questFormID) {
                execution.result["error"] = "missing quest";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("StopQuest", static_cast<int>(questFormID))) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_quest_form_id", questFormID}});
            return execution;
        }

        if (actionType == "start_scene") {
            const RE::FormID sceneFormID = resolvePayloadForm("form_id", "plugin");
            if (!sceneFormID) {
                execution.result["error"] = "missing scene";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("StartScene", static_cast<int>(sceneFormID))) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_scene_form_id", sceneFormID}});
            return execution;
        }

        if (actionType == "set_actor_value") {
            const RE::FormID actorFormID = resolvePayloadForm("actor_form_id", "plugin");
            const std::string actorValue = payload.value("av", std::string());
            const float value = payload.value("value", 0.0f);
            if (!actorFormID || actorValue.empty()) {
                execution.result["error"] = "missing actor or actor value";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("SetActorValue", static_cast<int>(actorFormID), actorValue,
                                                     value)) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_actor_form_id", actorFormID}, {"actor_value", actorValue}});
            return execution;
        }

        if (actionType == "set_ghost") {
            const RE::FormID actorFormID = resolvePayloadForm("actor_form_id", "plugin");
            const bool ghost = payload.value("ghost", true);
            if (!actorFormID) {
                execution.result["error"] = "missing actor";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("SetActorGhost", static_cast<int>(actorFormID), ghost)) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_actor_form_id", actorFormID}, {"ghost", ghost}});
            return execution;
        }

        if (actionType == "evaluate_package") {
            const RE::FormID actorFormID = resolvePayloadForm("actor_form_id", "plugin");
            if (!actorFormID) {
                execution.result["error"] = "missing actor";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("EvaluateActorPackage", static_cast<int>(actorFormID))) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_actor_form_id", actorFormID}});
            return execution;
        }

        if (actionType == "remove_item" || actionType == "add_item") {
            const std::string target = QuestProgressionToLower(payload.value("target", std::string("player")));
            const RE::FormID itemFormID = resolvePayloadForm("item_form_id", "plugin");
            const int count = std::max(payload.value("count", 1), 1);
            const bool silent = payload.value("silent", false);
            if (!itemFormID) {
                execution.result["error"] = "missing item";
                return execution;
            }
            if (target != "player") {
                execution.result["error"] = "unsupported inventory target";
                return execution;
            }

            const char* functionName = (actionType == "remove_item") ? "RemoveItemFromPlayer" : "AddItemToPlayer";
            if (!DispatchQuestProgressionPapyrusCall(functionName, static_cast<int>(itemFormID), count, silent)) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }

            {
                std::lock_guard<std::mutex> lock(g_questProgressionMutex);
                const std::string formKey = BuildQuestProgressionFormKey(
                    payload.value("plugin", std::string()),
                    payload.value("item_form_id", std::string()));
                if (actionType == "remove_item") {
                    AddSuppressedInventoryDelta(g_questProgressionSuppressedRemovals, formKey, count);
                } else {
                    AddSuppressedInventoryDelta(g_questProgressionSuppressedAdds, formKey, count);
                }
            }

            finishSuccess({{"runtime_item_form_id", itemFormID}, {"count", count}});
            return execution;
        }

        if (actionType == "enable_ref") {
            const RE::FormID refFormID = resolvePayloadForm("form_id", "plugin");
            const bool fadeIn = payload.value("fade_in", false);
            if (!refFormID) {
                execution.result["error"] = "missing reference";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("EnableReference", static_cast<int>(refFormID), fadeIn)) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_ref_form_id", refFormID}});
            return execution;
        }

        if (actionType == "set_relationship_rank") {
            const RE::FormID actorFormID = resolvePayloadForm("actor_form_id", "plugin");
            const int rank = payload.value("rank", 0);
            const std::string target = QuestProgressionToLower(payload.value("target", std::string("player")));
            if (!actorFormID) {
                execution.result["error"] = "missing actor";
                return execution;
            }
            if (target != "player") {
                execution.result["error"] = "unsupported relationship target";
                return execution;
            }
            if (!DispatchQuestProgressionPapyrusCall("SetActorRelationshipToPlayer", static_cast<int>(actorFormID),
                                                     rank)) {
                execution.result["error"] = "papyrus dispatch failed";
                return execution;
            }
            finishSuccess({{"runtime_actor_form_id", actorFormID}, {"rank", rank}});
            return execution;
        }

        execution.result["error"] = "unsupported action type";
        return execution;
    }

    void QueueQuestProgressionAction(const json& action)
    {
        if (!action.is_object()) {
            return;
        }

        const std::int64_t actionID = action.value("id", static_cast<std::int64_t>(0));
        if (actionID <= 0) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_questProgressionMutex);
            if (g_questProgressionActionsInFlight.contains(actionID)) {
                return;
            }
            g_questProgressionActionsInFlight.insert(actionID);
        }

        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            HTTPManager::postGameData("gamedata.php", json{
                {"type", "quest_action_ack"},
                {"action_id", actionID},
                {"status", "failed"},
                {"result", {{"error", "task interface unavailable"}, {"applied_by", "skse_plugin"}}}
            });
            std::lock_guard<std::mutex> lock(g_questProgressionMutex);
            g_questProgressionActionsInFlight.erase(actionID);
            return;
        }

        taskInterface->AddTask([action, actionID]() {
            QuestActionExecutionResult execution;
            if (!AIQuestProgressionEnabled || !pluginInited) {
                execution.success = false;
                execution.status = "failed";
                execution.result = {
                    {"error", "quest progression disabled before execution"},
                    {"applied_by", "skse_plugin"}
                };
            } else {
                execution = ExecuteQuestProgressionAction(action);
            }
            HTTPManager::postGameData("gamedata.php", json{
                {"type", "quest_action_ack"},
                {"action_id", actionID},
                {"status", execution.status},
                {"result", execution.result}
            });

            std::lock_guard<std::mutex> lock(g_questProgressionMutex);
            g_questProgressionActionsInFlight.erase(actionID);
        });
    }

    void SyncQuestProgressionState(bool includeInventory, const char* reason)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }

        logger::info("[QuestProgression] Running full resync ({}, includeInventory={})",
                     reason ? reason : "unspecified", includeInventory);

        if (auto* cell = player->GetParentCell()) {
            PostQuestProgressionLocationEntered(cell);
        }

        auto* storyTeller = RE::BGSStoryTeller::GetSingleton();
        if (storyTeller) {
            for (std::size_t i = 0; i < storyTeller->infoClearQuests.size(); ++i) {
                RE::TESQuest* quest = storyTeller->infoClearQuests[i];
                if (!quest || !quest->IsActive() || quest->IsCompleted() || !quest->IsEnabled()) {
                    continue;
                }
                PostQuestProgressionQuestStage(quest, quest->GetCurrentStageID());
            }
        }

        if (includeInventory) {
            PostQuestProgressionInventorySync();
        }
    }
}

void ResetQuestProgressionBridgeState()
{
    std::lock_guard<std::mutex> lock(g_questProgressionMutex);
    g_questProgressionLastStages.clear();
    g_questProgressionDeadActors.clear();
    g_questProgressionSuppressedAdds.clear();
    g_questProgressionSuppressedRemovals.clear();
    g_questProgressionActionsInFlight.clear();
    g_questProgressionLastLocationFormID = 0;
    g_questProgressionResyncScheduled = false;
    g_questProgressionResyncIncludeInventory = true;
    g_questProgressionResyncDueAt = std::chrono::steady_clock::time_point::max();
    g_questProgressionLastPollAt = std::chrono::steady_clock::time_point::min();
    g_questProgressionLastStatusPollAt = std::chrono::steady_clock::time_point::min();
}

void ScheduleQuestProgressionFullResync(const char* reason, int delayMs, bool includeInventory)
{
    if (!AIQuestProgressionEnabled) {
        return;
    }

    const auto dueAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(delayMs, 0));
    {
        std::lock_guard<std::mutex> lock(g_questProgressionMutex);
        if (!g_questProgressionResyncScheduled || dueAt < g_questProgressionResyncDueAt) {
            g_questProgressionResyncDueAt = dueAt;
        }
        g_questProgressionResyncScheduled = true;
        g_questProgressionResyncIncludeInventory =
            g_questProgressionResyncIncludeInventory || includeInventory;
    }

    logger::info("[QuestProgression] Scheduled full resync in {} ms ({}, includeInventory={})",
                 std::max(delayMs, 0), reason ? reason : "unspecified", includeInventory);
}

static void MaybeRunScheduledQuestProgressionResync()
{
    if (!pluginInited || !AIQuestProgressionEnabled) {
        return;
    }

    bool shouldRun = false;
    bool includeInventory = true;
    {
        std::lock_guard<std::mutex> lock(g_questProgressionMutex);
        if (g_questProgressionResyncScheduled &&
            std::chrono::steady_clock::now() >= g_questProgressionResyncDueAt) {
            shouldRun = true;
            includeInventory = g_questProgressionResyncIncludeInventory;
            g_questProgressionResyncScheduled = false;
            g_questProgressionResyncIncludeInventory = true;
            g_questProgressionResyncDueAt = std::chrono::steady_clock::time_point::max();
        }
    }

    if (shouldRun) {
        SyncQuestProgressionState(includeInventory, "scheduled");
    }
}

static void PollQuestProgressionActions()
{
    if (!pluginInited || !AIQuestProgressionEnabled) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_questProgressionMutex);
        if (g_questProgressionLastPollAt != std::chrono::steady_clock::time_point::min() &&
            now - g_questProgressionLastPollAt < kQuestProgressionPollInterval) {
            return;
        }
        g_questProgressionLastPollAt = now;
    }

    json response = HTTPManager::postGameDataJson("gamedata.php", json{
        {"type", "quest_action_poll"},
        {"limit", 25}
    }, 5000);

    if (!response.is_object() || !response.value("ok", false) || !response.contains("actions") ||
        !response["actions"].is_array()) {
        return;
    }

    for (const auto& action : response["actions"]) {
        QueueQuestProgressionAction(action);
    }
}

static void SyncQuestProgressionEnabledFromServer()
{
    if (!pluginInited) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_questProgressionMutex);
        if (g_questProgressionLastStatusPollAt != std::chrono::steady_clock::time_point::min() &&
            now - g_questProgressionLastStatusPollAt < kQuestProgressionStatusPollInterval) {
            return;
        }
        g_questProgressionLastStatusPollAt = now;
    }

    json response = HTTPManager::postGameDataJson("gamedata.php", json{
        {"type", "quest_status"}
    }, 1500);

    if (!response.is_object() || !response.contains("enabled") || !response["enabled"].is_boolean()) {
        return;
    }

    const bool serverEnabled = response["enabled"].get<bool>();
    const bool wasEnabled = AIQuestProgressionEnabled;
    if (wasEnabled == serverEnabled) {
        return;
    }

    AIQuestProgressionEnabled = serverEnabled;
    logger::info("[QuestProgression] Server global setting changed to {}", AIQuestProgressionEnabled);
    if (AIQuestProgressionEnabled) {
        ResetQuestProgressionBridgeState();
        ScheduleQuestProgressionFullResync("server_config_enable", 2500, true);
    } else {
        ResetQuestProgressionBridgeState();
    }
}

/* EVENT HOOKS. */

OnInit {

    logger::info("CHIM - Skyrim Plugin - Version {} ({})", PLUGIN_VERSION, PLUGIN_RELEASE_DATE);
    // Disabled TrackedStatsEvent
    // Disabled FastTravelEvent
    
    ProcessorSerialization::InitializeSerialization();
    logger::info("CHIM - Skyrim Plugin - Init Done");
}

OnFormsLoaded {
    logger::info("OnFormsLoaded");
    int a = 0;
}

OnSaveGame{
    logger::info("OnSaveGame");
    if (!pluginInited) {
        logger::info("Game saved first time ");

        RE::FormID NullVoiceTypeId =
            RE::TESDataHandler::GetSingleton()->LookupFormID((RE::FormID)0x1D70E, "AIAgent.esp");
        NullVoiceType = RE::TESForm::LookupByID(NullVoiceTypeId);

        logger::info("AIAgent.esp present");
        if (!Conf::getInstance().isOk()) {
            RE::DebugNotification("[CHIM] AIAgent.ini is missing or invalid.");
        }

        const bool serverReachable = Conf::getInstance().ping();
        if (!serverReachable) {
            RE::DebugNotification("[CHIM] Cannot connect to the server. Check AIAgent.ini.");
        }

        // setNewActionModeFromConfig();

        
        // Queue cleaning
        BackGroundDialogueQueue.clear();
        HTTPManager::log(std::format("wipe|{}|{}|", getCurrentTimeMillis(), GetGameTimeStamp()));

        auto now = std::chrono::high_resolution_clock::now();
        controlLastBoredTriggerTS = now;

        isGameReady = true;

        ProcedureSendActiveQuests();

        SpeakManager::getInstance().deleteQueue();

        ManagerMainQueue& mmq = ManagerMainQueue::getInstance();
        if (!mmq.isRunning()) {
            logger::info("Starting main manager thread  at OnSaveGame");
            std::string polint = Conf::getInstance().getPolint();
            int polint_i = std::stoi(polint);
            mmq.startThread(polint_i);
        }

        AIAgentManager& aiam = AIAgentManager::getInstance();

        auto oldNarrator = aiam.getAgentByName(NARRATOR_NAME);
        if (oldNarrator) aiam.deleteAgent(oldNarrator);

        // Lets add the player/narrator agent

        auto player = RE::PlayerCharacter::GetSingleton();

        auto narrator = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        player->SetDisplayName(player->GetName(), true);
        auto agent = aiam.createAgent();
        agent->setActor(narrator);
        agent->setAvailable(true);
        agent->setNarrator(true);
        agent->setOriginalVoice(narrator->GetActorBase()->voiceType);
        agent->overrideActorName(NARRATOR_NAME);
        logger::info("Narrator initialized for {}", narrator->GetDisplayFullName());
        aiam.addAgent(agent);

        aiam.setPlayerName(player->GetName());

        auto server = Conf::getInstance().getServer();
        auto port = Conf::getInstance().getPort();
        auto initMsg = std::format("[CHIM] Using server: http://{}:{}", server, port);
        RE::DebugNotification(initMsg.c_str());

        pendingLoadedPluginManifestSync = true;
        if (PostLoadedPluginManifest()) {
            pendingLoadedPluginManifestSync = false;
        } else {
            logger::warn("[LOADED_PLUGINS] Immediate sync from OnSaveGame init path did not post; deferring until a loaded cell event");
        }

        std::string playerinfo;

        playerinfo.assign(std::format("level:{},name:\"{}\",race:\"{}\",gender:\"{}\"", player->GetLevel(),
                                      player->GetDisplayFullName(), player->GetRace()->GetFullName(),
                                      (player->GetActorBase()->GetSex() == RE::SEXES::kFemale) ? "female" : "male"
        ));

        logger::debug("Sending init msg");
        HTTPManager::log(std::format("infoplayer|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), playerinfo));

        pluginInited = true;
        ResetQuestProgressionBridgeState();
        ScheduleQuestProgressionFullResync("startup_save_init", 4000, true);
    } else {
        HTTPManager::log(std::format("infosave|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp()));
    }


}

// Track last known equipment/inventory/skills/stats hashes to avoid duplicate updates
std::unordered_map<uint32_t, std::string> lastEquipmentHash;
std::unordered_map<uint32_t, std::string> lastInventoryHash;
std::unordered_map<uint32_t, std::string> lastSkillsHash;
std::unordered_map<uint32_t, std::string> lastStatsHash;
std::unordered_map<uint32_t, std::string> lastSpellsHash;

// Track last update times for periodic updates
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> lastSkillsUpdate;
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> lastStatsUpdate;

static bool IsWornHeadgear(RE::Actor* actor, RE::TESBoundObject* boundObject)
{
    if (!actor || !boundObject) {
        return false;
    }

    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
    return actor->GetWornArmor(Slot::kHead) == boundObject ||
           actor->GetWornArmor(Slot::kHair) == boundObject ||
           actor->GetWornArmor(Slot::kLongHair) == boundObject ||
           actor->GetWornArmor(Slot::kCirclet) == boundObject;
}

struct EquipmentSlotItem
{
    std::string name;
    std::string baseid;
    json keywords = json::array();
};

struct InventoryItemSnapshot
{
    std::string name;
    std::string baseid;
    int count = 0;
    json keywords = json::array();
    std::string hashEntry;
    int gold = 0;
};

struct ModdedEquipmentSlot
{
    const char* key;
    RE::BGSBipedObjectForm::BipedObjectSlot slot;
};

static constexpr std::array<ModdedEquipmentSlot, 15> kModdedEquipmentSlots{{
    {"mod_mouth", RE::BGSBipedObjectForm::BipedObjectSlot::kModMouth},
    {"mod_neck", RE::BGSBipedObjectForm::BipedObjectSlot::kModNeck},
    {"cape", RE::BGSBipedObjectForm::BipedObjectSlot::kModChestPrimary},
    {"backpack", RE::BGSBipedObjectForm::BipedObjectSlot::kModBack},
    {"mod_misc1", RE::BGSBipedObjectForm::BipedObjectSlot::kModMisc1},
    {"mod_pelvis_primary", RE::BGSBipedObjectForm::BipedObjectSlot::kModPelvisPrimary},
    {"mod_pelvis_secondary", RE::BGSBipedObjectForm::BipedObjectSlot::kModPelvisSecondary},
    {"mod_leg_right", RE::BGSBipedObjectForm::BipedObjectSlot::kModLegRight},
    {"mod_leg_left", RE::BGSBipedObjectForm::BipedObjectSlot::kModLegLeft},
    {"mod_face_jewelry", RE::BGSBipedObjectForm::BipedObjectSlot::kModFaceJewelry},
    {"shirt", RE::BGSBipedObjectForm::BipedObjectSlot::kModChestSecondary},
    {"mod_shoulder", RE::BGSBipedObjectForm::BipedObjectSlot::kModShoulder},
    {"mod_arm_left", RE::BGSBipedObjectForm::BipedObjectSlot::kModArmLeft},
    {"mod_arm_right", RE::BGSBipedObjectForm::BipedObjectSlot::kModArmRight},
    {"mod_misc2", RE::BGSBipedObjectForm::BipedObjectSlot::kModMisc2},
}};

static bool HasWornExtra(const std::unique_ptr<RE::InventoryEntryData>& entryData)
{
    if (!entryData || !entryData->extraLists) {
        return false;
    }

    for (auto* xList : *entryData->extraLists) {
        if (xList && xList->HasType(RE::ExtraDataType::kWorn)) {
            return true;
        }
    }

    return false;
}

static bool HasArmorSlot(RE::TESObjectARMO* armor, RE::BGSBipedObjectForm::BipedObjectSlot slot)
{
    if (!armor) {
        return false;
    }

    auto slotMask = static_cast<uint64_t>(armor->GetSlotMask());
    return (slotMask & static_cast<uint64_t>(slot)) != 0;
}

static json CollectItemKeywords(RE::TESBoundObject* boundObject)
{
    json keywords = json::array();
    if (!boundObject) {
        return keywords;
    }

    auto* keywordForm = boundObject->As<RE::BGSKeywordForm>();
    if (!keywordForm) {
        return keywords;
    }

    std::set<std::string> seen;
    for (auto* keyword : keywordForm->GetKeywords()) {
        if (!keyword) {
            continue;
        }

        std::string keywordID;
        if (const char* editorID = keyword->GetFormEditorID(); editorID && editorID[0]) {
            keywordID = editorID;
        } else {
            keywordID = std::format("{:08X}", keyword->GetFormID());
        }

        if (!keywordID.empty() && seen.insert(keywordID).second) {
            keywords.push_back(keywordID);
        }
    }

    return keywords;
}

static void AddModdedEquipmentSlots(RE::TESObjectARMO* armor, const std::string& itemName, const std::string& baseID, const json& keywords, std::unordered_map<std::string, EquipmentSlotItem>& moddedEquipment)
{
    if (!armor || itemName.empty()) {
        return;
    }

    for (const auto& slot : kModdedEquipmentSlots) {
        if (HasArmorSlot(armor, slot.slot)) {
            moddedEquipment[slot.key] = {itemName, baseID, keywords};
        }
    }
}

static bool HasAnyModdedEquipmentSlot(RE::TESObjectARMO* armor)
{
    if (!armor) {
        return false;
    }

    for (const auto& slot : kModdedEquipmentSlots) {
        if (HasArmorSlot(armor, slot.slot)) {
            return true;
        }
    }

    return false;
}

static void ApplyLegacyModdedEquipmentSlot(const std::unordered_map<std::string, EquipmentSlotItem>& moddedEquipment, const std::string& key, std::string& itemName, std::string& baseID, json& keywords)
{
    auto found = moddedEquipment.find(key);
    if (found == moddedEquipment.end()) {
        return;
    }

    itemName = found->second.name;
    baseID = found->second.baseid;
    keywords = found->second.keywords;
}

static json BuildEquipmentItemJson(const std::string& name, const std::string& baseID, const json& keywords)
{
    return {
        {"name", name},
        {"baseid", baseID},
        {"keywords", keywords.is_array() ? keywords : json::array()}
    };
}

static void AddModdedEquipmentToJson(json& equipmentJson, const std::unordered_map<std::string, EquipmentSlotItem>& moddedEquipment)
{
    for (const auto& slot : kModdedEquipmentSlots) {
        auto found = moddedEquipment.find(slot.key);
        const std::string name = found != moddedEquipment.end() ? found->second.name : "";
        const std::string baseID = found != moddedEquipment.end() ? found->second.baseid : "";
        const json keywords = found != moddedEquipment.end() ? found->second.keywords : json::array();
        equipmentJson[slot.key] = BuildEquipmentItemJson(name, baseID, keywords);
    }
}

static std::string BuildModdedEquipmentHash(const std::unordered_map<std::string, EquipmentSlotItem>& moddedEquipment)
{
    std::string hash;
    for (const auto& slot : kModdedEquipmentSlots) {
        auto found = moddedEquipment.find(slot.key);
        hash.append("|").append(slot.key).append("=");
        if (found != moddedEquipment.end()) {
            hash.append(found->second.name).append("^").append(found->second.baseid).append("^").append(found->second.keywords.dump());
        }
    }

    return hash;
}

// Helper function to refresh equipment for an AI Agent (with hash-based diffing)
static void RefreshAIAgentEquipmentImpl(RE::Actor* npc, const std::string& agentName, bool forceUpdate) {
    if (!npc) return;

    // Skip player
    if (npc->GetFormID() == RE::PlayerCharacter::GetSingleton()->As<RE::Actor>()->GetFormID()) return;

    // Comprehensive safety checks to prevent crashes during cell transitions
    if (npc->IsDeleted() || npc->IsDisabled()) {
        return;
    }

    // Check if actor has 3D loaded (critical for equipment access)
    if (!npc->Is3DLoaded()) {
        return;
    }

    // Check if actor has a valid parent cell
    if (!npc->GetParentCell()) {
        return;
    }

    // Additional check: verify the actor pointer is still valid
    try {
        auto testName = npc->GetName();  // This will crash if the pointer is invalid
        if (!testName) {
            logger::trace("[EQUIPMENT_SKIP] {} - invalid actor pointer", agentName);
            return;
        }
    } catch (...) {
        logger::error("[EQUIPMENT_ERROR] {} - caught exception during validity check", agentName);
        return;
    }

    std::string helmet, helmet_baseid;
    std::string armor, armor_baseid;
    std::string boots, boots_baseid;
    std::string gloves, gloves_baseid;
    std::string amulet, amulet_baseid;
    std::string ring, ring_baseid;
    std::string cape, cape_baseid;
    std::string backpack, backpack_baseid;
    std::string leftHand, leftHand_baseid;
    std::string rightHand, rightHand_baseid;

    std::string shirt, shirt_baseid;
    json helmet_keywords = json::array();
    json armor_keywords = json::array();
    json boots_keywords = json::array();
    json gloves_keywords = json::array();
    json amulet_keywords = json::array();
    json ring_keywords = json::array();
    json cape_keywords = json::array();
    json backpack_keywords = json::array();
    json leftHand_keywords = json::array();
    json rightHand_keywords = json::array();
    json shirt_keywords = json::array();
    std::unordered_map<std::string, EquipmentSlotItem> moddedEquipment;
    // Declare these outside try block so they can be used after
    std::string equipment;
    std::string equipmentHash;

    // We loop through inventory to build inventory string using extradata lists for custom names
    std::string inventoryData;
    std::string npcName;
    std::vector<std::string> inventoryItems;  // For sorted hash

    // Wrap entire equipment reading in try-catch to handle race conditions during cell transitions
    try {
        auto inventory = npc->GetInventory();

        npcName.assign(npc->GetDisplayFullName());

        for (const auto& item : inventory) {
            RE::TESBoundObject* boundObject = item.first;
            auto count = item.second.first;
            const std::unique_ptr<RE::InventoryEntryData>& entryData = item.second.second;

            if (!boundObject || count == 0) {
                continue;
            }

            // Get item name
            std::string itemName;
            if (auto baseName = boundObject->GetName(); baseName && baseName[0]) {
                itemName = baseName;
            }

            bool flagWorn = false;
            // Check worn equipment slots - these can crash if actor becomes invalid
            if (npc->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kRing) == boundObject) {
                flagWorn = true;
            } else if (IsWornHeadgear(npc, boundObject)) {
                flagWorn = true;
            } else if (npc->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kAmulet) == boundObject) {
                flagWorn = true;
            } else if (npc->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody) == boundObject) {
                flagWorn = true;
            } else if (npc->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kHands) == boundObject) {
                flagWorn = true;
            } else if (npc->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kFeet) == boundObject) {
                flagWorn = true;
            } else if (npc->GetEquippedObject(false) == boundObject) {
                flagWorn = true;
            } else if (npc->GetEquippedObject(true) == boundObject) {
                flagWorn = true;
            } else {
                // Check if it's a worn armor in modded biped slots.
                if (auto* armorItem = boundObject->As<RE::TESObjectARMO>()) {
                    if (HasAnyModdedEquipmentSlot(armorItem) && HasWornExtra(entryData)) {
                        flagWorn = true;
                    }
                }
            }

            if (!flagWorn) {
                continue;
            }

            // Get baseid (FormID in hex format)
            std::string baseID = std::format("{:08X}", boundObject->GetFormID());
            json itemKeywords = CollectItemKeywords(boundObject);

            //  Check for custom name in InventoryEntryData and ExtraDataList
            if (entryData) {
                if (entryData->GetDisplayName()) {
                    itemName.assign(entryData->GetDisplayName());
                }

                if (auto* xlist = entryData->extraLists; xlist) {
                    for (auto extraList : *xlist) {
                        if (!extraList) {
                            continue;
                        }

                        if (auto textData = extraList->GetByType<RE::ExtraTextDisplayData>(); textData) {
                            if (!textData->displayName.empty()) {
                                itemName.assign(textData->displayName);
                                // logger::info("[INVENTORY_CUSTOM_NAME] Inventory item ref full name: {}", itemName);
                                break;
                            }
                        }
                    }
                }
            }

            if (auto* armorItem = boundObject->As<RE::TESObjectARMO>()) {
                AddModdedEquipmentSlots(armorItem, itemName, baseID, itemKeywords, moddedEquipment);
            }

            if (npc->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kRing) == boundObject) {
                ring.assign(itemName);
                ring_baseid.assign(baseID);
                ring_keywords = itemKeywords;
            } else if (IsWornHeadgear(npc, boundObject)) {
                helmet.assign(itemName);
                helmet_baseid.assign(baseID);
                helmet_keywords = itemKeywords;
            } else if (npc->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kAmulet) == boundObject) {
                amulet.assign(itemName);
                amulet_baseid.assign(baseID);
                amulet_keywords = itemKeywords;
            } else if (npc->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody) == boundObject) {
                armor.assign(itemName);
                armor_baseid.assign(baseID);
                armor_keywords = itemKeywords;
            } else if (npc->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kHands) == boundObject) {
                gloves.assign(itemName);
                gloves_baseid.assign(baseID);
                gloves_keywords = itemKeywords;
            } else if (npc->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kFeet) == boundObject) {
                boots.assign(itemName);
                boots_baseid.assign(baseID);
                boots_keywords = itemKeywords;
            } else if (npc->GetEquippedObject(false) == boundObject) {
                rightHand.assign(itemName);
                rightHand_baseid.assign(baseID);
                rightHand_keywords = itemKeywords;
            } else if (npc->GetEquippedObject(true) == boundObject) {
                leftHand.assign(itemName);
                leftHand_baseid.assign(baseID);
                leftHand_keywords = itemKeywords;
            } else {
                // Check if it's cape or backpack
                if (auto* armorItem = boundObject->As<RE::TESObjectARMO>()) {
                    if (HasArmorSlot(armorItem, RE::BGSBipedObjectForm::BipedObjectSlot::kModChestPrimary)) {
                        cape.assign(itemName);
                        cape_baseid.assign(baseID);
                        cape_keywords = itemKeywords;
                    }
                    if (HasArmorSlot(armorItem, RE::BGSBipedObjectForm::BipedObjectSlot::kModBack)) {
                        backpack.assign(itemName);
                        backpack_baseid.assign(baseID);
                        backpack_keywords = itemKeywords;
                    }
                    if (HasArmorSlot(armorItem, RE::BGSBipedObjectForm::BipedObjectSlot::kModChestSecondary)) {
                        shirt.assign(itemName);
                        shirt_baseid.assign(baseID);
                        shirt_keywords = itemKeywords;
                    }
                }
            }
        }

        ApplyLegacyModdedEquipmentSlot(moddedEquipment, "cape", cape, cape_baseid, cape_keywords);
        ApplyLegacyModdedEquipmentSlot(moddedEquipment, "backpack", backpack, backpack_baseid, backpack_keywords);
        ApplyLegacyModdedEquipmentSlot(moddedEquipment, "shirt", shirt, shirt_baseid, shirt_keywords);

        // Build equipment string with baseids (format: name^baseid)
        equipment.append(!helmet.empty() ? helmet + "^" + helmet_baseid : "").append("@");
        equipment.append(!armor.empty() ? armor + "^" + armor_baseid : "").append("@");
        equipment.append(!boots.empty() ? boots + "^" + boots_baseid : "").append("@");
        equipment.append(!gloves.empty() ? gloves + "^" + gloves_baseid : "").append("@");
        equipment.append(!amulet.empty() ? amulet + "^" + amulet_baseid : "").append("@");
        equipment.append(!ring.empty() ? ring + "^" + ring_baseid : "").append("@");
        equipment.append(!cape.empty() ? cape + "^" + cape_baseid : "").append("@");
        equipment.append(!backpack.empty() ? backpack + "^" + backpack_baseid : "").append("@");
        equipment.append(!leftHand.empty() ? leftHand + "^" + leftHand_baseid : "").append("@");
        equipment.append(!rightHand.empty() ? rightHand + "^" + rightHand_baseid : "").append("@");

        equipment.append(!shirt.empty() ? shirt + "^" + shirt_baseid : "");

        // Create hash for comparison (including baseids to detect changes)
        equipmentHash = std::format(
            "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", !helmet.empty() ? helmet : "",
            !helmet.empty() ? helmet_baseid : "", !armor.empty() ? armor : "", !armor.empty() ? armor_baseid : "",
            !boots.empty() ? boots : "", !boots.empty() ? boots_baseid : "", !gloves.empty() ? gloves : "",
            !gloves.empty() ? gloves_baseid : "", !amulet.empty() ? amulet : "", !amulet.empty() ? amulet_baseid : "",
            !ring.empty() ? ring : "", !ring.empty() ? ring_baseid : "", !cape.empty() ? cape : "",
            !cape.empty() ? cape_baseid : "", !backpack.empty() ? backpack : "",
            !backpack.empty() ? backpack_baseid : "", !leftHand.empty() ? leftHand : "",
            !leftHand.empty() ? leftHand_baseid : "", !rightHand.empty() ? rightHand : "",
            !rightHand.empty() ? rightHand_baseid : "", !shirt.empty() ? shirt : "",
            !shirt.empty() ? shirt_baseid : "");
        equipmentHash.append("|keywords=")
            .append(helmet_keywords.dump())
            .append("|")
            .append(armor_keywords.dump())
            .append("|")
            .append(boots_keywords.dump())
            .append("|")
            .append(gloves_keywords.dump())
            .append("|")
            .append(amulet_keywords.dump())
            .append("|")
            .append(ring_keywords.dump())
            .append("|")
            .append(cape_keywords.dump())
            .append("|")
            .append(backpack_keywords.dump())
            .append("|")
            .append(leftHand_keywords.dump())
            .append("|")
            .append(rightHand_keywords.dump())
            .append("|")
            .append(shirt_keywords.dump());
        equipmentHash.append(BuildModdedEquipmentHash(moddedEquipment));

        logger::debug("[EQUIPMENT {}", equipmentHash);

    } catch (...) {
        // Actor became invalid during equipment reading (race condition during cell transition)
        logger::trace("[EQUIPMENT_SKIP] {} - caught exception during equipment read (actor invalidated)", agentName);
        return;
    }

    auto formID = npc->GetFormID();

    // Check if equipment changed (or force update on save load)
    if (!forceUpdate && lastEquipmentHash.find(formID) != lastEquipmentHash.end()) {
        if (lastEquipmentHash[formID] == equipmentHash) {
            logger::trace("[EQUIPMENT_SKIP] {} equipment unchanged", agentName);
            return;
        }
    }

    // Equipment changed or first time tracking - send update
    lastEquipmentHash[formID] = equipmentHash;

    // Build JSON data for equipment update
    json equipmentData;
    equipmentData["type"] = "equipment";
    equipmentData["actor_name"] = agentName;
    equipmentData["actor_type"] = "npc";
    equipmentData["timestamp"] = getCurrentTimeMillis();
    equipmentData["gamets"] = GetGameTimeStamp();
    equipmentData["equipment"] = {
        {"helmet", BuildEquipmentItemJson(helmet, helmet_baseid, helmet_keywords)},
        {"armor", BuildEquipmentItemJson(armor, armor_baseid, armor_keywords)},
        {"boots", BuildEquipmentItemJson(boots, boots_baseid, boots_keywords)},
        {"gloves", BuildEquipmentItemJson(gloves, gloves_baseid, gloves_keywords)},
        {"amulet", BuildEquipmentItemJson(amulet, amulet_baseid, amulet_keywords)},
        {"ring", BuildEquipmentItemJson(ring, ring_baseid, ring_keywords)},
        {"cape", BuildEquipmentItemJson(cape, cape_baseid, cape_keywords)},
        {"backpack", BuildEquipmentItemJson(backpack, backpack_baseid, backpack_keywords)},
        {"left_hand", BuildEquipmentItemJson(leftHand, leftHand_baseid, leftHand_keywords)},
        {"right_hand", BuildEquipmentItemJson(rightHand, rightHand_baseid, rightHand_keywords)}};
    AddModdedEquipmentToJson(equipmentData["equipment"], moddedEquipment);

    HTTPManager::postGameData("gamedata.php", equipmentData);

    logger::info("[EQUIPMENT_UPDATE] {} equipment updated", agentName);
}

void RefreshAIAgentEquipment(RE::Actor* npc, const std::string& agentName, bool forceUpdate) {
    if (!npc) return;

    auto actorHandle = npc->GetHandle();
    auto* taskInterface = SKSE::GetTaskInterface();
    if (!taskInterface) {
        logger::warn("[EQUIPMENT_UPDATE] SKSE task interface unavailable; reading {} equipment directly", agentName);
        RefreshAIAgentEquipmentImpl(npc, agentName, forceUpdate);
        return;
    }

    taskInterface->AddTask([actorHandle, agentName, forceUpdate]() {
        auto actorRef = actorHandle.get();
        auto* actor = actorRef.get() ? actorRef.get()->As<RE::Actor>() : nullptr;
        if (!actor) {
            logger::trace("[EQUIPMENT_SKIP] {} - actor handle no longer valid", agentName);
            return;
        }

        RefreshAIAgentEquipmentImpl(actor, agentName, forceUpdate);
    });
}


// Helper function to refresh inventory for an AI Agent (with hash-based diffing)
void RefreshAIAgentInventoryImpl(RE::Actor* npc, const std::string& agentName, bool forceUpdate, bool synchronous) {
    if (!npc) return;
    if (npc->IsPlayer()) return;  // Skip player

    std::string inventoryData;
    std::vector<InventoryItemSnapshot> inventoryItems;  // For sorted hash
    auto inventory = npc->GetInventory();

    for (const auto& item : inventory) {
        RE::TESBoundObject* boundObject = item.first;
        auto count = item.second.first;
        const std::unique_ptr<RE::InventoryEntryData>& entryData = item.second.second;

        if (!boundObject || count == 0) {
            continue;
        }

        // Base name from the bound object
        std::string itemName;
        if (auto baseName = boundObject->GetName(); baseName && baseName[0]) {
            itemName = baseName;
        }

        // Skip non-inventory items
        if (!boundObject->IsInventoryObject() || boundObject->IsIgnored() || !boundObject->GetPlayable()) {
            continue;
        }

        // Get baseid (FormID in hex format)
        std::string baseID = std::format("{:08X}", boundObject->GetFormID());
        json itemKeywords = CollectItemKeywords(boundObject);

        //  Check for custom name in InventoryEntryData and ExtraDataList
        if (entryData) {
            if (auto display = entryData->GetDisplayName(); display && display[0]) {
                itemName = display;
            }

            if (auto* xlist = entryData->extraLists; xlist) {
                for (auto extraList : *xlist) {
                    if (!extraList) {
                        continue;
                    }

                    if (auto textData = extraList->GetByType<RE::ExtraTextDisplayData>(); textData) {
                        if (!textData->displayName.empty()) {
                            itemName.assign(textData->displayName);
                            logger::info("[INVENTORY_CUSTOM_NAME] Inventory item ref full name: {}", itemName);
                            break;
                        }
                    }
                }
            }
        }

        //

        // Skip items with missing or invalid names
        if (!itemName.empty() && itemName != "<Missing Name>") {
            std::string itemEntry = std::format("{}^{}::{}", itemName, baseID, count);
            inventoryItems.push_back(
                {itemName, baseID, count, itemKeywords, itemEntry + "^" + itemKeywords.dump(), boundObject->GetGoldValue()});

            if (!inventoryData.empty()) {
                inventoryData.append("~");
            }
            inventoryData.append(itemEntry);
        }
    }

    // Create hash by sorting items (order-independent comparison)
    std::sort(inventoryItems.begin(), inventoryItems.end(),
              [](const InventoryItemSnapshot& lhs, const InventoryItemSnapshot& rhs) {
                  return lhs.hashEntry < rhs.hashEntry;
              });
    std::string inventoryHash;
    for (const auto& item : inventoryItems) {
        if (!inventoryHash.empty()) inventoryHash.append("|");
        inventoryHash.append(item.hashEntry);
    }

    auto formID = npc->GetFormID();

    // Check if inventory changed (or force update on save load)
    if (!forceUpdate && lastInventoryHash.find(formID) != lastInventoryHash.end()) {
        if (lastInventoryHash[formID] == inventoryHash) {
            logger::trace("[INVENTORY_SKIP] {} inventory unchanged", agentName);
            return;
        }
    }

    // Inventory changed or first time tracking - send update
    lastInventoryHash[formID] = inventoryHash;

    json inventoryDataJson;
    inventoryDataJson["type"] = "inventory";
    inventoryDataJson["actor_name"] = agentName;
    inventoryDataJson["actor_type"] = "npc";
    inventoryDataJson["timestamp"] = getCurrentTimeMillis();
    inventoryDataJson["gamets"] = GetGameTimeStamp();
    inventoryDataJson["items"] = json::array();

    for (const auto& item : inventoryItems) {
        inventoryDataJson["items"].push_back({{"name", item.name},
                                              {"baseid", item.baseid},
                                              {"count", item.count},
                                              {"keywords", item.keywords.is_array() ? item.keywords : json::array()}, 
                                              {"goldvalue", item.gold}
            });
    }

    if (synchronous) {
        HTTPManager::postGameDataSync("gamedata.php", inventoryDataJson);
    } else {
        HTTPManager::postGameData("gamedata.php", inventoryDataJson);
    }

    logger::info("[INVENTORY_UPDATE] {} inventory updated ({} items{})", agentName, inventoryItems.size(),
                 synchronous ? ", sync" : "");
}


void RefreshAIAgentInventory(RE::Actor* npc, const std::string& agentName, bool forceUpdate, bool synchronous) {
    if (!npc) return;

    auto actorHandle = npc->GetHandle();
    auto* taskInterface = SKSE::GetTaskInterface();
    if (!taskInterface) {
        logger::warn("[INVENTORY_UPDATE] SKSE task interface unavailable; reading {} inventory directly", agentName);
        RefreshAIAgentInventoryImpl(npc, agentName, forceUpdate, synchronous);
        return;
    }

    taskInterface->AddTask([actorHandle, agentName, forceUpdate, synchronous]() {
        auto actorRef = actorHandle.get();
        auto* actor = actorRef.get() ? actorRef.get()->As<RE::Actor>() : nullptr;
        if (!actor) {
            logger::trace("[INVENTORY_SKIP] {} - actor handle no longer valid", agentName);
            return;
        }

        RefreshAIAgentInventoryImpl(actor, agentName, forceUpdate, synchronous);
    });
}


// Helper function to refresh skills for an AI Agent (with hash-based diffing)
void RefreshAIAgentSkills(RE::Actor* npc, const std::string& agentName, bool forceUpdate) {
    // Enhanced null and validity checks to prevent crashes
    if (!npc) {
        logger::error("[SKILLS_UPDATE] Null actor pointer for {}", agentName);
        return;
    }
    
    if (!npc->GetActorRuntimeData().currentProcess) {
        logger::warn("[SKILLS_UPDATE] Actor {} has no current process", agentName);
        return;
    }
    
    if (npc->IsDisabled() || npc->IsDeleted() || npc->IsDead()) {
        logger::debug("[SKILLS_UPDATE] Actor {} is disabled/deleted/dead, skipping", agentName);
        return;
    }
    
    auto stats = npc->AsActorValueOwner();
    if (!stats) {
        logger::warn("[SKILLS_UPDATE] Actor {} has no ActorValueOwner", agentName);
        return;
    }
    
    // Get all 18 Skyrim skills
    float archery = stats->GetActorValue(RE::ActorValue::kArchery);
    float block = stats->GetActorValue(RE::ActorValue::kBlock);
    float onehanded = stats->GetActorValue(RE::ActorValue::kOneHanded);
    float twohanded = stats->GetActorValue(RE::ActorValue::kTwoHanded);
    float conjuration = stats->GetActorValue(RE::ActorValue::kConjuration);
    float destruction = stats->GetActorValue(RE::ActorValue::kDestruction);
    float illusion = stats->GetActorValue(RE::ActorValue::kIllusion);
    float restoration = stats->GetActorValue(RE::ActorValue::kRestoration);
    float alteration = stats->GetActorValue(RE::ActorValue::kAlteration);
    float enchanting = stats->GetActorValue(RE::ActorValue::kEnchanting);
    float smithing = stats->GetActorValue(RE::ActorValue::kSmithing);
    float heavyarmor = stats->GetActorValue(RE::ActorValue::kHeavyArmor);
    float lightarmor = stats->GetActorValue(RE::ActorValue::kLightArmor);
    float pickpocket = stats->GetActorValue(RE::ActorValue::kPickpocket);
    float lockpicking = stats->GetActorValue(RE::ActorValue::kLockpicking);
    float sneak = stats->GetActorValue(RE::ActorValue::kSneak);
    float alchemy = stats->GetActorValue(RE::ActorValue::kAlchemy);
    float speechcraft = stats->GetActorValue(RE::ActorValue::kSpeech);
    
    // Create hash for comparison
    std::string skillsHash = std::format("{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}",
        archery, block, onehanded, twohanded, conjuration, destruction, illusion, restoration,
        alteration, enchanting, smithing, heavyarmor, lightarmor, pickpocket, lockpicking, sneak, alchemy, speechcraft);
    
    auto formID = npc->GetFormID();
    
    // Check if skills changed (or force update)
    if (!forceUpdate && lastSkillsHash.find(formID) != lastSkillsHash.end()) {
        if (lastSkillsHash[formID] == skillsHash) {
            logger::trace("[SKILLS_SKIP] {} skills unchanged", agentName);
            return;
        }
    }
    
    // Skills changed - send update
    lastSkillsHash[formID] = skillsHash;
    
    // Build JSON data for skills update
    json skillsDataJson;
    skillsDataJson["type"] = "skills";
    skillsDataJson["actor_name"] = agentName;
    skillsDataJson["actor_type"] = "npc";
    skillsDataJson["timestamp"] = getCurrentTimeMillis();
    skillsDataJson["gamets"] = GetGameTimeStamp();
    skillsDataJson["skills"] = {
        {"archery", archery},
        {"block", block},
        {"onehanded", onehanded},
        {"twohanded", twohanded},
        {"conjuration", conjuration},
        {"destruction", destruction},
        {"illusion", illusion},
        {"restoration", restoration},
        {"alteration", alteration},
        {"enchanting", enchanting},
        {"smithing", smithing},
        {"heavyarmor", heavyarmor},
        {"lightarmor", lightarmor},
        {"pickpocket", pickpocket},
        {"lockpicking", lockpicking},
        {"sneak", sneak},
        {"alchemy", alchemy},
        {"speechcraft", speechcraft}
    };
    
    HTTPManager::postGameData("gamedata.php", skillsDataJson);
    
    logger::info("[SKILLS_UPDATE] {} skills changed, sent update", agentName);
}

// Helper function to refresh stats for an AI Agent (with hash-based diffing)
void RefreshAIAgentStats(RE::Actor* npc, const std::string& agentName, bool forceUpdate) {
    logger::info("[STATS_FUNCTION_ENTRY] RefreshAIAgentStats called for: {}", agentName);
    
    // Enhanced null and validity checks to prevent crashes
    if (!npc) {
        logger::error("[STATS_UPDATE] Null actor pointer for {}", agentName);
        return;
    }
    
    if (!npc->GetActorRuntimeData().currentProcess) {
        logger::warn("[STATS_UPDATE] Actor {} has no current process", agentName);
        return;
    }
    
    if (npc->IsDisabled() || npc->IsDeleted()) {
        logger::debug("[STATS_UPDATE] Actor {} is disabled/deleted, skipping", agentName);
        return;
    }
    
    // Skip dead NPCs
    if (npc->IsDead()) {
        logger::trace("[STATS_SKIP] {} is dead, skipping stats", agentName);
        return;
    }
    
    auto stats = npc->AsActorValueOwner();
    if (!stats) {
        logger::warn("[STATS_UPDATE] Actor {} has no ActorValueOwner", agentName);
        return;
    }
    
    // Get core stats (level + health/magicka/stamina current and max)
    auto level = npc->GetLevel();
    float health = stats->GetActorValue(RE::ActorValue::kHealth);
    float healthMax = stats->GetBaseActorValue(RE::ActorValue::kHealth);
    float magicka = stats->GetActorValue(RE::ActorValue::kMagicka);
    float magickaMax = stats->GetBaseActorValue(RE::ActorValue::kMagicka);
    float stamina = stats->GetActorValue(RE::ActorValue::kStamina);
    float staminaMax = stats->GetBaseActorValue(RE::ActorValue::kStamina);
    
    // Get actor scale
    float scale = npc->GetScale();
    
    // Safety: Clamp negative values to 0 (shouldn't happen but protect against engine bugs)
    health = std::max(0.0f, health);
    magicka = std::max(0.0f, magicka);
    stamina = std::max(0.0f, stamina);
    healthMax = std::max(1.0f, healthMax);  // Prevent division by zero
    magickaMax = std::max(1.0f, magickaMax);
    staminaMax = std::max(1.0f, staminaMax);
    
    // Clamp current to max (can't exceed)
    health = std::min(health, healthMax);
    magicka = std::min(magicka, magickaMax);
    stamina = std::min(stamina, staminaMax);
    
   

    // Create hash for comparison (round to nearest 1 to avoid float precision spam, scale to 2 decimals)
    std::string statsHash = std::format("{}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.2f}", level, health,
                                        healthMax, magicka, magickaMax, stamina, staminaMax, scale);

    auto formID = npc->GetFormID();
    
    // Check if stats changed (or force update)
    if (!forceUpdate && lastStatsHash.find(formID) != lastStatsHash.end()) {
        if (lastStatsHash[formID] == statsHash) {
            logger::trace("[STATS_SKIP] {} stats unchanged", agentName);
            return;
        }
    }
    
    // Stats changed - send update
    lastStatsHash[formID] = statsHash;
    

    json statsDataJson;
    statsDataJson["type"] = "stats";
    statsDataJson["actor_name"] = agentName;
    statsDataJson["actor_type"] = "npc";
    statsDataJson["timestamp"] = getCurrentTimeMillis();
    statsDataJson["gamets"] = GetGameTimeStamp();
    statsDataJson["stats"] = {
        {"level", level},
        {"health", health},
        {"health_max", healthMax},
        {"magicka", magicka},
        {"magicka_max", magickaMax},
        {"stamina", stamina},
        {"stamina_max", staminaMax},
        {"scale", scale}
    };
    
    HTTPManager::postGameData("gamedata.php", statsDataJson);
    
    // Calculate percentages for logging
    int hpPercent = (int)((health / healthMax) * 100);
    int mpPercent = (int)((magicka / magickaMax) * 100);
    int spPercent = (int)((stamina / staminaMax) * 100);
    
    logger::info("[STATS_UPDATE] {} stats changed: Lv:{} HP:{:.0f}/{:.0f} ({}%), MP:{:.0f}/{:.0f} ({}%), SP:{:.0f}/{:.0f} ({}%)", 
                agentName, level, health, healthMax, hpPercent, magicka, magickaMax, mpPercent, stamina, staminaMax, spPercent);
}

void RefreshAIAgentActivityStatus(RE::Actor* npc, const std::string& agentName, const std::string* furnitureOverride)
{
    logger::trace("[ACTIVITY_STATUS] Refresh requested for {}", agentName);

    if (!npc) {
        logger::error("[ACTIVITY_STATUS] Null actor pointer for {}", agentName);
        return;
    }

    if (!npc->GetActorRuntimeData().currentProcess) {
        logger::warn("[ACTIVITY_STATUS] Actor {} has no current process", agentName);
        return;
    }

    if (npc->IsDisabled() || npc->IsDeleted()) {
        logger::debug("[ACTIVITY_STATUS] Actor {} is disabled/deleted, skipping", agentName);
        return;
    }

    json statusPayload = BuildActivityStatusPayload(npc, agentName, furnitureOverride);
    if (statusPayload.empty()) {
        return;
    }

    HTTPManager::postGameData("gamedata.php", statusPayload);
    RefreshAIAgentTransformationState(npc, agentName);
}

void RefreshAIAgentTransformationState(RE::Actor* npc, const std::string& agentName)
{
    json transformationPayload = BuildTransformationStatePayload(npc, agentName, "npc");
    if (transformationPayload.empty()) {
        return;
    }

    HTTPManager::postGameData("gamedata.php", transformationPayload);
}

void RefreshAIAgentFurniture(RE::Actor* npc, const std::string& agentName, std::string furniture) {
    RefreshAIAgentActivityStatus(npc, agentName, &furniture);
}

void PostNearbyActivityStatus(RE::PlayerCharacter* player, float radius)
{
    if (!player) {
        return;
    }

    json batchPayload;
    batchPayload["type"] = "activity_status_bulk";
    batchPayload["timestamp"] = getCurrentTimeMillis();
    batchPayload["gamets"] = GetGameTimeStamp();
    batchPayload["statuses"] = json::array();
    json transformationBatchPayload;
    transformationBatchPayload["type"] = "transformation_state_bulk";
    transformationBatchPayload["timestamp"] = getCurrentTimeMillis();
    transformationBatchPayload["gamets"] = GetGameTimeStamp();
    transformationBatchPayload["states"] = json::array();

    AIAgentManager& aiam = AIAgentManager::getInstance();
    for (const auto& agent : aiam.getAgents()) {
        if (!agent) {
            continue;
        }

        auto* actor = agent->getActor();
        if (!actor) {
            actor = agent->getActorByFormId();
        }
        if (!actor || actor->GetFormID() == player->GetFormID()) {
            continue;
        }
        if (actor->IsDeleted() || actor->IsDisabled()) {
            continue;
        }

        const float distance = player->GetPosition().GetDistance(actor->GetPosition());
        if (distance > radius) {
            continue;
        }

        std::string actorName = trim(agent->getActorName());
        if (actorName.empty()) {
            actorName = trim(actor->GetDisplayFullName());
        }
        if (actorName.empty()) {
            continue;
        }

        json statusPayload = BuildActivityStatusPayload(actor, actorName);
        if (!statusPayload.empty()) {
            batchPayload["statuses"].push_back(statusPayload);
        }

        json transformationPayload = BuildTransformationStatePayload(actor, actorName, "npc");
        if (!transformationPayload.empty()) {
            transformationBatchPayload["states"].push_back(transformationPayload);
        }
    }

    if (!batchPayload["statuses"].empty()) {
        HTTPManager::postGameData("gamedata.php", batchPayload);
    }
    if (!transformationBatchPayload["states"].empty()) {
        HTTPManager::postGameData("gamedata.php", transformationBatchPayload);
    }
}

// Player tracking functions - send player data to server
void RefreshPlayerEquipment(bool forceUpdate) {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    
    std::string helmet, helmet_baseid;
    std::string armor, armor_baseid;
    std::string boots, boots_baseid;
    std::string gloves, gloves_baseid;
    std::string amulet, amulet_baseid;
    std::string ring, ring_baseid;
    std::string cape, cape_baseid;
    std::string backpack, backpack_baseid;
    std::string leftHand, leftHand_baseid;
    std::string rightHand, rightHand_baseid;
    std::string shirt, shirt_baseid;
    json helmet_keywords = json::array();
    json armor_keywords = json::array();
    json boots_keywords = json::array();
    json gloves_keywords = json::array();
    json amulet_keywords = json::array();
    json ring_keywords = json::array();
    json cape_keywords = json::array();
    json backpack_keywords = json::array();
    json leftHand_keywords = json::array();
    json rightHand_keywords = json::array();
    json shirt_keywords = json::array();
    std::unordered_map<std::string, EquipmentSlotItem> moddedEquipment;
    
    std::string equipmentHash;
    
    try {
        auto inventory = player->GetInventory();
        
        for (const auto& item : inventory) {
            RE::TESBoundObject* boundObject = item.first;
            auto count = item.second.first;
            const std::unique_ptr<RE::InventoryEntryData>& entryData = item.second.second;
            
            if (!boundObject || count == 0) continue;
            
            std::string itemName;
            if (auto baseName = boundObject->GetName(); baseName && baseName[0]) {
                itemName = baseName;
            }
            
            bool flagWorn = false;
            if (player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kRing) == boundObject) {
                flagWorn = true;
            } else if (IsWornHeadgear(player, boundObject)) {
                flagWorn = true;
            } else if (player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kAmulet) == boundObject) {
                flagWorn = true;
            } else if (player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody) == boundObject) {
                flagWorn = true;
            } else if (player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kHands) == boundObject) {
                flagWorn = true;
            } else if (player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kFeet) == boundObject) {
                flagWorn = true;
            } else if (player->GetEquippedObject(false) == boundObject) {
                flagWorn = true;
            } else if (player->GetEquippedObject(true) == boundObject) {
                flagWorn = true;
            } else {
                if (auto* armorItem = boundObject->As<RE::TESObjectARMO>()) {
                    if (HasAnyModdedEquipmentSlot(armorItem) && HasWornExtra(entryData)) {
                        flagWorn = true;
                    }
                }
            }
            
            if (!flagWorn) continue;
            
            std::string baseID = std::format("{:08X}", boundObject->GetFormID());
            json itemKeywords = CollectItemKeywords(boundObject);
            
            if (entryData) {
                if (entryData->GetDisplayName()) {
                    itemName.assign(entryData->GetDisplayName());
                }
                if (auto* xlist = entryData->extraLists; xlist) {
                    for (auto extraList : *xlist) {
                        if (!extraList) continue;
                        if (auto textData = extraList->GetByType<RE::ExtraTextDisplayData>(); textData) {
                            if (!textData->displayName.empty()) {
                                itemName.assign(textData->displayName);
                                break;
                            }
                        }
                    }
                }
            }

            if (auto* armorItem = boundObject->As<RE::TESObjectARMO>()) {
                AddModdedEquipmentSlots(armorItem, itemName, baseID, itemKeywords, moddedEquipment);
            }
            
            if (player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kRing) == boundObject) {
                ring.assign(itemName);
                ring_baseid.assign(baseID);
                ring_keywords = itemKeywords;
            } else if (IsWornHeadgear(player, boundObject)) {
                helmet.assign(itemName);
                helmet_baseid.assign(baseID);
                helmet_keywords = itemKeywords;
            } else if (player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kAmulet) == boundObject) {
                amulet.assign(itemName);
                amulet_baseid.assign(baseID);
                amulet_keywords = itemKeywords;
            } else if (player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody) == boundObject) {
                armor.assign(itemName);
                armor_baseid.assign(baseID);
                armor_keywords = itemKeywords;
            } else if (player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kHands) == boundObject) {
                gloves.assign(itemName);
                gloves_baseid.assign(baseID);
                gloves_keywords = itemKeywords;
            } else if (player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kFeet) == boundObject) {
                boots.assign(itemName);
                boots_baseid.assign(baseID);
                boots_keywords = itemKeywords;
            } else if (player->GetEquippedObject(false) == boundObject) {
                rightHand.assign(itemName);
                rightHand_baseid.assign(baseID);
                rightHand_keywords = itemKeywords;
            } else if (player->GetEquippedObject(true) == boundObject) {
                leftHand.assign(itemName);
                leftHand_baseid.assign(baseID);
                leftHand_keywords = itemKeywords;
            } else {
                if (auto* armorItem = boundObject->As<RE::TESObjectARMO>()) {
                    if (HasArmorSlot(armorItem, RE::BGSBipedObjectForm::BipedObjectSlot::kModChestPrimary)) {
                        cape.assign(itemName);
                        cape_baseid.assign(baseID);
                        cape_keywords = itemKeywords;
                    }
                    if (HasArmorSlot(armorItem, RE::BGSBipedObjectForm::BipedObjectSlot::kModBack)) {
                        backpack.assign(itemName);
                        backpack_baseid.assign(baseID);
                        backpack_keywords = itemKeywords;
                    }
                    if (HasArmorSlot(armorItem, RE::BGSBipedObjectForm::BipedObjectSlot::kModChestSecondary)) {
                        shirt.assign(itemName);
                        shirt_baseid.assign(baseID);
                        shirt_keywords = itemKeywords;
                    }
                }
            }
        }

        ApplyLegacyModdedEquipmentSlot(moddedEquipment, "cape", cape, cape_baseid, cape_keywords);
        ApplyLegacyModdedEquipmentSlot(moddedEquipment, "backpack", backpack, backpack_baseid, backpack_keywords);
        ApplyLegacyModdedEquipmentSlot(moddedEquipment, "shirt", shirt, shirt_baseid, shirt_keywords);
        
        equipmentHash = std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", 
            !helmet.empty() ? helmet : "",
            !helmet.empty() ? helmet_baseid : "",
            !armor.empty() ? armor : "",
            !armor.empty() ? armor_baseid : "",
            !boots.empty() ? boots : "",
            !boots.empty() ? boots_baseid : "",
            !gloves.empty() ? gloves : "",
            !gloves.empty() ? gloves_baseid : "",
            !amulet.empty() ? amulet : "",
            !amulet.empty() ? amulet_baseid : "",
            !ring.empty() ? ring : "",
            !ring.empty() ? ring_baseid : "",
            !cape.empty() ? cape : "",
            !cape.empty() ? cape_baseid : "",
            !backpack.empty() ? backpack : "",
            !backpack.empty() ? backpack_baseid : "",
            !leftHand.empty() ? leftHand : "",
            !leftHand.empty() ? leftHand_baseid : "",
            !rightHand.empty() ? rightHand : "",
            !rightHand.empty() ? rightHand_baseid : ""
        );
        equipmentHash.append("|keywords=")
            .append(helmet_keywords.dump()).append("|")
            .append(armor_keywords.dump()).append("|")
            .append(boots_keywords.dump()).append("|")
            .append(gloves_keywords.dump()).append("|")
            .append(amulet_keywords.dump()).append("|")
            .append(ring_keywords.dump()).append("|")
            .append(cape_keywords.dump()).append("|")
            .append(backpack_keywords.dump()).append("|")
            .append(leftHand_keywords.dump()).append("|")
            .append(rightHand_keywords.dump()).append("|")
            .append(shirt_keywords.dump());
        equipmentHash.append(BuildModdedEquipmentHash(moddedEquipment));
    } catch (...) {
        logger::trace("[PLAYER_EQUIPMENT_SKIP] Exception during equipment read");
        return;
    }
    
    auto formID = player->GetFormID();
    
    if (!forceUpdate && lastEquipmentHash.find(formID) != lastEquipmentHash.end()) {
        if (lastEquipmentHash[formID] == equipmentHash) {
            return;
        }
    }
    
    lastEquipmentHash[formID] = equipmentHash;
    
    json equipmentData;
    equipmentData["type"] = "equipment";
    equipmentData["actor_name"] = player->GetDisplayFullName();
    equipmentData["actor_type"] = "player";
    equipmentData["timestamp"] = getCurrentTimeMillis();
    equipmentData["gamets"] = GetGameTimeStamp();
    equipmentData["equipment"] = {
        {"helmet", BuildEquipmentItemJson(helmet, helmet_baseid, helmet_keywords)},
        {"armor", BuildEquipmentItemJson(armor, armor_baseid, armor_keywords)},
        {"boots", BuildEquipmentItemJson(boots, boots_baseid, boots_keywords)},
        {"gloves", BuildEquipmentItemJson(gloves, gloves_baseid, gloves_keywords)},
        {"amulet", BuildEquipmentItemJson(amulet, amulet_baseid, amulet_keywords)},
        {"ring", BuildEquipmentItemJson(ring, ring_baseid, ring_keywords)},
        {"cape", BuildEquipmentItemJson(cape, cape_baseid, cape_keywords)},
        {"backpack", BuildEquipmentItemJson(backpack, backpack_baseid, backpack_keywords)},
        {"left_hand", BuildEquipmentItemJson(leftHand, leftHand_baseid, leftHand_keywords)},
        {"right_hand", BuildEquipmentItemJson(rightHand, rightHand_baseid, rightHand_keywords)}
    };
    AddModdedEquipmentToJson(equipmentData["equipment"], moddedEquipment);
    
    HTTPManager::postGameData("gamedata.php", equipmentData);
    logger::info("[PLAYER_EQUIPMENT_UPDATE] Player equipment updated");
}

void RefreshPlayerInventory(bool forceUpdate) {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    
    std::vector<InventoryItemSnapshot> inventoryItems;
    auto inventory = player->GetInventory();
    
    for (const auto& item : inventory) {
        RE::TESBoundObject* boundObject = item.first;
        auto count = item.second.first;
        const std::unique_ptr<RE::InventoryEntryData>& entryData = item.second.second;
        
        if (!boundObject || count == 0) continue;
        
        std::string itemName;
        if (auto baseName = boundObject->GetName(); baseName && baseName[0]) {
            itemName = baseName;
        }
        
        if (!boundObject->IsInventoryObject() || boundObject->IsIgnored() || !boundObject->GetPlayable()) {
            continue;
        }
        
        std::string baseID = std::format("{:08X}", boundObject->GetFormID());
        json itemKeywords = CollectItemKeywords(boundObject);
        
        if (entryData) {
            if (auto display = entryData->GetDisplayName(); display && display[0]) {
                itemName = display;
            }
            if (auto* xlist = entryData->extraLists; xlist) {
                for (auto extraList : *xlist) {
                    if (!extraList) continue;
                    if (auto textData = extraList->GetByType<RE::ExtraTextDisplayData>(); textData) {
                        if (!textData->displayName.empty()) {
                            itemName.assign(textData->displayName);
                            break;
                        }
                    }
                }
            }
        }
        
        // Skip items with missing or invalid names
        if (!itemName.empty() && itemName != "<Missing Name>") {
            std::string itemEntry = std::format("{}^{}::{}", itemName, baseID, count);
            inventoryItems.push_back({itemName, baseID, count, itemKeywords, itemEntry + "^" + itemKeywords.dump()});
        }
    }
    
    std::sort(inventoryItems.begin(), inventoryItems.end(), [](const InventoryItemSnapshot& lhs, const InventoryItemSnapshot& rhs) {
        return lhs.hashEntry < rhs.hashEntry;
    });
    std::string inventoryHash;
    for (const auto& item : inventoryItems) {
        if (!inventoryHash.empty()) inventoryHash.append("|");
        inventoryHash.append(item.hashEntry);
    }
    
    auto formID = player->GetFormID();
    
    if (!forceUpdate && lastInventoryHash.find(formID) != lastInventoryHash.end()) {
        if (lastInventoryHash[formID] == inventoryHash) {
            return;
        }
    }
    
    lastInventoryHash[formID] = inventoryHash;
    
    if (!inventoryItems.empty()) {
        json inventoryDataJson;
        inventoryDataJson["type"] = "inventory";
        inventoryDataJson["actor_name"] = player->GetDisplayFullName();
        inventoryDataJson["actor_type"] = "player";
        inventoryDataJson["timestamp"] = getCurrentTimeMillis();
        inventoryDataJson["gamets"] = GetGameTimeStamp();
        inventoryDataJson["items"] = json::array();
        
        for (const auto& item : inventoryItems) {
            inventoryDataJson["items"].push_back({
                {"name", item.name},
                {"baseid", item.baseid},
                {"count", item.count},
                {"keywords", item.keywords.is_array() ? item.keywords : json::array()}
            });
        }
        
        HTTPManager::postGameData("gamedata.php", inventoryDataJson);
        logger::info("[PLAYER_INVENTORY_UPDATE] Player inventory updated ({} items)", inventoryItems.size());
    }
}

void RefreshPlayerSkills(bool forceUpdate) {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    
    auto stats = player->AsActorValueOwner();
    if (!stats) return;
    
    float archery = stats->GetActorValue(RE::ActorValue::kArchery);
    float block = stats->GetActorValue(RE::ActorValue::kBlock);
    float onehanded = stats->GetActorValue(RE::ActorValue::kOneHanded);
    float twohanded = stats->GetActorValue(RE::ActorValue::kTwoHanded);
    float conjuration = stats->GetActorValue(RE::ActorValue::kConjuration);
    float destruction = stats->GetActorValue(RE::ActorValue::kDestruction);
    float illusion = stats->GetActorValue(RE::ActorValue::kIllusion);
    float restoration = stats->GetActorValue(RE::ActorValue::kRestoration);
    float alteration = stats->GetActorValue(RE::ActorValue::kAlteration);
    float enchanting = stats->GetActorValue(RE::ActorValue::kEnchanting);
    float smithing = stats->GetActorValue(RE::ActorValue::kSmithing);
    float heavyarmor = stats->GetActorValue(RE::ActorValue::kHeavyArmor);
    float lightarmor = stats->GetActorValue(RE::ActorValue::kLightArmor);
    float pickpocket = stats->GetActorValue(RE::ActorValue::kPickpocket);
    float lockpicking = stats->GetActorValue(RE::ActorValue::kLockpicking);
    float sneak = stats->GetActorValue(RE::ActorValue::kSneak);
    float alchemy = stats->GetActorValue(RE::ActorValue::kAlchemy);
    float speechcraft = stats->GetActorValue(RE::ActorValue::kSpeech);
    
    std::string skillsHash = std::format("{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}",
        archery, block, onehanded, twohanded, conjuration, destruction, illusion, restoration,
        alteration, enchanting, smithing, heavyarmor, lightarmor, pickpocket, lockpicking, sneak, alchemy, speechcraft);
    
    auto formID = player->GetFormID();
    
    if (!forceUpdate && lastSkillsHash.find(formID) != lastSkillsHash.end()) {
        if (lastSkillsHash[formID] == skillsHash) {
            return;
        }
    }
    
    lastSkillsHash[formID] = skillsHash;
    
    json skillsDataJson;
    skillsDataJson["type"] = "skills";
    skillsDataJson["actor_name"] = player->GetDisplayFullName();
    skillsDataJson["actor_type"] = "player";
    skillsDataJson["timestamp"] = getCurrentTimeMillis();
    skillsDataJson["gamets"] = GetGameTimeStamp();
    skillsDataJson["skills"] = {
        {"archery", archery},
        {"block", block},
        {"onehanded", onehanded},
        {"twohanded", twohanded},
        {"conjuration", conjuration},
        {"destruction", destruction},
        {"illusion", illusion},
        {"restoration", restoration},
        {"alteration", alteration},
        {"enchanting", enchanting},
        {"smithing", smithing},
        {"heavyarmor", heavyarmor},
        {"lightarmor", lightarmor},
        {"pickpocket", pickpocket},
        {"lockpicking", lockpicking},
        {"sneak", sneak},
        {"alchemy", alchemy},
        {"speechcraft", speechcraft}
    };
    
    HTTPManager::postGameData("gamedata.php", skillsDataJson);
    logger::info("[PLAYER_SKILLS_UPDATE] Player skills updated");
}

void RefreshPlayerStats(bool forceUpdate) {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    
    auto stats = player->AsActorValueOwner();
    if (!stats) return;
    
    auto level = player->GetLevel();
    float health = stats->GetActorValue(RE::ActorValue::kHealth);
    float healthMax = stats->GetBaseActorValue(RE::ActorValue::kHealth);
    float magicka = stats->GetActorValue(RE::ActorValue::kMagicka);
    float magickaMax = stats->GetBaseActorValue(RE::ActorValue::kMagicka);
    float stamina = stats->GetActorValue(RE::ActorValue::kStamina);
    float staminaMax = stats->GetBaseActorValue(RE::ActorValue::kStamina);
    
    // Get player scale
    float scale = player->GetScale();
    
    health = std::max(0.0f, health);
    magicka = std::max(0.0f, magicka);
    stamina = std::max(0.0f, stamina);
    healthMax = std::max(1.0f, healthMax);
    magickaMax = std::max(1.0f, magickaMax);
    staminaMax = std::max(1.0f, staminaMax);
    
    health = std::min(health, healthMax);
    magicka = std::min(magicka, magickaMax);
    stamina = std::min(stamina, staminaMax);
    
    std::string statsHash = std::format("{}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.0f}|{:.2f}",
        level, health, healthMax, magicka, magickaMax, stamina, staminaMax, scale);
    
    auto formID = player->GetFormID();
    
    if (!forceUpdate && lastStatsHash.find(formID) != lastStatsHash.end()) {
        if (lastStatsHash[formID] == statsHash) {
            return;
        }
    }
    
    lastStatsHash[formID] = statsHash;
    
    json statsDataJson;
    statsDataJson["type"] = "stats";
    statsDataJson["actor_name"] = player->GetDisplayFullName();
    statsDataJson["actor_type"] = "player";
    statsDataJson["timestamp"] = getCurrentTimeMillis();
    statsDataJson["gamets"] = GetGameTimeStamp();
    statsDataJson["stats"] = {
        {"level", level},
        {"health", health},
        {"health_max", healthMax},
        {"magicka", magicka},
        {"magicka_max", magickaMax},
        {"stamina", stamina},
        {"stamina_max", staminaMax},
        {"scale", scale}
    };
    
    HTTPManager::postGameData("gamedata.php", statsDataJson);
    
    int hpPercent = (int)((health / healthMax) * 100);
    int mpPercent = (int)((magicka / magickaMax) * 100);
    int spPercent = (int)((stamina / staminaMax) * 100);
    
    logger::info("[PLAYER_STATS_UPDATE] Player stats: Lv:{} HP:{:.0f}/{:.0f} ({}%), MP:{:.0f}/{:.0f} ({}%), SP:{:.0f}/{:.0f} ({}%)", 
                level, health, healthMax, hpPercent, magicka, magickaMax, mpPercent, stamina, staminaMax, spPercent);
}

void RefreshPlayerTransformationState(bool forceUpdate) {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    json transformationData = BuildTransformationStatePayload(player, player->GetDisplayFullName(), "player");
    if (transformationData.empty()) {
        return;
    }

    HTTPManager::postGameData("gamedata.php", transformationData);
    logger::trace("[PLAYER_TRANSFORMATION_UPDATE] Player transformation state updated{}", forceUpdate ? " (forced)" : "");
}

// Helper function to refresh spells for an AI Agent (with hash-based diffing)
void RefreshAIAgentSpells(RE::Actor* npc, const std::string& agentName, bool forceUpdate) {
    if (!npc) return;
    if (npc->IsPlayer()) return;  // Skip player
    
    // Safety checks
    if (npc->IsDeleted() || npc->IsDisabled() || npc->IsDead()) {
        return;
    }
    
    std::vector<std::string> spellsList;
    std::set<std::string> spellsSeen; // Track unique spells
    
    auto processSpell = [&](RE::SpellItem* spell) {
        if (!spell || !spell->GetName() || !spell->GetName()[0]) return;
        
        std::string spellName = spell->GetName();
        std::string spellID = std::format("{:08X}", spell->GetFormID());
        
        // Skip if name is empty or missing
        if (spellName.empty() || spellName == "<Missing Name>") {
            return;
        }
        
        // Strip suffixes like ": Added Spell" or ": Base Spell" from spell names
        // These are debug markers that shouldn't be part of the spell name
        size_t colonPos = spellName.find(": Added Spell");
        if (colonPos != std::string::npos) {
            spellName = spellName.substr(0, colonPos);
        } else {
            colonPos = spellName.find(": Base Spell");
            if (colonPos != std::string::npos) {
                spellName = spellName.substr(0, colonPos);
            }
        }
        
        // Skip duplicates
        if (spellsSeen.find(spellID) != spellsSeen.end()) {
            return;
        }
        spellsSeen.insert(spellID);
        
        // Get casting type from spell
        uint32_t castingType = static_cast<uint32_t>(spell->GetCastingType());
        
        // Get delivery type from first magic effect
        uint32_t deliveryType = 0; // Default to Self
        if (spell->effects.size() > 0) {
            auto effect = spell->effects[0];
            if (effect && effect->baseEffect) {
                deliveryType = static_cast<uint32_t>(effect->baseEffect->data.delivery);
            }
        }
        
        spellsList.push_back(std::format("{}^{}^{}^{}", spellName, spellID, castingType, deliveryType));
    };
    
    try {
        // Get spells from actor base spell list (base character spells)
        auto actorBase = npc->GetActorBase();
        if (actorBase) {
            auto spellList = actorBase->GetSpellList();
            if (spellList && spellList->numSpells > 0) {
                for (uint32_t i = 0; i < spellList->numSpells; i++) {
                    processSpell(spellList->spells[i]);
                }
            }
        }
        
        // ALSO get spells added during gameplay (runtime spells)
        auto& addedSpells = npc->GetActorRuntimeData().addedSpells;
        for (auto* spell : addedSpells) {
            if (spell) {
                auto spellItem = spell->As<RE::SpellItem>();
                if (spellItem) {
                    processSpell(spellItem);
                }
            }
        }
    } catch (...) {
        logger::trace("[SPELLS_SKIP] {} - exception during spell read", agentName);
        return;
    }
    
    // Sort for order-independent hash
    std::sort(spellsList.begin(), spellsList.end());
    
    // Create hash
    std::string spellsHash;
    for (const auto& spell : spellsList) {
        if (!spellsHash.empty()) spellsHash.append("|");
        spellsHash.append(spell);
    }
    
    auto formID = npc->GetFormID();
    
    // Check if changed
    if (!forceUpdate && lastSpellsHash.find(formID) != lastSpellsHash.end()) {
        if (lastSpellsHash[formID] == spellsHash) {
            logger::trace("[SPELLS_SKIP] {} spells unchanged", agentName);
            return;
        }
    }
    
    lastSpellsHash[formID] = spellsHash;
    
    // Build JSON
    json spellsData;
    spellsData["type"] = "spells";
    spellsData["actor_name"] = agentName;
    spellsData["actor_type"] = "npc";
    spellsData["timestamp"] = getCurrentTimeMillis();
    spellsData["gamets"] = GetGameTimeStamp();
    spellsData["spells"] = json::array();
    
    for (const auto& spellEntry : spellsList) {
        // Parse: name^baseid^castingType^deliveryType
        std::vector<std::string> parts;
        std::stringstream ss(spellEntry);
        std::string part;
        while (std::getline(ss, part, '^')) {
            parts.push_back(part);
        }
        
        if (parts.size() >= 4) {
            spellsData["spells"].push_back({
                {"name", parts[0]},
                {"baseid", parts[1]},
                {"casting_type", std::stoi(parts[2])},
                {"delivery", std::stoi(parts[3])}
            });
        }
    }
    
    HTTPManager::postGameData("gamedata.php", spellsData);
    logger::trace("[SPELLS_UPDATE] {} spells updated ({} spells)", agentName, spellsList.size());
}

void RefreshPlayerSpells(bool forceUpdate) {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    
    std::vector<std::string> spellsList;
    std::set<std::string> spellsSeen; // Track unique spells
    
    auto processSpell = [&](RE::SpellItem* spell) {
        if (!spell || !spell->GetName() || !spell->GetName()[0]) return;
        
        std::string spellName = spell->GetName();
        std::string spellID = std::format("{:08X}", spell->GetFormID());
        
        // Skip if name is empty or missing
        if (spellName.empty() || spellName == "<Missing Name>") {
            return;
        }
        
        // Strip suffixes like ": Added Spell" or ": Base Spell" from spell names
        // These are debug markers that shouldn't be part of the spell name
        size_t colonPos = spellName.find(": Added Spell");
        if (colonPos != std::string::npos) {
            spellName = spellName.substr(0, colonPos);
        } else {
            colonPos = spellName.find(": Base Spell");
            if (colonPos != std::string::npos) {
                spellName = spellName.substr(0, colonPos);
            }
        }
        
        // Skip duplicates
        if (spellsSeen.find(spellID) != spellsSeen.end()) {
            return;
        }
        spellsSeen.insert(spellID);
        
        // Get casting type from spell
        uint32_t castingType = static_cast<uint32_t>(spell->GetCastingType());
        
        // Get delivery type from first magic effect
        uint32_t deliveryType = 0; // Default to Self
        if (spell->effects.size() > 0) {
            auto effect = spell->effects[0];
            if (effect && effect->baseEffect) {
                deliveryType = static_cast<uint32_t>(effect->baseEffect->data.delivery);
            }
        }
        
        spellsList.push_back(std::format("{}^{}^{}^{}", spellName, spellID, castingType, deliveryType));
    };
    
    try {
        // Get spells from player base spell list (base character spells)
        auto actorBase = player->GetActorBase();
        if (actorBase) {
            auto spellList = actorBase->GetSpellList();
            if (spellList && spellList->numSpells > 0) {
                for (uint32_t i = 0; i < spellList->numSpells; i++) {
                    processSpell(spellList->spells[i]);
                }
            }
        }
        
        // ALSO get spells added during gameplay (runtime spells)
        auto& addedSpells = player->GetActorRuntimeData().addedSpells;
        for (auto* spell : addedSpells) {
            if (spell) {
                auto spellItem = spell->As<RE::SpellItem>();
                if (spellItem) {
                    processSpell(spellItem);
                }
            }
        }
    } catch (...) {
        logger::trace("[PLAYER_SPELLS_SKIP] Exception during spell read");
        return;
    }
    
    // Sort for order-independent hash
    std::sort(spellsList.begin(), spellsList.end());
    
    // Create hash
    std::string spellsHash;
    for (const auto& spell : spellsList) {
        if (!spellsHash.empty()) spellsHash.append("|");
        spellsHash.append(spell);
    }
    
    auto formID = player->GetFormID();
    
    // Check if changed
    if (!forceUpdate && lastSpellsHash.find(formID) != lastSpellsHash.end()) {
        if (lastSpellsHash[formID] == spellsHash) {
            return;
        }
    }
    
    lastSpellsHash[formID] = spellsHash;
    
    // Build JSON
    json spellsData;
    spellsData["type"] = "spells";
    spellsData["actor_name"] = player->GetDisplayFullName();
    spellsData["actor_type"] = "player";
    spellsData["timestamp"] = getCurrentTimeMillis();
    spellsData["gamets"] = GetGameTimeStamp();
    spellsData["spells"] = json::array();
    
    for (const auto& spellEntry : spellsList) {
        // Parse: name^baseid^castingType^deliveryType
        std::vector<std::string> parts;
        std::stringstream ss(spellEntry);
        std::string part;
        while (std::getline(ss, part, '^')) {
            parts.push_back(part);
        }
        
        if (parts.size() >= 4) {
            spellsData["spells"].push_back({
                {"name", parts[0]},
                {"baseid", parts[1]},
                {"casting_type", std::stoi(parts[2])},
                {"delivery", std::stoi(parts[3])}
            });
        }
    }
    
    HTTPManager::postGameData("gamedata.php", spellsData);
    logger::trace("[PLAYER_SPELLS_UPDATE] Player spells updated ({} spells)", spellsList.size());
}

OnLoadedGame {
    logger::info("OnLoadedGame");
    // Runs twice?
    auto player = RE::PlayerCharacter::GetSingleton();
    
    // Reset game timestamp to allow changes when loading a save
    ResetGameTimeStamp();

    if (!pluginInited) {
        logger::info("Game loaded");

       
        if (!Conf::getInstance().isOk()) {
            RE::DebugNotification("[CHIM] AIAgent.ini is missing or invalid.");
        }

        const bool serverReachable = Conf::getInstance().ping();
        if (!serverReachable) {
            RE::DebugNotification("[CHIM] Cannot connect to the server. Check AIAgent.ini.");
        }

        // setNewActionModeFromConfig();

        if (GetGameTimeStamp() == 13333334) return;  // ??
        // Queue cleaning
        BackGroundDialogueQueue.clear();
        SPGResponse::getInstance().clearAllQueues();
        ThreadPool::getInstance().cancelTasksByType("HTTPStream");
        ThreadPool::getInstance().cancelTasksByType("HTTPStreamRechat");

        auto now = std::chrono::high_resolution_clock::now();
        controlLastBoredTriggerTS = now;
        logger::info("[BORED_TIMER] Reset - game loaded");
        
        // Only add delay to dynamic profile timer, don't reset completely
        auto timeSinceLastLoad = std::chrono::duration_cast<std::chrono::seconds>(now - lastGameLoadTime);
        if (timeSinceLastLoad >= std::chrono::seconds(GAME_LOAD_COOLDOWN_SECONDS)) {
            controlLastDynamicProfileTS = controlLastDynamicProfileTS + std::chrono::seconds(30);
            logger::info("[DYNAMIC_TIMER] Added 30s delay after game load");
            lastGameLoadTime = now;
        } else {
            logger::info("[DYNAMIC_TIMER] Skipped delay - cooldown active ({}s since last load)", timeSinceLastLoad.count());
        }

        isGameReady = true;

        //ProcedureSendActiveQuests();

        SpeakManager::getInstance().deleteQueue();
        if (SpeakManager::getInstance().getProcessing()) {
            SpeakManager::getInstance().abortPlay();
        }

        ManagerMainQueue& mmq = ManagerMainQueue::getInstance();
        if (!mmq.isRunning()) {
            logger::info("Starting main manager thread  at OnLoadedGame");
            std::string polint = Conf::getInstance().getPolint();
            int polint_i = std::stoi(polint);
            mmq.startThread(polint_i);
        }

        AIAgentManager& aiam = AIAgentManager::getInstance();

        auto oldNarrator = aiam.getAgentByName(NARRATOR_NAME);
        if (oldNarrator) aiam.deleteAgent(oldNarrator);

        // Lets add the player/narrator agent
        if (ENABLE_AUTOADDNPC) {
            aiam.removeAllAgents();
        }

        auto narrator = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        player->SetDisplayName(player->GetName(), true);
        auto agent = aiam.createAgent();
        agent->setActor(narrator);
        agent->setAvailable(true);
        agent->setNarrator(true);
        agent->setOriginalVoice(narrator->GetActorBase()->voiceType);
        agent->overrideActorName(NARRATOR_NAME);
        logger::info("Narrator initialized for {}", narrator->GetDisplayFullName());
        aiam.addAgent(agent);

        aiam.setPlayerName(player->GetName());

        auto server = Conf::getInstance().getServer();
        auto port = Conf::getInstance().getPort();
        auto initMsg = std::format("[CHIM] Using server: http://{}:{}", server, port);
        RE::DebugNotification(initMsg.c_str());

        pendingLoadedPluginManifestSync = true;
        if (PostLoadedPluginManifest()) {
            pendingLoadedPluginManifestSync = false;
        } else {
            logger::warn("[LOADED_PLUGINS] Immediate sync from OnLoadedGame did not post; deferring until a loaded cell event");
        }

        std::string playerinfo;

        playerinfo.assign(std::format("level:{},name:\"{}\",race:\"{}\"", player->GetLevel(),
                                      player->GetDisplayFullName(), player->GetRace()->GetFullName()));
        
        
        HTTPManager::log(std::format("playerinfo|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), playerinfo));
        auto result =
            InspectSurroundings(player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);
        HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "(beings in range:" + result + ")"));

        char timeDateString[200];
        RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, true);

        auto resultLoc = InspectLocations(player->AsReference());

        HTTPManager::log(std::format("infoloc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "(Context location: " + std::string(GetPlayerLocation()) + ", Buildings to go:" +
                                         resultLoc + ", Current Date in Skyrim World: " + timeDateString + ")"));

        if (REL::Module::GetRuntime() != REL::Module::Runtime::VR) {
            // Send nearby items context BEFORE infonpc_close (so it gets logged in same request)
            std::string itemsResult = InspectNearbyItems(player->AsReference(), 256.0f);
            if (!itemsResult.empty()) {
                HTTPManager::log(std::format("infoitems|{}|{}|{}", getCurrentTimeMillis(),
                                             GetGameTimeStamp(), "(items in range:" + itemsResult + ")"));
            }
        }
        
        auto resultClose = InspectSurroundingsNavmesh(player->AsReference(), true, 3000, "/");
        if (!resultClose.empty()) {
            resultClose.append("/");
        }
        resultClose.append(AIAgentManager::getInstance().getPlayerName());
        HTTPManager::log(
            std::format("infonpc_close|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), resultClose));
        PostNearbyActivityStatus(player, 3000.0f);

        logger::debug("Sending init msg init|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp() + 1, PLUGIN_VERSION);
        HTTPManager::log(std::format("init|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp() + 1, PLUGIN_VERSION));
        ScheduleVersionMismatchStartupCheck();

        // Detect and upload CSV import data files on first load (only once per session)
        if (!importDataDetectionDone) {
            DetectAndUploadImportDataFiles();
            ScheduleServerPluginSync();
            importDataDetectionDone = true;
        }

        if (AIAgentRoleMasterFaction) {
            HTTPManager::log(std::format("setconf|{}|{}|{}@{:08x}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "aiagent_rolemastered_faction", AIAgentRoleMasterFaction->GetFormID()));
        } else {
            logger::info("AIAgentRoleMasterFaction not available - skipping setconf");
        }
        pluginInited = true;
        ResetQuestProgressionBridgeState();
        ScheduleQuestProgressionFullResync("loaded_game_initial", 4000, true);

        RefreshPlayerInventory(true);
        RefreshPlayerTransformationState(true);
    } else {
        BackGroundDialogueQueue.clear();
        SPGResponse::getInstance().clearAllQueues();
        ThreadPool::getInstance().cancelTasksByType("HTTPStream");
        ThreadPool::getInstance().cancelTasksByType("HTTPStreamRechat");
        
        auto now = std::chrono::high_resolution_clock::now();
        controlLastBoredTriggerTS = now;
        logger::info("[BORED_TIMER] Reset - game reloaded");
        
        // Only add delay to dynamic profile timer, don't reset completely
        auto timeSinceLastLoad = std::chrono::duration_cast<std::chrono::seconds>(now - lastGameLoadTime);
        if (timeSinceLastLoad >= std::chrono::seconds(GAME_LOAD_COOLDOWN_SECONDS)) {
            controlLastDynamicProfileTS = controlLastDynamicProfileTS + std::chrono::seconds(30);
            logger::info("[DYNAMIC_TIMER] Added 30s delay after game reload");
            lastGameLoadTime = now;
        } else {
            logger::info("[DYNAMIC_TIMER] Skipped delay - cooldown active ({}s since last load)", timeSinceLastLoad.count());
        }
        isGameReady = true;
        //ProcedureSendActiveQuests();
        SpeakManager::getInstance().deleteQueue();
        if (SpeakManager::getInstance().getProcessing()) {
            SpeakManager::getInstance().abortPlay();
        }

        std::string playerinfo;

        playerinfo.assign(std::format("level:{},name:\"{}\",race:\"{}\"", player->GetLevel(),
                                      player->GetDisplayFullName(), player->GetRace()->GetFullName()));

        
        
        auto result =
            InspectSurroundings(player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);
        HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "(beings in range:" + result + ")"));
        char timeDateString[200];
        RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, true);

        auto resultLoc = InspectLocations(player->AsReference());

        HTTPManager::log(std::format("infoloc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "(Context location: " + std::string(GetPlayerLocation()) + ", Buildings to go:" +
                                         resultLoc + ", Current Date in Skyrim World: " + timeDateString + ")"));

        if (REL::Module::GetRuntime() != REL::Module::Runtime::VR) {
            // Send nearby items context BEFORE infonpc_close (so it gets logged in same request)
            std::string itemsResult = InspectNearbyItems(player->AsReference(), 256.0f);
            if (!itemsResult.empty()) {
                HTTPManager::log(std::format("infoitems|{}|{}|{}", getCurrentTimeMillis(),
                                             GetGameTimeStamp(), "(items in range:" + itemsResult + ")"));
            }
        }
        
        auto resultClose = InspectSurroundingsNavmesh(player->AsReference(), true, 3000, "/");
        if (!resultClose.empty()) {
            resultClose.append("/");
        }
        resultClose.append(AIAgentManager::getInstance().getPlayerName());
        HTTPManager::log(
            std::format("infonpc_close|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), resultClose));
        PostNearbyActivityStatus(player, 3000.0f);


        logger::debug("Sending init msg");
        HTTPManager::log(std::format("init|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp() + 1, PLUGIN_VERSION));
        ScheduleVersionMismatchStartupCheck();
        // This will trigger narrator welcome
        HTTPManager::log(std::format("playerinfo|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), playerinfo));

      
        // Refresh inventory and equipment for all AI Agents after loading save
        // This ensures database is in sync with actual game state
        // Force updates to resync hashes after save load
        logger::info("[SAVE_LOAD] Refreshing equipment and inventory for all AI Agents");
        AIAgentManager& aiamRefresh = AIAgentManager::getInstance();
        int refreshCount = 0;
        for (const auto& agent : aiamRefresh.getAgents()) {
            auto agentActor = agent->getActor();
            std::string agentName = agent->getActorName();
            
            // Skip The Narrator (player)
            if (agentActor->GetFormID() == RE::PlayerCharacter::GetSingleton()->GetFormID()) {
                logger::trace("[SAVE_LOAD] Skipping refresh for {} - player FormID", agentName);
                continue;
            }
            
            // Force refresh equipment, inventory, skills, stats, and spells (bypass hash check to resync)
            RefreshAIAgentEquipment(agentActor, agentName, true);
            RefreshAIAgentInventory(agentActor, agentName, true);
            RefreshAIAgentSkills(agentActor, agentName, true);
            RefreshAIAgentStats(agentActor, agentName, true);
            RefreshAIAgentSpells(agentActor, agentName, true);
            refreshCount++;
        }
        logger::info("[SAVE_LOAD] Refreshed {} AI Agents (equipment, inventory, skills, stats, spells)", refreshCount);
        
        // Also refresh player data on load
        RefreshPlayerEquipment(true);
        RefreshPlayerInventory(true);
        RefreshPlayerSkills(true);
        RefreshPlayerStats(true);
        RefreshPlayerTransformationState(true);
        RefreshPlayerSpells(true);
        ResetQuestProgressionBridgeState();
        ScheduleQuestProgressionFullResync("loaded_game_reload", 4000, true);

    }

    
    std::thread([]() {
        // Give time to init code to finish
        std::this_thread::sleep_for(std::chrono::seconds(15));
        AIAgentManager& aiamRefresh = AIAgentManager::getInstance();
        // Iterate over renamed NPCs and log their FormID and name
        auto renamedNpcs = aiamRefresh.getRenamedNpcs();
        for (const auto& [formId, name] : renamedNpcs) {
            logger::info("Enabling BgL for NPC: FormID={:08X}, Name={}", formId, name);
            HTTPManager::log(
                std::format("enable_bg|{}|{}|{}/{:08X}", getCurrentTimeMillis(), GetGameTimeStamp(), name, formId));
        }
    }).detach();

    auto now = std::chrono::high_resolution_clock::now();
    controlLastBoredTriggerTS = now + std::chrono::seconds(30);
    logger::info("[BORED_TIMER] Initialized with 30s delay");
}

OnLoadingGame { 
    logger::info("OnLoadingGame");
    

    if (pluginInited) {
        logger::info("Cancelling all ongoing HTTP stream messages , game reloading");
        ThreadPool::getInstance().cancelTasksByType("HTTPStream");
        ThreadPool::getInstance().cancelTasksByType("HTTPStreamRechat");
        controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();
        logger::info("[BORED_TIMER] Reset - game loading");
        logger::info("[DYNAMIC_TIMER] Timer preserved during loading");
        
        SPGResponse::getInstance().clearAllQueues();

        SpeakManager::getInstance().deleteQueue(true);
        if (SpeakManager::getInstance().getProcessing()) {
            SpeakManager::getInstance().abortPlay();  // Stop NPCs talking
            // AudioManagerController::GetInstance().Stop(); // SHould not be need as audio stops when SpeakManager
            // stops
        }
        AIAgentManager& aiamRefresh = AIAgentManager::getInstance();
        aiamRefresh.resetRenamedNpcs();

    }

    // Reset game timestamp to allow changes when loading a game
    ResetGameTimeStamp();
    ManagerMainQueue& mmq = ManagerMainQueue::getInstance();
    mmq.stopThread();
    ResetQuestProgressionBridgeState();
    pluginInited = false; 
}

OnNewGame {

    logger::info("OnNewGame");

    /*
    ManagerMainQueue& serverPooler = ManagerMainQueue::getInstance();
    std::string polint = Conf::getInstance().getPolint();
    int polint_i = std::stoi(polint);
    serverPooler.startThread(polint_i);
    */

    // Reset game timestamp to allow changes when starting a new game
    ResetGameTimeStamp();
    ResetQuestProgressionBridgeState();

    RE::FormID NullVoiceTypeId = RE::TESDataHandler::GetSingleton()->LookupFormID((RE::FormID)0x1D70E, "AIAgent.esp");
    NullVoiceType = RE::TESForm::LookupByID(NullVoiceTypeId);

    logger::info("AIAgent.esp present");
    if (!Conf::getInstance().isOk()) {
        RE::DebugNotification("[CHIM] AIAgent.ini is missing or invalid.");
    }
    // setNewActionModeFromConfig();

    // Queue cleaning
    BackGroundDialogueQueue.clear();
    //HTTPManager::log(std::format("init|{}|{}|", getCurrentTimeMillis(), GetGameTimeStamp()));

    auto now = std::chrono::high_resolution_clock::now();
    controlLastBoredTriggerTS = now;
    controlLastDynamicProfileTS = now;
    logger::info("[BORED_TIMER] Initialized for new game");
    logger::info("[DYNAMIC_TIMER] Initialized for new game");

    isGameReady = true;
    auto player = RE::PlayerCharacter::GetSingleton();

    //ProcedureSendActiveQuests();
    /*
    ManagerMainQueue& mmq = ManagerMainQueue::getInstance();
    if (!mmq.isRunning()) {
        std::string polint = Conf::getInstance().getPolint();
        int polint_i = std::stoi(polint);
        mmq.startThread(polint_i);
    }
    SpeakManager::getInstance().deleteQueue();
    
    
    AIAgentManager& aiam = AIAgentManager::getInstance();

    auto oldNarrator = aiam.getAgentByName(NARRATOR_NAME);
    if (oldNarrator) aiam.deleteAgent(oldNarrator);

    // Lets add the player/narrator agent

    

    auto narrator = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
    auto agent = aiam.createAgent();
    agent->setActor(narrator);
    agent->setAvailable(true);
    agent->setNarrator(true);
    agent->setOriginalVoice(narrator->GetActorBase()->voiceType);
    agent->overrideActorName(NARRATOR_NAME);
    aiam.setPlayerName("Prisoner");
    logger::info("Narrator initialized for {}", narrator->GetDisplayFullName());
    aiam.addAgent(agent);


    */
    auto server = Conf::getInstance().getServer();
    auto port = Conf::getInstance().getPort();
    auto initMsg = std::format("[CHIM] Using server: http://{}:{}", server, port);
    RE::DebugNotification(initMsg.c_str());

    pendingLoadedPluginManifestSync = true;
    if (PostLoadedPluginManifest()) {
        pendingLoadedPluginManifestSync = false;
    } else {
        logger::warn("[LOADED_PLUGINS] Immediate sync from OnNewGame did not post; deferring until a loaded cell event");
    }

    std::string playerinfo;

    playerinfo.assign(std::format("level:{},name:\"{}\",race:\"{}\"", player->GetLevel(), player->GetDisplayFullName(),
                                  player->GetRace()->GetFullName()));

    //logger::info("Sending init msg");
    //HTTPManager::log(std::format("newgame|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), playerinfo));
    logger::debug("OnNewGame End");
    //pluginInited = true;
}

OnDataLoaded {
    
    logger::info("OnDataLoaded");
    VRItemAwareness::Initialize();
    logger::trace("Initializing trampoline...");
    auto& trampoline = SKSE::GetTrampoline();
    trampoline.create(42);

    const bool isVrRuntime = REL::Module::GetRuntime() == REL::Module::Runtime::VR;
    if (!isVrRuntime) {
        ProcessorScreenShot::InstallHooks();
        ProcessorNativeScreenShot::InstallHooks();
        ProcessorActorDialogue::InstallHooks();
        ProcessorDialogueMenu::InstallHooks();
    } else {
        logger::info("VR runtime detected; skipping flat Skyrim native screenshot/dialogue hooks");
        ProcessorVrVisemePump::InstallHooks();
    }

    const auto papyrus = SKSE::GetPapyrusInterface();
    papyrus->Register(Papyrus::RegisterSGPFuncs);

    auto* ui = RE::UI::GetSingleton();
    if (ui) {
        ui->AddEventSink<RE::MenuOpenCloseEvent>(&ProcessorMenu::ProcessorMenuEvent::GetInstance());
    }
    if (auto* modCallbackEventSource = SKSE::GetModCallbackEventSource()) {
        modCallbackEventSource->AddEventSink(&ProcessorPlayerMenuModEvent::PlayerMenuModCallbackEventSink::GetInstance());
    } else {
        logger::warn("[PlayerMenuTTS] Mod callback event source unavailable; native dialogue menu fallback disabled");
    }
    logger::info("OnDataLoaded End");

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (dataHandler) {
        RE::FormID NullVoiceTypeId = dataHandler->LookupFormID((RE::FormID)0x1D70E, "AIAgent.esp");
        NullVoiceType = RE::TESForm::LookupByID(NullVoiceTypeId);

        RE::FormID TargetFaction = dataHandler->LookupFormID((RE::FormID)0x021d0b, "AIAgent.esp");
        AIAgentRoleMasterFaction = RE::TESForm::LookupByID<RE::TESFaction>(TargetFaction);

        RE::FormID FollowFactionId = dataHandler->LookupFormID((RE::FormID)0x01BC24, "AIAgent.esp");
        AIAgentFollowFaction = RE::TESForm::LookupByID<RE::TESFaction>(FollowFactionId);
    } else {
        logger::info("TESDataHandler not available yet - AIAgent.esp forms will be loaded later");
    }

    if (!AIAgentRoleMasterFaction) {
        logger::info("AIAgentRoleMasterFaction could not be loaded");
    } else {
        logger::info("AIAgent.esp present");
    }
    if (!AIAgentFollowFaction) {
        logger::info("AIAgentFollowFaction could not be loaded");
    }

    // Initialize Prisma UI Bridge for conversation history panel
    if (PrismaUIBridge::Initialize()) {
        logger::info("Prisma UI Bridge initialized successfully");
        logger::info("Crosshair target tracking will run in ManagerMainQueue loop");
    } else {
        logger::info("Prisma UI not available - conversation history panel disabled");
    }

}

EventHandlers {

    On<RE::TESCellFullyLoadedEvent>([](const RE::TESCellFullyLoadedEvent* event) {
        // Location Change trigger
        if (GetGameTimeStamp() == 13333334) return;

        if (!pluginInited) {
            logger::trace("[TESCellFullyLoadedEvent] Cell loaded event bypassed");
            return;
        }

        auto* transitionPlayer = RE::PlayerCharacter::GetSingleton();
        auto* playerCell = transitionPlayer ? transitionPlayer->GetParentCell() : nullptr;
        if (!playerCell || !playerCell->IsAttached()) {
            logger::trace("[TESCellFullyLoadedEvent] Player cell unavailable; skipping transition handling");
        } else {
            static RE::FormID staticLastPlayerCellFormId = 0;
            const RE::FormID currentPlayerCellFormId = playerCell->GetFormID();
            if (staticLastPlayerCellFormId == 0) {
                staticLastPlayerCellFormId = currentPlayerCellFormId;
            } else if (staticLastPlayerCellFormId != currentPlayerCellFormId) {
                const RE::FormID previousPlayerCellFormId = staticLastPlayerCellFormId;
                staticLastPlayerCellFormId = currentPlayerCellFormId;

                logger::info("[RECHAT_CELL_CANCEL] Player cell changed {:08X} -> {:08X}; cancelling autonomous dialogue",
                             previousPlayerCellFormId, currentPlayerCellFormId);

                VRItemAwareness::CancelPendingHandoff("player changed cells");

                SpatialAwareness::InvalidateCache();
                SpatialSnapshotManager::InvalidateForEnvironmentChange(std::chrono::milliseconds(2000));
                ThreadPool::getInstance().cancelTasksByType("HTTPStreamRechat");

                SpeakManager& speakManager = SpeakManager::getInstance();
                speakManager.abortPendingUtterances("cell_change");
                speakManager.cancelRechatChain();
                speakManager.deleteQueue();
                if (speakManager.getProcessing()) {
                    speakManager.abortPlay(true);
                    AudioManagerController::GetInstance().Stop();
                }
                speakManager.stopRechatForNseconds(3);

                SPGResponse::getInstance().clearAllQueues();
                SPGResponse::getInstance().markUnFinished(false);
                BackGroundDialogueQueue.clear();
                AudioFilesBufferManager::clear();
            }
        }

        // Cell streaming can fire in bursts. Keep broad context scans out of
        // the same window as Papyrus sendCellInfo/equipment detach storms.
        ExtendWorldMaintenanceSuppress(std::chrono::seconds(6));

        if (pendingLoadedPluginManifestSync) {
            if (PostLoadedPluginManifest()) {
                pendingLoadedPluginManifestSync = false;
            } else {
                logger::warn("[LOADED_PLUGINS] Deferred sync still not ready on loaded cell; will retry on the next loaded cell event");
            }
        }

        static std::string staticLastLocation;
        std::string cLoc(GetPlayerLocation());
        if (cLoc.compare(staticLastLocation.c_str()) == 0) {
            // Same location
        } else {
            

            char timeDateString[200];
            RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, true);

            HTTPManager::log(std::format("location|{}|{}|(Context new location: {}, {})", getCurrentTimeMillis(),
                             GetGameTimeStamp(), GetPlayerLocation(), BuildCurrentWorldContextDetails()));

            auto player = RE::PlayerCharacter::GetSingleton();

            RE::TESObjectCELL* cell = player->GetParentCell();
            PostQuestProgressionLocationEntered(cell);
            auto resultLoc = InspectLocations(player->AsReference());
            
            RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, true);
            HTTPManager::log(std::format("infoloc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "(Context location: " + std::string(GetPlayerLocation()) +
                                             ", Buildings to go:" + resultLoc +
                                             ", Current Date in Skyrim World: " + timeDateString + ")"));

            /*
            auto result = InspectSurroundings(player->AsReference(), true,HERIKA_MAX_VISION_RANGE);
            HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "(beings in range:" + result + ")"));
            */

            auto result = InspectSurroundingsNavmesh(player->AsReference(), true, 3000, "/");
            if (!result.empty()) {
                result.append("/");
            }
            result.append(AIAgentManager::getInstance().getPlayerName());
            HTTPManager::log(std::format("infonpc_close|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), result));


            BackGroundDialogueQueue.clear();
            AudioFilesBufferManager::clear();

            staticLastLocation.assign(cLoc);

            logger::info("New location {}", cLoc.c_str());

            // Follow persistence: teleport following agents to new location
            // Update: Out of scope. This behavior is managed by packages/papyrus scripts.
            // If you have a bug about people not following you across cells, look elsewhere, this is not the place to fix such issue.

            /*
            if (AIAgentFollowFaction) {
                AIAgentManager& followAiam = AIAgentManager::getInstance();
                for (const auto& agent : followAiam.getAgents()) {
                    if (!agent) continue;
                    RE::Actor* actor = agent->getActorByFormId();
                    if (!actor || actor->IsDead()) continue;
                    if (!actor->IsInFaction(AIAgentFollowFaction)) continue;

                    auto* actorCell = actor->GetParentCell();
                    if (actorCell && actorCell == player->GetParentCell()) continue;

                    actor->MoveTo(player);
                    logger::info("[FollowPersist] Moved {} to player at {}",
                                 actor->GetDisplayFullName(), cLoc);
                }
            }
            */
        }

        // Lets send cell info to Server
        // auto game = RE::UI::GetSingleton();
        auto* cell = event->cell;
        if (!cell) {
            return;
        }

        
        const auto cellFormID = cell->GetFormID();
        // Town entry can fire 26+ cell-load events in 1s, saturating Papyrus VM.
        {
            static std::chrono::steady_clock::time_point lastDispatch;
            static std::mutex lastDispatchMtx;
            std::lock_guard<std::mutex> lk(lastDispatchMtx);
            const auto now = std::chrono::steady_clock::now();
            if (now - lastDispatch < std::chrono::milliseconds(6000)) {
                logger::trace("[TESCellFullyLoadedEvent] Throttled cell <{:#x}>", cellFormID);
                return;
            }
            lastDispatch = now;
        }

        QueueDelayedSendCellInfo(cellFormID);
        
        logger::trace("[TESCellFullyLoadedEvent] Queued delayed SendCellInfo for cell <{:#x}>", cellFormID);
        
        
    }
    );

    On<RE::TESLockChangedEvent>([](const RE::TESLockChangedEvent* event) {


        auto object = event->lockedObject;

        auto lpmTr = RE::LockpickingMenu::GetTargetReference();
        if (!lpmTr) return;

      
        auto* player = RE::PlayerCharacter::GetSingleton();

        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - controlLastLockPickedTS);
        if (elapsed <= std::chrono::seconds(60)) return;
        controlLastLockPickedTS = now;

        if (elapsed.count() % 4 == 0) return;  // 25% chance

        if (lpmTr->GetFormID() == object->GetFormID())
            if (object->GetName())
                HTTPManager::stream(std::format("lockpicked|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), object->GetName()));
    });

    On<RE::TESCellAttachDetachEvent>([](const RE::TESCellAttachDetachEvent* event) {

        if (event->attached == 1) {
            if (!event->reference) return;
            auto ref = event->reference;
            if (!ref->data.objectReference) return;
            auto ref1 = ref->data.objectReference->formID;
            auto form2 = RE::TESForm::LookupByID(ref1);
            if (!form2) return;
            

            if (form2->formType == RE::FormType::Door) {
                auto door = form2->As<RE::TESObjectDOOR>();
                if (door) {
                    RE::ExtraDataList* extra = &ref->extraList;

                    for (RE::ExtraDataList::iterator entry = extra->begin(); entry != extra->end(); ++entry) {
                        RE::BSExtraData* extraData = &(*entry);

                        if (extraData->GetType() == RE::ExtraDataType::kTeleport) {
                            RE::ExtraTeleport* extraTeleport = static_cast<RE::ExtraTeleport*>(extraData);
                            RE::TESObjectREFR* destination = extraTeleport->teleportData->linkedDoor.get().get();

                            // Use door's activate text as name
                            RE::BSString actText;
                            door->GetActivateText(event->reference, actText);

                            // With the following code:
                            auto locNameFull = std::string(actText);
                            std::string locName;
                            auto pos = locNameFull.find('\n');
                            if (pos != std::string::npos) {
                                locName = locNameFull.substr(pos + 1);
                            } else {
                                locName = locNameFull;
                            }
                            // Remove all text after the first '<' character, if present
                            auto ltPos = locName.find('<');
                            if (ltPos != std::string::npos) {
                                locName = locName.substr(0, ltPos);
                            }
                            locName.append(" (door/passage)");
                            LocationList::GetInstance().AddLocation(locName, ref);

                            /*
                            RE::TESObjectCELL* cellDoor = destination->GetParentCell();
                            if (cellDoor)
                                LocationList::GetInstance().AddLocation(cellDoor->GetName(), ref);
                            else {  // Outside door?
                                auto destForm = RE::TESForm::LookupByID(destination->formID)->As<RE::TESObjectREFR>();
                                auto loc = destForm->GetCurrentLocation();
                                if (!loc) continue;

                                auto locName = std::string(loc->GetName());

                                if (object) {
                                    RE::BSString actText;  
                                    object->GetActivateText(destForm,
                                                            actText);
                                    locName.assign(actText.c_str());

                                }
                                

                                std::string locText;

                                if (destForm->GetParentCell()) {
                                    auto cell = destForm->GetParentCell();
                                    auto rtdata = cell->GetRuntimeData();
                                    if (rtdata.worldSpace->GetFormID() == 0x03c) {  // Skyrim's worldspace
                                        locText.assign(door->GetFullName()).append("(exit)");
                                    } 
                                } else {
                                    locText.assign(door->GetFullName()).append("(passage to " + locName + ")");
                                }

                                
                                LocationList::GetInstance().AddLocation(
                                    locText, ref);
                                // logger::info("Extra data {}", locName);
                            }*/
                        }
                    }
                }

            } else if (form2->formType == RE::FormType::NPC) {
                auto npc = form2->As<RE::TESNPC>();
                if (npc) {
                    
                    RE::ExtraDataList* extra = &ref->extraList;
                    NPCList::GetInstance().AddNPC(npc->GetName(), ref);

                   
                }

                auto npcRef = event->reference;
                if (npcRef) {
                    if (npcRef->GetFactionOwner() == AIAgentRoleMasterFaction && AIAgentRoleMasterFaction != nullptr) {
                        RE::Actor* actor = npcRef->As<RE::Actor>();
                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                        auto args = RE::MakeFunctionArguments(std::move(actor));
                        logger::info("[TESCellAttachDetachEvent] AIAgentRoleMasterFaction NPC loaded {:#x} {}", form2->GetFormID(), npcRef->GetDisplayFullName());
                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                            "AIAgentAIMind", "AddDelayedNPC", args, callback);
                    }
                }

            } else if (form2->formType == RE::FormType::Armor) {
                
                    //logger::info("TESObjectLoadedEvent item: {:#x} {} ", form2->GetFormID(), form2->GetName());
                    
                    auto item = event->reference;


                    if (item) {
                        if (item->GetFactionOwner() == AIAgentRoleMasterFaction && AIAgentRoleMasterFaction != nullptr) {
                            logger::info("AIFaction item loaded {:#x} {}", form2->GetFormID(),
                                         item->GetDisplayFullName());

                            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                            auto args = RE::MakeFunctionArguments(std::move(item));
                            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                "AIAgentAIMind", "AddDelayedHint", args, callback);

                            
                        } 
                    }
                

            } else if (form2->formType == RE::FormType::Misc) {
                //logger::info("TESObjectLoadedEvent item: {:#x} {} ", form2->GetFormID(), form2->GetName());
               auto item = event->reference;

               if (item->GetFactionOwner() == AIAgentRoleMasterFaction && AIAgentRoleMasterFaction != nullptr) {
                   logger::info("AIFaction item loaded {:#x} {}, event attached: {}", item->GetFormID(),
                                item->GetDisplayFullName(), event->attached);
                   std::thread([item]() {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        RE::TESObjectREFR *localIitem = item;
                        if (localIitem) {
                            logger::info("AIFaction item loaded {:#x} {}", localIitem->GetFormID(),
                                         localIitem->GetDisplayFullName());
                            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                            auto args = RE::MakeFunctionArguments(std::move(localIitem));
                            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                "AIAgentAIMind", "AddDelayedHint", args, callback);
                        }
                    }).detach();
                }
            
            } else if (form2->formType == RE::FormType::NPC) {
               
            }
            
        } else {
            if (!event->reference) return;
            auto ref = event->reference;
            if (!ref->data.objectReference) return;
            auto ref1 = ref->data.objectReference->formID;
            auto form2 = RE::TESForm::LookupByID(ref1);
            if (!form2) return;
            if (form2->formType == RE::FormType::Door) {
                auto door = form2->As<RE::TESObjectDOOR>();
                if (door) {
                    RE::ExtraDataList* extra = &ref->extraList;

                    for (RE::ExtraDataList::iterator entry = extra->begin(); entry != extra->end(); ++entry) {
                        RE::BSExtraData* extraData = &(*entry);

                        if (extraData->GetType() == RE::ExtraDataType::kTeleport) {
                            RE::ExtraTeleport* extraTeleport = static_cast<RE::ExtraTeleport*>(extraData);
                            RE::TESObjectREFR* destination = extraTeleport->teleportData->linkedDoor.get().get();

                            // Use door's activate text as name
                            RE::BSString actText;

                            door->GetActivateText(event->reference, actText);

                            // With the following code:
                            auto locNameFull = std::string(actText);
                            std::string locName;
                            auto pos = locNameFull.find('\n');
                            if (pos != std::string::npos) {
                                locName = locNameFull.substr(pos + 1);
                            } else {
                                locName = locNameFull;
                            }
                            // Remove all text after the first '<' character, if present
                            auto ltPos = locName.find('<');
                            if (ltPos != std::string::npos) {
                                locName = locName.substr(0, ltPos);
                            }
                            locName.append(" (door/passage)");
                            LocationList::GetInstance().RemoveLocation(locName);

                            /*
                            RE::TESObjectCELL* cellDoor = destination->GetParentCell();
                            if (cellDoor) {
                                // logger::info("Teleport removed {}", cellDoor->GetName());
                                LocationList::GetInstance().RemoveLocation(cellDoor->GetName());
                            } else {  // Outside door?
                                auto destForm = RE::TESForm::LookupByID(destination->formID)->As<RE::TESObjectREFR>();
                                auto loc = destForm->GetCurrentLocation();
                                if (!loc) continue;
                                auto locName = std::string(loc->GetName());

                                LocationList::GetInstance().RemoveLocation("passage to " + locName);
                                // logger::info("Extra data {}", locName);
                            }
                            */
                        }
                    }
                }
            } else if (form2->formType == RE::FormType::NPC) {
                auto npc = form2->As<RE::TESNPC>();
                if (npc) {
                    RE::ExtraDataList* extra = &ref->extraList;
                    NPCList::GetInstance().RemoveNPC(npc->GetName());
                    
                }
            }
        }

         
      
    });

    On<RE::TESPerkEntryRunEvent>([](const RE::TESPerkEntryRunEvent* event) {
        
    });

    On<RE::TESActivateEvent>([](const RE::TESActivateEvent* event) {
        
        try {
            //logger::info("TESActivateEvent");
            if (!event->objectActivated) return;

            auto activatedS = event->objectActivated;
            if (activatedS) {
                if (activatedS->GetBaseObject()) {
                    if (activatedS->GetBaseObject()->GetName()) {
                        /*logger::info("TESActivateEvent {}  {:#x}", activatedS->GetBaseObject()->GetName(),
                                     activatedS->GetBaseObject()->GetFormID());*/
                    }
                }
            }

            if (!event->actionRef) {
               /* logger::info("TESActivateEvent {}  {:#x}, exited because no activator", activatedS->GetBaseObject()->GetName(),
                             activatedS->GetBaseObject()->GetFormID());*/
                return;
            }

            auto activatorS = event->actionRef->GetBaseObject();
            auto activated2 = event->objectActivated->GetBaseObject();
            if (!activatorS || !activated2) {
                return;
            }

            auto activator = activatorS->GetName();
            auto activated = activated2->GetName();

            RE::TESObjectREFR* objectPointer = nullptr;

            auto refObjActivator = event->actionRef;

            if (event->objectActivated) objectPointer = event->objectActivated.get();

            if (refObjActivator->GetFormID() ==
                RE::PlayerCharacter::GetSingleton()->GetFormID()) {  // Player activates something
                if (objectPointer) {
                    if (objectPointer->GetFactionOwner() == AIAgentRoleMasterFaction) {
                        HTTPManager::log(std::format("itemfound|{}|{}|{} found {} {}", getCurrentTimeMillis(),
                                                     GetGameTimeStamp(), RE::PlayerCharacter::GetSingleton()->GetName(),
                                                     1, objectPointer->GetDisplayFullName()));
                    }
                }
            }

            if (activatedS->formType == RE::FormType::ActorCharacter) {

            } else if (activated2->formType == RE::FormType::Book) {
                // Herika reads a book

                auto activatedName = objectPointer->GetDisplayFullName();
                HTTPManager::log(
                    std::format("book|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), activatedName));

                if (refObjActivator->GetFormID() ==
                    RE::PlayerCharacter::GetSingleton()->GetFormID()) {  // Player activated book

                    logger::info("TESActivateEvent {}  {:#x}", activated2->GetName(), activated2->GetFormID());

                    RE::TESForm* realObject = RE::TESForm::LookupByID(objectPointer->GetFormID());
                    std::string name(activated2->GetName());
                    if (realObject) {
                        if (name == "Generic Note" &&
                            !DynamicDiaryBook::IsPhysicalDiaryBook(activated2->As<RE::TESObjectBOOK>())) {
                            // AIAgent faction. is an ethereal note
                            std::string hashName = md5low(trim(activatedName), false);
                            std::string sourceFilePath = "data/textures/AIAgent/Books/" + hashName + ".png";
                            std::string destinationFilePath = "data/textures/AIAgent/Books/Note01.png";

                            std::ifstream srcFile(sourceFilePath, std::ios::binary);

                            std::ofstream destFile(destinationFilePath, std::ios::binary);

                            // Check if both files are open
                            if (srcFile.is_open() && destFile.is_open()) {
                                destFile << srcFile.rdbuf();
                            } else {
                                logger::info("Error: Could not open source or destination file {} {}", sourceFilePath,
                                             destinationFilePath);
                            }

                            // Close the files
                            srcFile.close();
                            destFile.close();
                            realObject->AsReference()->InitializeDataComponent();

                            // bookForm->InitializeData();
                        }
                    }
                }
                // saySomethingHerika();  // Temporal
            } else if (activated2->formType == RE::FormType::Armor) {
                // saySomethingHerika();  // Temporal
            } else if (activated2->formType == RE::FormType::Furniture) {
                // CHECK DRIVEN NPC

                // CartFurniturePassenger "Carriage" [FURN:00103445]

                if (refObjActivator->GetFormID() ==
                    RE::PlayerCharacter::GetSingleton()->GetFormID()) {  // Player activates something
                    std::string itemActivated = activated2->GetName();
                    HTTPManager::log(
                        std::format("infoaction|{}|{}|{} uses {}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                    RE::PlayerCharacter::GetSingleton()->GetDisplayFullName(), itemActivated));
                } else {
                    AIAgentManager& aiam = AIAgentManager::getInstance();

                    for (const auto& agent : aiam.getAgents()) {
                        if (!agent) {
                            continue;
                        }

                        auto* agentActor = agent->getActorByFormId();
                        if (agentActor && refObjActivator->GetFormID() ==
                            agentActor->GetFormID()) {  // Agent activates something
                            logger::trace("[ACTIVITY_STATUS] Suppressing legacy furniture event text for agent {}",
                                          agentActor->GetDisplayFullName());
                        }
                    }
                }

            } else if (activated2->formType == RE::FormType::Door) {
                InvalidateSpatialCachesForDoor(event->objectActivated.get(), "activate_event");

                if (refObjActivator->GetFormID() ==
                    RE::PlayerCharacter::GetSingleton()->GetFormID()) {  // Player activates something
                    std::string name(activated);

                    if (name == "Chim Portal") {
                        auto door = event->objectActivated.get();
                        if (door) {
                            logger::info("Chim Portal activated by player {:#x}", door->GetFormID());
                            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                            auto args = RE::MakeFunctionArguments(std::move(door));
                            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                "AIAgentAIMind", "ChimTeleportDoorActivated", args, callback);
                        } else {
                            logger::info("Chim Portal activated by player - no door ref");
                        }
                    }
                } 

            } else if (activated2->formType == RE::FormType::Activator) {
                if (refObjActivator->GetFormID() ==
                    RE::PlayerCharacter::GetSingleton()->GetFormID()) {  // Player activates something

                    std::string itemActivated = activated2->GetName();
                    std::string itemActivatedAlt = activated2->GetFormEditorID();
                    logger::info("[ACTIVATION] Player activates {} {}  {:X}", itemActivated, itemActivatedAlt,
                                 activated2->GetFormID());
                    HTTPManager::log(std::format(
                        "infoaction|{}|{}|{} activates {}  (hint:{})", getCurrentTimeMillis(), GetGameTimeStamp(),
                        RE::PlayerCharacter::GetSingleton()->GetDisplayFullName(), itemActivated, itemActivatedAlt));
                } else {
                    std::string itemActivated = activated2->GetName();
                    std::string itemActivatedAlt = activated2->GetFormEditorID();
                    logger::info("[ACTIVATION] Something gets activated {} {}  {:X}", itemActivated, itemActivatedAlt,
                                 activated2->GetFormID());
                }
            } 
        }
        catch (...) {
            std::exception_ptr p = std::current_exception();
            logger::info("Error TESActivateEvent");
        }
    });
    
    //Is this working?
    /*
    On<RE::TESOpenCloseEvent>([](const RE::TESOpenCloseEvent* event) {
        logger::info("TESOpenCloseEvent");
        try {
            auto ref = event->activeRef;
            logger::info("[OPENCLOSE] {:X}, opened: ", ref->GetFormID(), event->opened);
        } catch (...) {
            std::exception_ptr p = std::current_exception();
            logger::info("Error TESOpenCloseEvent");
        }
        

    });
    */
    On<RE::TESDeathEvent>([](const RE::TESDeathEvent* event) {
        try {
            if (!event->actorDying) return;
            if (!event->actorKiller) {
                //Died
                if (event->dead == 0) {
                    PostQuestProgressionActorDead(event->actorDying.get()->As<RE::Actor>());
                }

                auto activated = event->actorDying->GetDisplayFullName();
                std::string victimLegend;
                victimLegend.append(activated);
                HTTPManager::log(
                    std::format("death|{}|{}|(Context location: {}){} died", getCurrentTimeMillis(),
                                             GetGameTimeStamp(), GetPlayerLocation(), victimLegend
                                ));
                return;


            };

            auto activated = event->actorDying->GetDisplayFullName();
            auto activator = event->actorKiller->GetDisplayFullName();

            auto victim = event->actorDying->As<RE::Actor>();

            bool killedByTeammate = false;

            std::string victimLegend;
            victimLegend.append(activated);
            std::string victimRace;
            victimRace.append(victim->GetRace()->GetFullName());
            victimRace.append(" lvl: " + std::format("{}",victim->GetLevel()));
            // logger::info("death,{},{},{},{}", GetGameTimeStamp(), GetPlayerLocation(), activator, activated);
            if (event->dead == 0) {  // Died
                PostQuestProgressionActorDead(event->actorDying.get()->As<RE::Actor>());
                // Notice ASAP
                if (event->actorKiller->GetFormID() == RE::PlayerCharacter::GetSingleton()->GetFormID()) {
                    // Only player kills
                    // Check if Actor was killed by player or NPC party
                    // CHECK DRIVEN NPC

                    AIAgentManager& aiam = AIAgentManager::getInstance();
                    auto player = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
                    std::string beings = InspectManagedAgents(player->AsReference(), HERIKA_MAX_VISION_RANGE, ",",
                                                              DISTANCE_ACTIVATING_NPC_OUT);

                    if (victim->GetLevel() > player->GetLevel()) {
                        victimLegend.append("(powerful enemy)");
                        forceCombatEnd = true;
                    }
                    
                  

                    if (victimRace.contains("Dragon Race")) {
                        victimLegend.append("(powerful DRAGON)");
                        forceCombatEnd = true;
                    }

                    for (const auto& agent : aiam.getAgents()) {
                        // AI AGent chat
                        auto* actor = agent->getActor();
                        if (agent->isNarrator()) continue;

                        RE::Actor* herika = agent->getActor();
                        if (!agent->isPresent(beings)) 
                            continue;

                        auto herikasTarget = herika->GetActorRuntimeData().currentCombatTarget;
                      
                        logger::info("This actor was killed by: {}, Race:{} ", event->actorDying.get()
                                                                         ->As<RE::Actor>()
                                                                         ->GetActorRuntimeData()
                                                                         .myKiller.get()
                                         ->GetDisplayFullName(),
                                     victimRace);

                        if (herikasTarget && event->actorDying.get()->GetFormID() == herikasTarget.get()->GetFormID() &&
                            (herika->IsAttacking() || herika->IsInKillMove()) &&
                            !event->actorKiller.get()->As<RE::Actor>()->IsInKillMove()) {
                            // Killed by Actor
                            logger::debug("Agent kill");
                            RE::TESForm* possibleWeapon = herika->GetEquippedObject(false);
                            if (possibleWeapon) {
                                
                                if (herika->IsInKillMove())
                                    HTTPManager::log(std::format(
                                        "death|{}|{}|(Context location: {}){} has defeated {} with {} in an awesome move",
                                        getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                        herika->GetDisplayFullName(),
                                        victimLegend, possibleWeapon->GetName()));
                                else {
                                    // Check if "boss"
                                    if (possibleWeapon->GetFormType() == RE::FormType::Weapon) {
                                            HTTPManager::log(std::format(
                                                "death|{}|{}|(Context location: {}){} has defeated {} using weapon {}",
                                                getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                                herika->GetDisplayFullName(), victimLegend, possibleWeapon->GetName()));
                                    }else if (possibleWeapon->GetFormType() == RE::FormType::Weapon) {
                                        HTTPManager::log(std::format(
                                            "death|{}|{}|(Context location: {}){} has defeated {} using spell {}",
                                            getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                            herika->GetDisplayFullName(), victimLegend, possibleWeapon->GetName()));
                                    }
                                    else {
                                        HTTPManager::log(std::format(
                                            "death|{}|{}|(Context location: {}){} has defeated {} using {} ",
                                            getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                            herika->GetDisplayFullName(), victimLegend, possibleWeapon->GetName()));
                                    }
                                }
                            }
                            else {
                                
                                if (herika->IsInKillMove())
                                    HTTPManager::log(std::format(
                                        "death|{}|{}|(Context location: {}){} has defeated {}  in an awesome move",
                                        getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                        herika->GetDisplayFullName(), victimLegend));
                                else
                                    HTTPManager::log(std::format("death|{}|{}|(Context location: {}){} has defeated {}",
                                                     getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(), herika->GetDisplayFullName(),
                                                                 victimLegend));
                            }
                            killedByTeammate = true;
                            break;
                        }
                    }
                        
                    // Player kill
                    if (!killedByTeammate) {
                        logger::debug("Player kill");
                        RE::TESForm* possibleWeapon =
                            event->actorKiller.get()->As<RE::Actor>()->GetEquippedObject(false);

                        if (possibleWeapon) {
                    
                            if (player->IsInKillMove()) {
                        
                                HTTPManager::log(std::format(
                                    "death|{}|{}|(Context location: {}){} has defeated {} with {} in an awesome move",
                                    getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(), activator,
                                    victimLegend, possibleWeapon->GetName()));
                            } else {
                        
                                HTTPManager::log(std::format("death|{}|{}|(Context location: {}){} has defeated {} with {}",
                                                    getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                                activator, victimLegend, possibleWeapon->GetName()));
                            }
                        } else {
                    

                            HTTPManager::log(std::format("death|{}|{}|(Context location: {}){} has defeated {}",
                                                getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(), activator, victimLegend));
                        }  
                    }

                    
                } else if (event->actorDying->GetFormID() == RE::PlayerCharacter::GetSingleton()->GetFormID()) {
                    HTTPManager::log(std::format("playerdied|{}|{}|(Context location: {}){} has killed {}",
                                                 getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                                 activator, victimLegend)); 
                    SpeakManager::getInstance().abortPlay();
                    SpeakManager::getInstance().deleteQueue();
                    ManagerMainQueue::getInstance().stopThread();
                    
                
                } else {
                    HTTPManager::log(std::format("death|{}|{}|(Context location: {}) {} has killed {}",
                                                 getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                                 activator, victimLegend)); 
                }
            }

        } catch (...) {
            std::exception_ptr p = std::current_exception();
            logger::info("Error");
        }

        return;
    });

    On<RE::TESCombatEvent>([](const RE::TESCombatEvent* event) {
        // Combat kNone = 0,        kCombat = 1, kSearching = 2

        if (!event->actor) return;

        auto activatorS = event->actor.get();
        const bool worldMaintenanceSuppressed = IsWorldMaintenanceSuppressed();

        if (activatorS->formType == RE::FormType::ActorCharacter) {
            RE::Actor* target = activatorS->As<RE::Actor>();
            // CHECK DRIVEN NPC

            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto player = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
            RE::Actor *agentPointer=nullptr;
            for (const auto& agent : aiam.getAgents()) {
                auto actor = agent->getActorByFormId();
                if (!actor) continue;//     

                if (actor->GetFormID() == target->GetFormID()) {
                    agentPointer = actor;
                }

            }
            if (!agentPointer) {
                if (target->GetFormID() == 0x14 || target->GetFormID() == 0x7) {
                    auto narrator = aiam.getAgentByName(NARRATOR_NAME);
                    if (narrator) agentPointer = narrator->getActor();
                }
            }

            auto playerSingleton = RE::PlayerCharacter::GetSingleton();
            const bool targetIsPlayer = playerSingleton && target->GetFormID() == playerSingleton->GetFormID();

            if (agentPointer && worldMaintenanceSuppressed) {
                if (targetIsPlayer &&
                    event->newState == RE::ACTOR_COMBAT_STATE::kNone) {
                    playerPartyCombatActive = false;
                }
                ThreadPool::getInstance().cancelTasksByType("CombatBark");
                ThreadPool::getInstance().cancelTasksByType("CombatBarkStart");
                return;
            }

            if (agentPointer && !targetIsPlayer && !IsActorLoadedInPlayerCell(agentPointer)) {
                ThreadPool::getInstance().cancelTasksByType("CombatBark");
                ThreadPool::getInstance().cancelTasksByType("CombatBarkStart");
                return;
            }

            if (agentPointer)
                if (event->newState == RE::ACTOR_COMBAT_STATE::kNone) {  // Actor finishes combat

                    auto player = RE::PlayerCharacter::GetSingleton();
                    
                    // Reset global combat flag when PLAYER exits combat
                    if (target->GetFormID() == player->GetFormID()) {
                        playerPartyCombatActive = false;
                        logger::info("[COMBAT_END] Player exited combat - reset global combat flag");
                    }

                    // Cancel all pending combat bark tasks
                    logger::info("[COMBAT_END] Cancelling all combat bark tasks for {}", target->GetDisplayFullName());
                    ThreadPool::getInstance().cancelTasksByType("CombatBark");
                    ThreadPool::getInstance().cancelTasksByType("CombatBarkStart");

                    // Update stats when exiting combat (capture post-combat state)
                    AIAgentManager& aiamStats = AIAgentManager::getInstance();
                    for (const auto& agent : aiamStats.getAgents()) {
                        // Had some crash here, related to GetFormID. 
                        if (!agent) continue;   
                        if (agent->GetFormId() == target->GetFormID()) {
                            RefreshAIAgentStats(agent->getActor(), agent->getActorName(), false);
                            RefreshAIAgentActivityStatus(agent->getActor(), agent->getActorName());
                            logger::info("[COMBAT_END] Updated stats for {} after combat", agent->getActorName());
                            break;
                        }
                    }

                    // 50% chance, 30 sec cooldown

                    if (!forceCombatEnd) {
                    
                        if (player->IsInCombat()) return;  // Avoid when still fighting

                        auto now = std::chrono::high_resolution_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - controlLastCombatEndTS);
                        if (elapsed <= std::chrono::seconds(30)) return;

                        if (elapsed.count() % 2 == 0) return;  // 50% chance

                        controlLastCombatEndTS = now;

                        logger::info("TESCombatEvent  Actor {} {}", target->GetName(), GetCombatStateString(event));
                        HTTPManager::stream(std::format("combatend|{}|{}|(Context location: {})", getCurrentTimeMillis(),
                                     GetGameTimeStamp(), GetPlayerLocation()),agentPointer);
                    
                    } else {
                        forceCombatEnd = false;
                        logger::info("TESCombatEventMighty  Actor {} {}", target->GetName(), GetCombatStateString(event));
                         HTTPManager::stream(std::format("combatendmighty|{}|{}|(Context location: {})", getCurrentTimeMillis(),
                                                    GetGameTimeStamp(), GetPlayerLocation()),
                                        agentPointer);
                    }
                    

                    

                } else {
                    if (event->newState == RE::ACTOR_COMBAT_STATE::kCombat) {  // Actor starts combat
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        bool isPlayerEnteringCombat = (target->GetFormID() == player->GetFormID());
                        
                        // Cancel all dialogue on player combat entry if enabled
                        extern bool CancelDialogueOnCombat;
                        if (CancelDialogueOnCombat && isPlayerEnteringCombat) {
                            logger::info("[COMBAT_CANCEL] Player entered combat - cancelling all dialogue");
                            SpeakManager::getInstance().deleteQueue();
                            SPGResponse::getInstance().clearAllQueues();
                            ThreadPool::getInstance().cancelTasksByType("HTTPStream");
                            ThreadPool::getInstance().cancelTasksByType("HTTPStreamRechat");
                            AudioManagerController::GetInstance().Stop();
                        }
                        
                        // Update stats when entering combat
                        AIAgentManager& aiamStats = AIAgentManager::getInstance();
                        for (const auto& agent : aiamStats.getAgents()) {
                            if (!agent || agent->getActor()) continue;   
                            if (agent->GetFormId() == target->GetFormID()) {
                                RefreshAIAgentStats(agent->getActor(), agent->getActorName());
                                RefreshAIAgentActivityStatus(agent->getActor(), agent->getActorName());
                                break;
                            }
                        }
                        
                        // Trigger ONE combat bark when PLAYER enters combat (global event, not per-NPC)
                        extern bool CombatBarksEnabled;
                        extern bool CombatDialogueEnabled;
                        if (CombatBarksEnabled && CombatDialogueEnabled && isPlayerEnteringCombat && !playerPartyCombatActive) {
                            playerPartyCombatActive = true;
                            logger::info("[COMBAT_BARK_START] Player party enters combat - picking random agent for bark");
                            
                            // Find nearby AI agents in combat to pick one for the bark
                            AIAgentManager& aiam = AIAgentManager::getInstance();
                            std::string beings = InspectManagedAgents(player->AsReference(), HERIKA_MAX_VISION_RANGE,
                                                                      ",", DISTANCE_ACTIVATING_NPC_OUT);
                            std::vector<AIAgent*> nearbyAgents;
                            
                            for (const auto& agent : aiam.getAgents()) {
                                if (agent->getActorName() == NARRATOR_NAME) continue; // Skip narrator
                                auto* actor = agent->getActor();
                                if (actor && IsActorLoadedInPlayerCell(actor) &&
                                    beings.find(agent->getActorName()) != std::string::npos) {
                                    nearbyAgents.push_back(agent.get());
                                }
                            }
                            
                            // Pick random agent for the combat start bark
                            if (!nearbyAgents.empty()) {
                                int randomIndex = rand() % nearbyAgents.size();
                                auto selectedAgent = nearbyAgents[randomIndex];
                                auto selectedActor = selectedAgent->getActor();
                                
                                logger::info("[COMBAT_BARK_START] Selected {} for combat start bark ({} nearby agents)", 
                                            selectedAgent->getActorName(), nearbyAgents.size());
                                
                                auto selectedActorHandle = selectedActor->GetHandle();
                                ThreadPool::getInstance().enqueue("CombatBarkStart", [selectedActorHandle]() {
                                    auto selectedActorRef = selectedActorHandle.get();
                                    auto* resolvedActor = selectedActorRef.get() ? selectedActorRef.get()->As<RE::Actor>() : nullptr;
                                    if (!resolvedActor || resolvedActor->IsDead() || !IsActorLoadedInPlayerCell(resolvedActor)) {
                                        logger::debug("[COMBAT_BARK_START] Skipped stale combat-start actor");
                                        return;
                                    }

                                    HTTPManager::stream(std::format("combatbark|{}|{}|{}", 
                                                                   getCurrentTimeMillis(),
                                                                   GetGameTimeStamp(), 
                                                                   GetPlayerLocation()),
                                                       resolvedActor);
                                });
                            } else {
                                logger::info("[COMBAT_BARK_START] No nearby AI agents available for combat bark");
                            }
                        }
                        
                        auto targetVictim = agentPointer->GetActorRuntimeData().currentCombatTarget;
                        if (targetVictim) {
                            if (targetVictim.get()) {
                                std::string name(targetVictim.get()->GetDisplayFullName());
                                if (name.empty()) {
                                    name.assign(targetVictim.get()->GetName());
                                }
                                
                                auto activatorS = event->actor.get();
                                auto victim = targetVictim.get();
                                if (victim->GetPosition().GetDistance(activatorS->GetPosition()) <
                                    HERIKA_MAX_VISION_RANGE) {
                                    // Ignore distant combat engagements
                                    if (activatorS &&
                                        activatorS->GetFormID() != RE::PlayerCharacter::GetSingleton()->GetFormID()) {
                                        HTTPManager::log(std::format(
                                            "infoaction|{}|{}|(Context location: {}) {} engages combat with {}",
                                            getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                            activatorS->GetDisplayFullName(), name));
                                    } else {
                                        HTTPManager::log(std::format(
                                            "infoaction|{}|{}|(Context location: {}) The party engages combat with {}",
                                            getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(), name));
                                    }
                                }
                            }
                        }
                    }
                }
            
                
        }
    });

    On<RE::TESHitEvent>([](const RE::TESHitEvent* event) {
        if (IsWorldMaintenanceSuppressed()) return;

        // Update stats immediately when AI Agent is hit (for combat awareness)
        if (!event->target) return;
        
        auto targetRef = event->target.get();
        if (!targetRef || targetRef->GetFormType() != RE::FormType::ActorCharacter) return;
        
        auto targetActor = targetRef->As<RE::Actor>();
        if (!targetActor) return;
        
        // Check if this is an AI Agent
        AIAgentManager& aiam = AIAgentManager::getInstance();
        auto playerID = RE::PlayerCharacter::GetSingleton()->GetFormID();
        
        for (const auto& agent : aiam.getAgents()) {
            auto actor = agent->getActorByFormId();
            if (!actor) continue;  //     
            auto agentID = actor->GetFormID();
            if (agentID == playerID) continue; // Skip player
            
            if (agentID == targetActor->GetFormID()) {
                // AI Agent was hit - update stats (with throttling)
                auto now = std::chrono::steady_clock::now();
                auto formID = agentID;
                
                // Only update stats if it's been at least 3 seconds since last update (avoid spam)
                if (lastStatsUpdate.find(formID) != lastStatsUpdate.end()) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastStatsUpdate[formID]);
                    if (elapsed < std::chrono::seconds(3)) {
                        break; // Too soon, skip
                    }
                }
                
                lastStatsUpdate[formID] = now;
                RefreshAIAgentStats(actor, agent->getActorName());
                break;
            }
        }
    });

    On<RE::TESTopicInfoEvent>([](const RE::TESTopicInfoEvent* event) {
        if (!event) {
            return;
        }

        auto* topicForm = RE::TESForm::LookupByID(event->topicInfoID);
        RE::TESTopicInfo* source =
            topicForm && topicForm->GetFormType() == RE::FormType::Info
                ? static_cast<RE::TESTopicInfo*>(topicForm)
                : nullptr;
        
        if (source) {
            //logger::info("TESTopicInfoEvent {} {}", source->GetFormID(),event->topicInfoID);
            ;
        }
        const auto* tm = RE::MenuTopicManager::GetSingleton();
        // Check if im involved

        auto lastSpeaker = tm ? tm->speaker.get() : nullptr;
        auto* topicActor = event->speaker;

        AIAgentManager& aiam = AIAgentManager::getInstance();

         //if (event->speaker) {
        if (topicActor && event->flag) {
            auto cameraObject = RE::CrosshairPickData::GetSingleton()->target;
            // DISABLED
            if (cameraObject && false ) {   // Will do the audio search in the other condition branch
                if (cameraObject.get()->GetFormType() == RE::FormType::ActorCharacter) {
                    auto targetActor = cameraObject.get()->As<RE::Actor>();
                    if (targetActor == topicActor) {
                        if (!source) {
                            logger::warn("[VOICE] Topic {:08X} could not be resolved for {}", event->topicInfoID,
                                         topicActor->GetDisplayFullName());
                            return;
                        }
                        auto debugMe = source->GetDialogueData(topicActor);
                        logger::info("Trying to recover audio files...");
                        
                        if (debugMe.responses.empty()) {
                            logger::info("No responses found in dialogue data 1");
                        }
                        else {
                            auto response = debugMe.responses.front();
                            std::string textSample(response->text);
                            std::string audioFileDetected(response->voice);

                            replaceAll(audioFileDetected, "Data\\", "");

                            replaceAll(audioFileDetected, ".wav", ".fuz");
                            RE::BSResourceNiBinaryStream finaudioFileDetected(audioFileDetected);
                            if (finaudioFileDetected.good()) {
                                AudioFilesBufferManager::addAudioFile(targetActor, audioFileDetected,
                                                                    strlen(textSample.c_str()));
                            }

                            replaceAll(audioFileDetected, ".fuz", ".xwm");
                            RE::BSResourceNiBinaryStream finaudioFileDetected2(audioFileDetected);
                            if (finaudioFileDetected2.good()) {
                                AudioFilesBufferManager::addAudioFile(targetActor, audioFileDetected,
                                                                    strlen(textSample.c_str()));
                            }
                            logger::info("Added audiofile for {} length:{}, queue size: {}", targetActor->GetDisplayFullName(),
                                strlen(textSample.c_str()),AudioFilesBufferManager::audioFilesBuffer.size());
                        }
                        

                        controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();
                        
                        AIAgentManager& aiam = AIAgentManager::getInstance();
                        auto agentPtr = aiam.getAgentByName(topicActor->GetDisplayFullName());
                        /* if (agentPtr)
                            agentPtr->setAvailable(false);*/

                    }
                }
            } else {
                if (topicActor && false) {  // Not doing anything useful here
                    auto player = RE::PlayerCharacter::GetSingleton();
                    float minDistance = MIN_DISTANCE;
                    AIAgentManager& aiam = AIAgentManager::getInstance();
                    auto agentPtr = aiam.getAgentByName(topicActor->GetDisplayFullName());

                    if (agentPtr && false) {
                        // We could rechat standard dialogue lines. But that 'argghs' in combat is also a dialogue.
                        // Should  guess what type of comment is . COmbat arrgs should not be rechatted
                        auto npc = agentPtr->getActor();
                        // RECHAT FROM STANDARD COMMENT
                        std::vector<std::shared_ptr<AIAgent>> agents =
                            aiam.getAgents();  // Assuming Agent is the type returned by aiam.getAgents()
                        std::random_device rd;
                        std::default_random_engine rng(rd());
                        std::shuffle(agents.begin(), agents.end(), rng);

                        for (const auto& agent : agents) {
                            if (agentPtr == agent) continue;  // Don't choose the same guy

                            if (!agent->isAvailableforDialog(false)) continue;

                            float distance = player->GetPosition().GetDistance(agent->getActor()->GetPosition());
                            if (distance < minDistance && (distance > 1)) {
                                if (agent->getActor()->GetParentCell() ==
                                    RE::PlayerCharacter::GetSingleton()->GetParentCell()) {
                                    if (npc->GetFormID() != agent->getActor()->GetFormID()) {
                                        if (agent->isAvailableforAnimation()) {
                                            // agent->getActor()->EndDialogue(); //1.0.6
                                            agent->getActor()->AllowPCDialogue(false);

                                            if (agent->isAvailableforAnimation()) {
                                                auto callback =
                                                    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                                auto args = RE::MakeFunctionArguments(std::move(agent->getActor()),
                                                                                      std::move(npc));
                                                RE::BSScript::Internal::VirtualMachine::GetSingleton()
                                                    ->DispatchStaticCall("AIAgentAIMind", "LookAt", args, callback);
                                                agent->setClean(false);
                                                agent->setRestored(false);
                                            }
                                        }

                                        logger::info("Rechat {}, distance {}", agent->getActor()->GetDisplayFullName(),
                                                     distance);
                                        HTTPManager::stream(
                                            std::format("{}|{}|{}|{} {} has something to say", "rechat",
                                                        getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                                        agent->getActor()->GetDisplayFullName()),
                                            agent->getActor());
                                        break;
                                    }
                                }
                            }
                        }
                    }
                } else {
                }
            }
        } else  if (topicActor && !event->flag) {
            if (topicActor) {
                std::string actorName(topicActor->GetDisplayFullName());
                auto agentPtr = aiam.getAgentByName(actorName);

                if (agentPtr) {
                    // agentPtr->setAvailable(false);

                    /* logger::info("Topic launched from {}, flag {},{}", agentPtr->getActorName(), event->flag,
                                 event->topicInfoID);*/
                }

                auto cameraObject = RE::CrosshairPickData::GetSingleton()->target;
                if (!cameraObject) {
                    // logger::info("Checking NPC via grabbed ref");
                    // logger::info("TESTopicInfoEvent {} {}", source->GetFormID(), event->topicInfoID);

                    auto targetObjectRef = RE::PlayerCharacter::GetSingleton()->GetGrabbedRef();
                    if (targetObjectRef) {
                        // logger::info("Grabbed ref is null");
                        auto cameraObject2 = targetObjectRef.get();
                        if (cameraObject2) {
                            cameraObject = cameraObject2;
                            logger::info("Checked NPC via grabbed ref {}", cameraObject.get()->GetDisplayFullName());
                        }
                    }
                }

                if (cameraObject) {
                    if (cameraObject.get()->GetFormType() == RE::FormType::ActorCharacter) {
                        auto targetActor = cameraObject.get()->As<RE::Actor>();
                        if (targetActor == topicActor) {
                            auto existingAgent = aiam.getAgentByName(targetActor->GetDisplayFullName());
                            if (existingAgent) {
                                // Agent already in framework - check if we need to capture voice sample
                                if (existingAgent->getNeedsVoiceSample() && source) {
                                    auto debugMe = source->GetDialogueData(topicActor);
                                    if (!debugMe.responses.empty()) {
                                        auto response = debugMe.responses.front();
                                        if (response) {
                                            std::string textSample(response->text);
                                            std::string audioFileDetected(response->voice);
                                            replaceAll(audioFileDetected, "Data\\", "");

                                            // Try different audio formats
                                            std::string finalAudioPath;
                                            
                                            // Try .wav first
                                            RE::BSResourceNiBinaryStream streamWav(audioFileDetected);
                                            if (streamWav.good()) {
                                                finalAudioPath = audioFileDetected;
                                            }
                                            
                                            // Try .fuz
                                            if (finalAudioPath.empty()) {
                                                std::string fuzPath = audioFileDetected;
                                                replaceAll(fuzPath, ".wav", ".fuz");
                                                RE::BSResourceNiBinaryStream streamFuz(fuzPath);
                                                if (streamFuz.good()) {
                                                    finalAudioPath = fuzPath;
                                                }
                                            }
                                            
                                            // Try .xwm
                                            if (finalAudioPath.empty()) {
                                                std::string xwmPath = audioFileDetected;
                                                replaceAll(xwmPath, ".wav", ".xwm");
                                                replaceAll(xwmPath, ".fuz", ".xwm");
                                                RE::BSResourceNiBinaryStream streamXwm(xwmPath);
                                                if (streamXwm.good()) {
                                                    finalAudioPath = xwmPath;
                                                }
                                            }

                                            if (!finalAudioPath.empty()) {
                                                std::string finalData;
                                                std::string readFailure;
                                                if (ResourceFileReader::Read(finalAudioPath, finalData, readFailure)) {
                                                    logger::info(
                                                        "[VOICE] Deferred voice sample captured for {} from dialogue: {}",
                                                        existingAgent->getActorName(), finalAudioPath);

                                                    HTTPUploader& uploader = HTTPUploader::getInstance();
                                                    uploader.UploadVoiceSampleWithText(
                                                        finalData, existingAgent->getActorName(), finalAudioPath,
                                                        textSample);

                                                    existingAgent->setNeedsVoiceSample(false);
                                                    existingAgent->setVoiceSamplePath(finalAudioPath);

                                                    // Also add to buffer for future use
                                                    AudioFilesBufferManager::addAudioFile(targetActor, finalAudioPath,
                                                                                          textSample.size());
                                                } else {
                                                    logger::warn("[VOICE] Skipping unreadable dialogue sample for {} from {} (topic {:08X}): {}",
                                                                 existingAgent->getActorName(), finalAudioPath,
                                                                 event->topicInfoID, readFailure);
                                                }
                                            }
                                        }
                                    }
                                }
                            } else if (source) {
                                // Not in framework - capture audio for potential future use
                                auto debugMe = source->GetDialogueData(topicActor);
                                logger::info("Trying to recover audio files...");
                                if (debugMe.responses.empty()) {
                                    logger::info("No responses found in dialogue data");
                                } else {
                                    auto response = debugMe.responses.front();
                                    if (!response) {
                                        logger::info("Response is null");
                                    } else {
                                        std::string textSample(response->text);
                                        std::string audioFileDetected(response->voice);

                                        replaceAll(audioFileDetected, "Data\\", "");

                                        RE::BSResourceNiBinaryStream finaudioFileDetectedW(audioFileDetected);
                                        if (finaudioFileDetectedW.good()) {
                                            AudioFilesBufferManager::addAudioFile(targetActor, audioFileDetected,
                                                                                  strlen(textSample.c_str()));
                                        }

                                        replaceAll(audioFileDetected, ".wav", ".fuz");
                                        RE::BSResourceNiBinaryStream finaudioFileDetected(audioFileDetected);
                                        if (finaudioFileDetected.good()) {
                                            AudioFilesBufferManager::addAudioFile(targetActor, audioFileDetected,
                                                                                  strlen(textSample.c_str()));
                                        }

                                        replaceAll(audioFileDetected, ".fuz", ".xwm");
                                        RE::BSResourceNiBinaryStream finaudioFileDetected2(audioFileDetected);
                                        if (finaudioFileDetected2.good()) {
                                            AudioFilesBufferManager::addAudioFile(targetActor, audioFileDetected,
                                                                                  strlen(textSample.c_str()));
                                        }
                                        logger::info("Added audiofile for {} length:{}, queue size: {}",
                                                     targetActor->GetDisplayFullName(), strlen(textSample.c_str()),
                                                     AudioFilesBufferManager::audioFilesBuffer.size());
                                    }
                                }

                                controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();
                            }
                        }
                    }
                }
            } else {
                logger::info("Unknown topic actor");
            }
        }
        
        // Find EditorID for voiceline
        

        if (!tm || !tm->currentTopicInfo || !lastSpeaker) {
            ProcedureListenToScene();
            MonitorAllSubtitlesForChatbox();  // Monitor all subtitles for chatbox
        } else {
            

            
            auto responseNode = tm->selectedResponseNode;
            if (responseNode) {

                auto responseNodeCurrent = responseNode->front();
                 if (responseNodeCurrent) {
                      RE::Actor* menuListenerActor = lastSpeaker ? lastSpeaker->As<RE::Actor>() : nullptr;
                      ProcessorMenu::RememberRecentConversationTarget(menuListenerActor);
                      QueuePlayerMenuDialoguePrefetch(tm, menuListenerActor, aiam.getPlayerName());

                      ConsumePlayerMenuSkipNextTopicPlayback();
                      if (DialogueLastStringSay.compare(responseNodeCurrent->topicText) != 0) {
                          DialogueLastStringSay.assign(responseNodeCurrent->topicText);
                          if (menuListenerActor &&
                              !IsPlayerActorName(trim(menuListenerActor->GetDisplayFullName()))) {
                              SetPendingBarterMerchant(menuListenerActor);
                          }
                          // Do not queue Player TTS from this late topic-event path. By this point vanilla dialogue has
                          // already advanced, so playback would overlap NPC dialogue and show delayed player subtitles.
                          // The supported traditional-dialogue Player TTS path is the optional SWF -> Papyrus bridge.
                         const std::string currentChatboxMode = PrismaUIBridge::GetCurrentChatboxMode();
                         const char* dialogueVerb = currentChatboxMode == "SHOUT"
                             ? "Shouting to"
                             : (currentChatboxMode == "WHISPER" ? "Whispering to" : "Talking to");

                         HTTPManager::log(std::format("chat|{}|{}|(Context location: {}){}: {} ({} {})",
                                                      getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                                      aiam.getPlayerName(),
                                                      DialogueLastStringSay, dialogueVerb, lastSpeaker->GetDisplayFullName()));

                         controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();

                         try {
                             json sData;
                             sData["listener"] = lastSpeaker->GetDisplayFullName();
                             sData["location"] = GetPlayerLocation();
                             sData["speech"] = DialogueLastStringSay;
                             sData["speaker"] = aiam.getPlayerName();
                             sData["debug"] = (event->flag) ? "true" : "false";
                             AddCachedSpeechAudience(sData, "traditional_player_speech");
                             HTTPManager::log(std::format("_speech|{}|{}|{}", getCurrentTimeMillis(),
                                                          GetGameTimeStamp(), sData.dump()));

                             // Push to chatbox in real-time
                             if (PrismaUIBridge::IsAvailable()) {
                                 char timeDateString[200];
                                 RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, false);
                                 // Use actual player character name instead of getPlayerName()
                                 std::string playerName = RE::PlayerCharacter::GetSingleton()->GetDisplayFullName();
                                 logger::info("[Chatbox] Pushing player dialogue: {} says: {}", 
                                            playerName, 
                                            DialogueLastStringSay.substr(0, 50));
                                 PrismaUIBridge::PushChatboxMessage(
                                     playerName, 
                                     DialogueLastStringSay,
                                     std::string(timeDateString),
                                     "player",
                                     "subtitle"
                                 );
                                 // Also push to conversation history panel
                                 PrismaUIBridge::PushDialogueEntry(
                                     playerName, 
                                     DialogueLastStringSay,
                                     std::string(timeDateString),
                                     "inputtext",
                                     "subtitle"
                                 );
                             } else {
                                 logger::warn("[Chatbox] Cannot push player dialogue - PrismaUI not available");
                             }

                             auto result =
                                 InspectManagedAgents(RE::PlayerCharacter::GetSingleton()->AsReference(),
                                                      HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);
                             HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(),
                                                          GetGameTimeStamp(), "(beings in range:" + result + ")"));
                         } catch (nlohmann::json_abi_v3_11_2::detail::type_error& ex) {
                             logger::info("Error sending speech. Review encoding, {}", ex.what());
                         }
                       }

                    if (event->flag) {
                        ResetPlayerMenuCustomPlaybackStateForNpcResponse();

                        auto responses = responseNodeCurrent->responses;
                        std::string fullResponse;
                        std::string audioPaths;

                        for (auto it = responses.begin(); it != responses.end(); ++it) {
                            RE::DialogueResponse* response = *it;
                            if (!response) {
                                logger::warn("[VOICE] Dialogue response was null for {} (topic {:08X})",
                                             lastSpeaker->GetDisplayFullName(), event->topicInfoID);
                                continue;
                            }

                            const std::string responseText(response->text.c_str() ? response->text.c_str() : "");
                            const std::string responseVoice(response->voice.c_str() ? response->voice.c_str() : "");
                            fullResponse.append(responseText);
                            BackGroundDialogueQueue.push_back(
                                std::string(lastSpeaker->GetDisplayFullName()).append(responseText));
                            if (!audioPaths.empty())
                                audioPaths.append(",");
                            audioPaths.append(responseVoice);


                            std::string audioFileDetected(responseVoice);

                            replaceAll(audioFileDetected, "Data\\", "");

                            replaceAll(audioFileDetected, ".wav", ".fuz");
                            RE::BSResourceNiBinaryStream finaudioFileDetected(audioFileDetected);
                            if (finaudioFileDetected.good()) {
                                AudioFilesBufferManager::addAudioFile(lastSpeaker->As<RE::Actor>(), audioFileDetected,
                                                                      responseText.size());
                            }

                            replaceAll(audioFileDetected, ".fuz", ".xwm");
                            RE::BSResourceNiBinaryStream finaudioFileDetected2(audioFileDetected);
                            if (finaudioFileDetected2.good()) {
                                AudioFilesBufferManager::addAudioFile(lastSpeaker->As<RE::Actor>(), audioFileDetected,
                                                                      responseText.size());
                            }

                            //logger::info("Added audiofile for {} lenth:{}, queue size: {}", lastSpeaker->As<RE::Actor>()->GetDisplayFullName(), strlen(response->text.c_str()),AudioFilesBufferManager::audioFilesBuffer.size());

                            // Check if this speaker is an AI agent that needs a voice sample
                            auto speakerAgent = aiam.getAgentByName(lastSpeaker->GetDisplayFullName());
                            if (speakerAgent && speakerAgent->getNeedsVoiceSample() && !responseVoice.empty()) {
                                std::string voicePath(responseVoice);
                                replaceAll(voicePath, "Data\\", "");

                                // Try to find a valid audio file format
                                std::string finalPath;
                                RE::BSResourceNiBinaryStream streamCheck(voicePath);
                                if (streamCheck.good()) {
                                    finalPath = voicePath;
                                } else {
                                    std::string fuzPath = voicePath;
                                    replaceAll(fuzPath, ".wav", ".fuz");
                                    RE::BSResourceNiBinaryStream fuzCheck(fuzPath);
                                    if (fuzCheck.good()) {
                                        finalPath = fuzPath;
                                    } else {
                                        std::string xwmPath = voicePath;
                                        replaceAll(xwmPath, ".wav", ".xwm");
                                        replaceAll(xwmPath, ".fuz", ".xwm");
                                        RE::BSResourceNiBinaryStream xwmCheck(xwmPath);
                                        if (xwmCheck.good()) {
                                            finalPath = xwmPath;
                                        }
                                    }
                                }

                                if (!finalPath.empty()) {
                                    std::string finalData;
                                    std::string readFailure;
                                    if (ResourceFileReader::Read(finalPath, finalData, readFailure)) {
                                        logger::info(
                                            "[VOICE] Deferred voice sample captured for {} from dialogue menu: {}",
                                            speakerAgent->getActorName(), finalPath);

                                        HTTPUploader& uploader = HTTPUploader::getInstance();
                                        uploader.UploadVoiceSampleWithText(finalData, speakerAgent->getActorName(),
                                                                           finalPath, responseText);

                                        speakerAgent->setNeedsVoiceSample(false);
                                        speakerAgent->setVoiceSamplePath(finalPath);
                                    } else {
                                        logger::warn("[VOICE] Skipping unreadable dialogue-menu sample for {} from {} (topic {:08X}): {}",
                                                     speakerAgent->getActorName(), finalPath, event->topicInfoID,
                                                     readFailure);
                                    }
                                }
                            }
                        }


                        if (DialogueLastStringResponse.compare(fullResponse) != 0) {
                            DialogueLastStringResponse.assign(fullResponse);




                            HTTPManager::log(std::format("chat|{}|{}|(Context location: {}){}: {}",
                                                         getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(),
                                                         lastSpeaker->GetDisplayFullName(), DialogueLastStringResponse));

                            try {
                                json sData;
                                sData["speaker"] = lastSpeaker->GetDisplayFullName();
                                sData["location"] = GetPlayerLocation();
                                sData["speech"] = DialogueLastStringResponse;
                                sData["listener"] = RE::PlayerCharacter::GetSingleton()->GetDisplayFullName();
                                sData["audios"] = audioPaths;
                                sData["debug"] = (event->flag) ? "true" : "false";
                                AddCachedSpeechAudience(sData, "traditional_npc_speech");
                                HTTPManager::log(std::format("_speech|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                             sData.dump()));

                                // Push to chatbox in real-time
                                if (PrismaUIBridge::IsAvailable()) {
                                    char timeDateString[200];
                                    RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, false);
                                    logger::info("[Chatbox] Pushing NPC dialogue: {} says: {}",
                                                 lastSpeaker->GetDisplayFullName(),
                                                 DialogueLastStringResponse.substr(0, 50));
                                    PrismaUIBridge::PushChatboxMessage(
                                        lastSpeaker->GetDisplayFullName(),
                                        DialogueLastStringResponse,
                                        std::string(timeDateString),
                                        "npc",
                                        "subtitle"
                                    );
                                    // Also push to conversation history panel
                                    PrismaUIBridge::PushDialogueEntry(
                                        lastSpeaker->GetDisplayFullName(),
                                        DialogueLastStringResponse,
                                        std::string(timeDateString),
                                        "chat",
                                        "subtitle"
                                    );
                                } else {
                                    logger::warn("[Chatbox] Cannot push NPC dialogue - PrismaUI not available");
                                }
                            } catch (nlohmann::json_abi_v3_11_2::detail::type_error& ex) {
                                logger::info("Error sending speech. Review encoding, {}", ex.what());
                            }

                        }
                    }
                         
                     
                 }
            }
            

        
        }
      
    });

    On<RE::TESEquipEvent>([](const RE::TESEquipEvent* event) {
        if (!event->actor) return;
        auto activatorS = event->actor.get();
        
        // Check if this is player equipping something
        if (activatorS->GetFormID() == RE::PlayerCharacter::GetSingleton()->GetFormID()) {
            // Schedule player equipment refresh after short delay
            int n = ThreadPool::getInstance().runningTasksByType("PlayerEquipmentUpdate");
            if (n == 0) {
                ThreadPool::getInstance().enqueue(
                    "PlayerEquipmentUpdate", []() { RefreshPlayerEquipment(); }, "player_equipment",
                    std::chrono::milliseconds(200));
            }
        }
        
        // Cell streaming fires equip events before 3D settles; periodic refresh catches the final state.
        if (!IsWorldMaintenanceSuppressed()) {
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto agent = aiam.getAgentByName(activatorS->GetDisplayFullName());
            if (agent) {
                // Schedule equipment check after short delay (let game update equipment slots)
                // Hash-based diffing handles duplicate prevention automatically
                auto actorHandle = agent->getActor()->GetHandle();
                std::string agentName = agent->getActorName();
                ThreadPool::getInstance().enqueue(
                    "EquipmentUpdate",
                    [actorHandle, agentName]() {
                        auto* npc = actorHandle.get().get();
                        if (!npc) return;
                        // RefreshAIAgentEquipment has built-in hash checking
                        // Only sends HTTP if equipment actually changed
                        RefreshAIAgentEquipment(npc, agentName);
                    },
                    agentName + "_equipment",
                    std::chrono::milliseconds(200)  // 200ms delay to let game update
                );
            }
            
        }

        if (activatorS->GetFormID() == RE::PlayerCharacter::GetSingleton()->GetFormID()) {
            RE::TESForm* object = RE::TESForm::LookupByID(event->baseObject);
            if (!object) return;

            if (object->formType == RE::FormType::Book) {
                std::string activatedName = object->GetName();
                if (RE::TESForm::LookupByID(event->originalRefr)) {
                    auto activatedRef = RE::TESForm::LookupByID(event->originalRefr)->AsReference();
                    activatedName = activatedRef ? activatedRef->GetDisplayFullName() : object->GetName();
                }
                bool bypass = false;
                HTTPManager::log(std::format("book|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), activatedName));
                logger::info("TESEquipEvent {}  {:#x}", object->GetName(), object->GetFormID());

                RE::TESForm* realObject = RE::TESForm::LookupByID(event->originalRefr);
                std::string name(object->GetName());
                if (realObject) {
                    if (name == "Generic Note" &&
                        !DynamicDiaryBook::IsPhysicalDiaryBook(object->As<RE::TESObjectBOOK>())) {
                        // AIAgent faction. is an ethereal note
                        std::string hashName = md5low(trim(realObject->AsReference()->GetDisplayFullName()),false);
                        std::string sourceFilePath = "data/textures/AIAgent/Books/" + hashName + ".png";
                        std::string destinationFilePath = "data/textures/AIAgent/Books/Note01.png";

                        std::ifstream srcFile(sourceFilePath, std::ios::binary);

                        std::ofstream destFile(destinationFilePath, std::ios::binary);

                        // Check if both files are open
                        if (srcFile.is_open() && destFile.is_open()) {
                            destFile << srcFile.rdbuf();
                        } else {
                            logger::info("Error: Could not open source or destination file {} {}", sourceFilePath,
                                         destinationFilePath);
                        }

                        // Close the files
                        srcFile.close();
                        destFile.close();
                        realObject->AsReference()->InitializeDataComponent();
                        bypass = false;
                            
                        // bookForm->InitializeData();
                    }
                }
                
                if (object && !bypass) {

                    auto bookForm = object->As<RE::TESForm>();

                    auto bookDescription = object->As<RE::TESDescription>();

                    RE::BSString description;
                    bookDescription->GetDescription(description, bookForm);
                    
                    RE::TESObjectBOOK* bookObj = object->As<RE::TESObjectBOOK>();

                    std::string title(activatedName);
                    //if (bookObj && bookObj->GetFullName()) title.assign(bookObj->GetFullName());

                    std::string finalContent("Title: ");
                    finalContent.append(title);
                    finalContent.append("\n");
                    finalContent.append(description.c_str());
                    /*
                    HTTPManager::log(
                        std::format("contentbook|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), finalContent));
                    */

                     try {
                        ThreadPool::getInstance().enqueue(
                            "HTTPUploader",
                             [finalContent, title]() {
                                try {
                                     HTTPUploader& uploader = HTTPUploader::getInstance();
                                     uploader.UploadBookContent(finalContent, title);
                                } catch (const std::exception& e) {
                                    logger::error("[HTTPUploader] Error in log thread: {}", e.what());
                                }
                            },
                            "UploadBookContent",
                            std::chrono::seconds(45));  // The server blocks while processing, so we need long timeouts.
                    } catch (const std::exception& e) {
                        logger::error("[HTTPManager] Failed to queue log task: {}", e.what());
                    }

                    logger::info("[TESEquipEvent] Book data analyzed and sent");
                }
            } else if (object->formType == RE::FormType::Shout) {
                /* HTTPManager::log(
                    std::format("shout|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), object->GetName()));
                    */
                logger::info("TEShout equiped {} ", object->GetName());

                if (object) {
                    auto bookForm = object->As<RE::TESForm>();
                    auto bookDescription = object->As<RE::TESDescription>();

                    RE::BSString description;
                    bookDescription->GetDescription(description, bookForm);

                    std::string finalContent = "Title: ";
                    finalContent.append(object->GetName());
                    finalContent.append("\n");
                    finalContent.append(description.c_str());

                }
            }
        }
    });

    On<RE::TESBookReadEvent>([](const RE::TESBookReadEvent* event) {
        // Only triggers on PC?s
        if (!event->book.get()) return;

        bool bypass = false;
        auto bookRefPtr = event->book.get();
        auto bookRef = event->book.get()->GetObjectReference();
        if (bookRef) {

            std::string fullName(bookRefPtr->GetDisplayFullName());
            auto bookForm = bookRef->As<RE::TESForm>();
            auto bookDescription = bookRef->As<RE::TESDescription>();
            std::string localName(bookForm->GetName());

            if (DynamicDiaryBook::QueueReadableBook(
                    bookRefPtr->AsReference(), bookRef->As<RE::TESObjectBOOK>(), fullName)) {
                bypass = true;
            } else if (localName=="Generic Note") {
                // AIAgent faction. is an ethereal note
                std::string hashName=md5low(trim(event->book.get()->GetDisplayFullName()),false);
                std::string sourceFilePath = "data/textures/AIAgent/Books/" + hashName+".png";
                std::string destinationFilePath = "data/textures/AIAgent/Books/Note01.png";

                std::ifstream srcFile(sourceFilePath, std::ios::binary);

                std::ofstream destFile(destinationFilePath, std::ios::binary);

                // Check if both files are open
                if (srcFile.is_open() && destFile.is_open()) {
                    destFile << srcFile.rdbuf();
                } else {
                    logger::info("Error: Could not open source or destination file {} {}", sourceFilePath,
                                 destinationFilePath);
                }

                // Close the files
                srcFile.close();
                destFile.close();

                bookRefPtr->AsReference()->InitializeDataComponent();
                bypass = false;
                //bookForm->InitializeData();
            }

            if (!bypass) {
                RE::BSString description;
                bookDescription->GetDescription(description, bookForm);

                std::string title(fullName);
                // finalContent.append(event->book.get()->GetDisplayFullName()); // This works too, probably better, but
                // we need the reference
                std::string finalContent("Title: ");

                finalContent.append(title);
                finalContent.append("\n");
                finalContent.append(description.c_str());
                /*
                HTTPManager::log(
                    std::format("contentbook|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), finalContent));
                */

                try {
                    ThreadPool::getInstance().enqueue(
                        "HTTPUploader",
                        [finalContent, title]() {
                            try {
                                HTTPUploader& uploader = HTTPUploader::getInstance();
                                uploader.UploadBookContent(finalContent, title);
                            } catch (const std::exception& e) {
                                logger::error("[HTTPUploader] Error in log thread: {}", e.what());
                            }
                        },
                        "UploadBookContent",
                        std::chrono::seconds(45));  // The server blocks while processing, so we need long timeouts.
                } catch (const std::exception& e) {
                    logger::error("[HTTPManager] Failed to queue log task: {}", e.what());
                }

                logger::info("[TESBookReadEvent] Book data analyzed and sent");
            }
        }
        

        //logger::info("TESBookReadEvent {} ", event->book.get()->GetName());
        //HTTPManager::log(std::format("book|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), event->book.get()->GetName()));

        // setPoolFast(true);
        return;
    });
    
    On<RE::TESSceneEvent>([](const RE::TESSceneEvent* event) {

        if (false) {
            RE::TESForm* scene = RE::TESForm::LookupByID(event->sceneId);
            logger::info("TESSceneEvent .Scene running, scene {:#x} {}", scene->formID,scene->GetFormEditorID());
        }

        ProcedureListenToScene();
        MonitorAllSubtitlesForChatbox();  // Monitor all subtitles for chatbox

    });


    On<RE::TESSpellCastEvent>([](const RE::TESSpellCastEvent* event) {
        
        logger::info("TESSpellCastEvent. Spell {:#x} value {}", event->spell, event->object->GetDisplayFullName());
        if (event->object) {
            if (event->object->GetFormID() == RE::PlayerCharacter::GetSingleton()->GetFormID()) {
                RE::TESForm* spell = RE::TESForm::LookupByID(event->spell);
                if (spell) {
                    if (spell->formType == RE::FormType::Spell) {
                        auto cameraObject = RE::CrosshairPickData::GetSingleton()->target;
                        if (cameraObject && cameraObject.get() &&
                            cameraObject.get()->GetFormType() == RE::FormType::ActorCharacter) {
                            auto targetActor = cameraObject.get()->As<RE::Actor>();
                            if (targetActor) {
                                HTTPManager::log(std::format("npcspellcast|{}|{}|{} casts {} on {}",
                                                             getCurrentTimeMillis(), GetGameTimeStamp(),
                                                             event->object->GetDisplayFullName(), spell->GetName(),
                                                             targetActor->GetDisplayFullName()));
                            } else {
                                HTTPManager::log(std::format("npcspellcast|{}|{}|{} casts {} ", getCurrentTimeMillis(),
                                                             GetGameTimeStamp(), event->object->GetDisplayFullName(),
                                                             spell->GetName()));
                            }
                        
                        } else {
                            HTTPManager::log(std::format("npcspellcast|{}|{}|{} casts {} ", getCurrentTimeMillis(),
                                                         GetGameTimeStamp(), event->object->GetDisplayFullName(),
                                                         spell->GetName()));
                        }
                    } else {
                        //logger::error("TESSpellCastEvent. Not a spell {:#x}", event->spell);
                    }
                } else {
                    //logger::error("TESSpellCastEvent. Spell not found for id {:#x}", event->spell);
                }

            } else {
                AIAgentManager& aiam = AIAgentManager::getInstance();
                auto agent = aiam.getAgentByName(event->object->GetDisplayFullName());

                if (agent) {
                    RE::TESForm* spell = RE::TESForm::LookupByID(event->spell);
                    if (spell) {
                        if (spell->formType == RE::FormType::Spell) {
                            HTTPManager::log(std::format("npcspellcast|{}|{}|{} casts {}", getCurrentTimeMillis(),
                                                         GetGameTimeStamp(), event->object->GetDisplayFullName(),
                                                         spell->GetName()));
                        } else {
                            //logger::error("TESSpellCastEvent. nnot a spell  {:#x}", event->spell);
                        }

                    } else {
                        //logger::error("TESSpellCastEvent. Spell not found for id {:#x}", event->spell);
                    }

                } else {
                    //logger::error("TESSpellCastEvent. Agent not found for object {}", event->object->GetDisplayFullName());
                }
            }
        } else {
            //logger::error("TESSpellCastEvent. Object is null");
        }
        
    });


    On<RE::TESSceneActionEvent>([](const RE::TESSceneActionEvent* event) {
        
        if (false) {
            RE::TESForm* scene = RE::TESForm::LookupByID(event->sceneId);

            uint32_t actionIndex;
            RE::FormID questId;
            uint32_t actorAliasId;

            logger::info(
                "TESSceneActionEvent. Scene running, scene name {}, FormId {:#x} , FEID {}, actionIndex {}, questId "
                "{:#x}",
                scene->GetName(), scene->formID, scene->GetFormEditorID(), actionIndex, questId);
        }
    });

    On<RE::TESScenePhaseEvent>([](const RE::TESScenePhaseEvent* event) {
        if (false) {
            RE::TESForm* scene = RE::TESForm::LookupByID(event->sceneFormID);
            if (scene) {
                logger::info("TESSceneActionEvent. Scene running, scene name {}, FormId {:#x} , FEID {}",
                             scene->GetName(), scene->formID, scene->GetFormEditorID());
            }
        }
    });

    On<RE::TESMagicEffectApplyEvent>([](const RE::TESMagicEffectApplyEvent* event) {
        if (!event || !event->target) return;
        
        auto target = event->target.get();
        if (!target || target->GetFormType() != RE::FormType::ActorCharacter) return;
        
        auto targetActor = target->As<RE::Actor>();
        if (!targetActor) return;
        
        // Get the magic effect
        RE::TESForm* magicEffectForm = RE::TESForm::LookupByID(event->magicEffect);
        if (!magicEffectForm) return;
        
        auto effectSetting = magicEffectForm->As<RE::EffectSetting>();
        if (!effectSetting) return;
        
        // Check if this is a reanimate effect
        bool isReanimateEffect = false;
        
        // Check by archetype (Reanimate = 18)
        if (effectSetting->data.archetype == RE::EffectArchetypes::ArchetypeID::kReanimate) {
            isReanimateEffect = true;
        }
        
        // Also check by name as a fallback
        if (!isReanimateEffect && effectSetting->GetFullName()) {
            std::string effectName = effectSetting->GetFullName();
            if (effectName.find("Reanimate") != std::string::npos || 
                effectName.find("reanimate") != std::string::npos) {
                isReanimateEffect = true;
            }
        }
        
        // Only process if this is a reanimate effect AND the target is actually dead
        // The TESMagicEffectApplyEvent fires for all actors the effect touches,
        // not just valid targets - we must verify the target is dead to prevent
        // falsely marking living NPCs as reanimated zombies
        if (isReanimateEffect && targetActor->IsDead()) {
            std::string targetName = targetActor->GetDisplayFullName();
            uint32_t targetFormID = targetActor->GetFormID();
            auto now = std::chrono::high_resolution_clock::now();
            
            // Throttle to prevent spam (event fires multiple times per cast)
            if (lastReanimateEventByTarget.find(targetFormID) != lastReanimateEventByTarget.end()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastReanimateEventByTarget[targetFormID]);
                if (elapsed < std::chrono::seconds(5)) {
                    // Too soon - skip to prevent spam
                    return;
                }
            }
            
            // Update last event time for this target
            lastReanimateEventByTarget[targetFormID] = now;
            
            // Send reanimation event to server immediately
            HTTPManager::log(std::format("npc_reanimated|{}|{}|{}",
                                       getCurrentTimeMillis(), GetGameTimeStamp(), 
                                       targetName));
            
            logger::info("Reanimation detected: {} reanimated (FormID: {:#x})", 
                        targetName, targetFormID);
        }
    });

    On<RE::TESActiveEffectApplyRemoveEvent>([](const RE::TESActiveEffectApplyRemoveEvent* event) {
        //logger::info("TESActiveEffectApplyRemoveEvent. Magic Effect {:#x} on {}", event->activeEffectUniqueID,event->target.get()->GetDisplayFullName());
    });

    /*On<RE::TESQuestStartStopEvent>([](const RE::TESQuestStartStopEvent* event) {
        if (!event) {
            logger::error("QuestStartStopEvent: Event is null");
            return;
        }
        
        RE::TESForm* qData = RE::TESForm::LookupByID(event->formID);
        if (!qData) {
            logger::error("QuestStartStopEvent: qData is null");
            return;
        }
        RE::TESQuest* qqData = qData->As<RE::TESQuest>();
        if (!qqData) {
            logger::error("QuestStartStopEvent: qqData is null");
            return;
        }
        
        if (!qqData->formEditorID) {
            logger::error("QuestStartStopEvent: formEditorID is null");
            return;
        }
        if (false) 
            logger::info("Quest started, name {} editorId {} , started {} ", qqData->GetName(), qqData->formEditorID,
                     event->started);
        
        if (event->started) {
            auto p = RE::PlayerCharacter::GetSingleton();
            
            if (qqData->formEditorID == "FlowerGirlsScene01" && false) {
                HTTPManager::log(std::format("infoaction|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "An intimate moment starts."));
                HTTPManager::log(std::format("force_current_task|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "Enjoy the intimate session. Pleasure time !"));

                HTTPManager::log(std::format("setconf|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "sexscene@on"));
                

                inSexMode = true;
                inSexDescSent = false;
                inSexLastStage = 0;
                controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();

            } else if (qqData->formEditorID == "FlowerGirlsPostSex" && false) {
                HTTPManager::log(std::format("infoaction|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "An intimate moment ends."));
                HTTPManager::log(std::format("recover_last_task|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "Enjoy the intimate session. Pleasure time !"));

                
                inSexMode = false;
                inSexDescSent = false;
                inSexLastStage = 0;
                HTTPManager::log(
                    std::format("setconf|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), "sexscene@off"));
            
            } else if (qqData->formEditorID == "WINewVoicePower01") {
                HTTPManager::log(std::format("infoaction|{}|{}|{} {}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             GetPlayerName(),
                                             " is surrounded by a mysterious aura, and hears and learns a Word of Power."));
                // Relenting force

            }
        }
    });*/
    
    On<RE::TESQuestStageEvent>([](const RE::TESQuestStageEvent* event) {
        // Quest obtained
        RE::TESForm* qData = RE::TESForm::LookupByID(event->formID);
        RE::TESQuest* qqData = qData->As<RE::TESQuest>();
        if (!qqData) return;
        PostQuestProgressionQuestStage(qqData, event->stage);
        
        if (false)
            logger::info("Quest staged, name {} editorId {} stage {} ", qqData->GetName(), qqData->formEditorID,
                         event->stage);
        
        
        bool sent = false;
        RE::BSSimpleList<RE::BGSQuestObjective*>* objetives = &qqData->objectives;
        RE::BGSQuestObjective* lastObjective = nullptr;
        
        for (auto iter = objetives->begin(); iter != objetives->end(); ++iter) {
            lastObjective = *iter;
            sent = false;
            RE::BGSQuestObjective* stage = *iter;
            if (stage->index != event->stage) {
                continue;
            }
            if (stage->index == event->stage) {
                RE::TESQuestTarget** targets = stage->targets;


                RE::BSString bsString(stage->displayText);

                ReplaceTagsInQuestText(&bsString, qqData, qqData->currentInstanceID);
                auto stateX = stage->state.get();
                
                HTTPManager::log(std::format("_uquest|{}|{}|{}@{}@{}@{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             qqData->GetFormEditorID(), qqData->GetName(),
                                             lastObjective->displayText.c_str(), stage->index));

                HTTPManager::log(std::format(
                    "quest|{}|{}|(Context location: {}) Quest Updated \"{}\" new objetive: {} ", getCurrentTimeMillis(),
                    GetGameTimeStamp(), GetPlayerLocation(), qqData->GetName(), lastObjective->displayText.c_str()));
                
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                auto args = RE::MakeFunctionArguments(std::move(qqData->GetFormID()));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "FillLogJournal", args, callback);

                sent = true;
                
            }
        }

        if (lastObjective && !sent) {
            //if (lastObjective->state.get() == RE::QUEST_OBJECTIVE_STATE::) {
                RE::TESQuestTarget** targets = lastObjective->targets;

                RE::BSString bsString(lastObjective->displayText);
                

                ReplaceTagsInQuestText(&bsString, qqData, qqData->currentInstanceID);
                HTTPManager::log(std::format(
                    "_uquest|{}|{}|{}@{}@{}@{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             qqData->GetFormEditorID(), qqData->GetName(),
                                             lastObjective->displayText.c_str(), event->stage));

                

            //}
        }

        
    });
    
    On<RE::TESEnterBleedoutEvent>([](const RE::TESEnterBleedoutEvent* event) {
        if (!event->actor) return;

        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - controlLastBleedOutTriggerTS);
        
        controlLastBleedOutTriggerTS = now;

        auto activatorS = event->actor.get();

        if (activatorS) {
            if (activatorS->formType == RE::FormType::ActorCharacter) {
                RE::Actor* target = activatorS->As<RE::Actor>();
                std::string name(target->GetDisplayFullName());

                if (elapsed >= std::chrono::seconds(30)) {
                    HTTPManager::log(
                        std::format("bleedout|{}|{}|(Context location: {}){} falls to the ground almost unconscious",
                                    getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(), name));
                }

                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                auto args = RE::MakeFunctionArguments(std::move(target));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "RecoverFromCombat", args, callback);
            }

            
        }
    });

    On<RE::TESSleepStopEvent>([](const RE::TESSleepStopEvent* event) {
        if (!event->interrupted) {
            logger::info("TESSleepStopEvent ");
            char timeDateString[200];
            RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, true);
            
            HTTPManager::stream(std::format("goodmorning|{}|{}|(Context location: {})(In-game date: {})", getCurrentTimeMillis(),
                             GetGameTimeStamp(), GetPlayerLocation(), timeDateString));
        }

        for (std::string notification : pendindNotifications) {
            RE::DebugNotification(notification.c_str());
        }
    });

    On<RE::TESSleepStartEvent>([](const RE::TESSleepStartEvent* event) {
        char timeDateString[200];
        RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, true);
        HTTPManager::log(std::format("goodnight|{}|{}|(Context location: {})(In-game date: {}) {} goes to sleep",
                         getCurrentTimeMillis(), GetGameTimeStamp(), GetPlayerLocation(), timeDateString,
                         RE::PlayerCharacter::GetSingleton()->GetName()));
        
        // Note: Dynamic profile updates are now handled by timer system, not sleep events
    });

    On<RE::TESContainerChangedEvent>([](const RE::TESContainerChangedEvent* event_o) {
        
        if (!pluginInited) {
            logger::info("[TESContainerChangedEvent] Cell loaded event bypassed");
            return;
        }
        
         const RE::TESContainerChangedEventEx* event = RE::castWrongStruct(event_o);
         auto item = event->baseObj;
         auto source = event->oldContainer;
         auto destination = event->newContainer;

         std::string itemName;
         std::string itemNameEx;

         auto itemPointer = RE::TESForm::LookupByID(item);
         if (itemPointer && event->itemCount > 0) {
             auto* player = RE::PlayerCharacter::GetSingleton();
             const RE::FormID playerFormID = player ? player->GetFormID() : 0;
             const std::string questItemKey = BuildQuestProgressionFormKey(itemPointer);
             if (destination == playerFormID) {
                 int forwardedCount = event->itemCount;
                 {
                     std::lock_guard<std::mutex> lock(g_questProgressionMutex);
                     forwardedCount -= ConsumeSuppressedInventoryDelta(
                         g_questProgressionSuppressedAdds, questItemKey, forwardedCount);
                 }
                 if (forwardedCount > 0) {
                     PostQuestProgressionInventoryDelta("item_acquired", itemPointer, forwardedCount);
                 }
             }
             if (source == playerFormID) {
                 int forwardedCount = event->itemCount;
                 {
                     std::lock_guard<std::mutex> lock(g_questProgressionMutex);
                     forwardedCount -= ConsumeSuppressedInventoryDelta(
                         g_questProgressionSuppressedRemovals, questItemKey, forwardedCount);
                 }
                 if (forwardedCount > 0) {
                     PostQuestProgressionInventoryDelta("item_removed", itemPointer, forwardedCount);
                 }
             }
         }

         int transferValue = 0;
         if (itemPointer) {
             if (itemPointer->GetName() && itemPointer->GetName()[0]) {
                 itemName.append(itemPointer->GetName());
             }

             
             auto reference = RE::TESForm::LookupByID(event->reference);
             
             if (reference) {
                 auto ref = reference->AsReference();
                 if (ref) {
                     itemNameEx.append(ref->GetDisplayFullName());
                     logger::info("[TESContainerChangedEvent] <{}> vs <{}>", ref->GetDisplayFullName(), ref->GetName());
                 }
             }

             if (itemNameEx.size() > 0) 
                 itemName.assign(itemNameEx);

             transferValue += itemPointer->GetGoldValue() * event->itemCount;
         }
         
         auto destinationPointer = RE::TESForm::LookupByID(destination);
         const bool suppressPlayerItemLoggingForBarter =
             ProcessorMenu::RecordBarterTransfer(
                 source,
                 destination,
                 static_cast<int32_t>(event->itemCount),
                 itemPointer,
                 itemName);

         if (destination == RE::PlayerCharacter::GetSingleton()->GetFormID() && itemPointer && event->itemCount > 0) {
             PostPlayerItemAcquiredTelemetry(
                 itemPointer,
                 itemName,
                 event->itemCount,
                 source,
                 suppressPlayerItemLoggingForBarter,
                 ProcessorMenu::IsCraftingActive());
         }

         if (!suppressPlayerItemLoggingForBarter &&
            destinationPointer && destinationPointer->GetFormType() == RE::FormType::ActorCharacter && 
           source ==  RE::PlayerCharacter::GetSingleton()->GetFormID() ) {
             
             AIAgentManager& aiam = AIAgentManager::getInstance();
             auto agent = aiam.getAgentByFormId(destinationPointer->GetFormID());
             if (agent) {
                 if (agent->getActor()->GetFormID() == destinationPointer->GetFormID()) {
                     HTTPManager::log(std::format("itemfound|{}|{}|{} gave {} {} to {},(value {} gold)",
                                                  getCurrentTimeMillis(), GetGameTimeStamp(),
                                                  RE::PlayerCharacter::GetSingleton()->GetName(), event->itemCount,
                                                  itemName, agent->getActorName(), transferValue));
                 }
             }
             
         }
         
         if (false) return;

        // Track inventory changes for AI Agents
        AIAgentManager& aiam = AIAgentManager::getInstance();
        RE::Actor* affectedAgent = nullptr;
        std::string agentName;

        auto sourceAgentPtr = aiam.getAgentByFormId(source);     // Check if source is an AI Agent
        auto destAgentPtr = aiam.getAgentByFormId(destination);  // Check if destination is an AI Agent
        // Detect item consumption: destination == 0 means item was consumed/destroyed
        auto playerID = RE::PlayerCharacter::GetSingleton()->GetFormID();
        if (!sourceAgentPtr && !destAgentPtr && source != playerID && destination != playerID) {
            return;
        }

        const bool worldMaintenanceSuppressed = IsWorldMaintenanceSuppressed();
        if (worldMaintenanceSuppressed && source != playerID && destination != playerID) {
            return;
        }

        if (destination == 0 && source != 0 && source != playerID) {
            if (sourceAgentPtr && !sourceAgentPtr->isNarrator()) {
                auto actor = sourceAgentPtr->getActorByFormId();
                if (actor) {
                    RefreshAIAgentInventory(actor, sourceAgentPtr->getActorName(), false, false);
                    RefreshAIAgentEquipment(actor, sourceAgentPtr->getActorName());
                    RefreshAIAgentSpells(actor, sourceAgentPtr->getActorName());
                }
            }
        }

        // Check if source or destination is an AI Agent (exclude player)
        // Also detect NPC-to-NPC transfers for itemtransfer logging
         
         RE::Actor* sourceAgent = nullptr;
         RE::Actor* destAgent = nullptr;
         std::string sourceAgentName;
         std::string destAgentName;
         

        if (sourceAgentPtr) {
             if (!sourceAgentPtr->isNarrator()) {
                 sourceAgent = sourceAgentPtr->getActorByFormId();
                 sourceAgentName = sourceAgentPtr->getActorName();
                 affectedAgent = sourceAgentPtr->getActorByFormId();
                 agentName = sourceAgentPtr->getActorName();
             }
         }

         if (destAgentPtr) {
             if (!destAgentPtr->isNarrator()) {
                 destAgent = destAgentPtr->getActorByFormId();
                 destAgentName = destAgentPtr->getActorName();
                 if (!affectedAgent) {
                     affectedAgent = destAgentPtr->getActorByFormId();
                     agentName = destAgentPtr->getActorName();
                 }
             }
             
         }

         
         // Detect NPC-to-NPC transfer and log to server
         if (sourceAgent && destAgent && source != 0 && destination != 0) {
             // This is a transfer between two tracked NPCs
             logger::info("[NPC_TRANSFER] {} gave {} {} to {}", sourceAgentName, event->itemCount, itemName, destAgentName);
             
             // Log to server for inventory sync
             HTTPManager::log(std::format("itemtransfer|{}|{}|{} gave {} {} to {}", 
                                          getCurrentTimeMillis(),
                                          GetGameTimeStamp(),
                                          sourceAgentName,
                                          event->itemCount,
                                          itemName,
                                          destAgentName));
         }
         
        if (affectedAgent && !worldMaintenanceSuppressed) {
            // Refresh inventory, equipment, and spells
            // Hash-based diffing handles duplicate prevention automatically
            // Force spell update on inventory change (spell tomes might have been used)
            RefreshAIAgentInventory(affectedAgent, agentName, false, false);
            RefreshAIAgentEquipment(affectedAgent, agentName);
            RefreshAIAgentSpells(affectedAgent, agentName, true);
        }
         
        if (source == playerID || destination == playerID) {
            // Player's inventory changed - refresh inventory and spells
            // Only enqueue if not already running to prevent duplicate updates
            int runningTasks = ThreadPool::getInstance().runningTasksByType("PlayerInventoryUpdate");
            
            if (runningTasks == 0) {
                ThreadPool::getInstance().enqueue(
                    "PlayerInventoryUpdate",
                    []() {
                        RefreshPlayerInventory();
                        RefreshPlayerSpells();
                    },
                    "player_inventory");
            } else {
                logger::debug("Avoided player inventory as task PlayerInventoryUpdate already running");
            }
        }
    });

    On<RE::TESTrapHitEvent>([](const RE::TESTrapHitEvent* event) {
         if (event->flag >= 1 && false) {
             AIAgentManager& aiam = AIAgentManager::getInstance();
             for (const auto& agent : aiam.getAgents()) {
                 // AI AGent chat
                 if (agent->getActor()->GetFormID() == event->target->GetFormID()) {
                     /* HTTPManager::log(std::format("infoaction|{}|{}|{} gave {} {} to {} ", getCurrentTimeMillis(),
                                                  GetGameTimeStamp(), agent->getActor()->GetDisplayFullName()));*/

                     logger::info("TESTrapHitEvent {:#x},{:#x} actor:{} ", event->flag, event->target->GetFormID(),
                                  agent->getActor()->GetDisplayFullName());

                     break;
                 }
             }
         }

     });
     
    /*
    On<RE::TESFastTravelEndEvent>([](const RE::TESFastTravelEndEvent* event) {
            HTTPManager::log(std::format("infoaction|{}|{}|The party travels for a {} hours to {} ", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     RE::PlayerCharacter::GetSingleton()->GetName(), event->fastTravelEndHours,GetPlayerLocation()));
            
        });

    */
    On<RE::TESGrabReleaseEvent>([](const RE::TESGrabReleaseEvent* event) {
        VRItemAwareness::HandleFlatGrabReleaseEvent(event);
        if (!event) {
            return;
        }
        auto itemGrabbed = event->ref;

        if (itemGrabbed) {
            logger::info("TESGrabReleaseEvent item:{} grabbed:{}", itemGrabbed->GetDisplayFullName(), event->grabbed);
        }
        
                                 
        
    });

    On<RE::TESWaitStartEvent>([](const RE::TESWaitStartEvent* event) {
        SpeakManager::getInstance().deleteQueue();
        if (SpeakManager::getInstance().getProcessing()) 
            SpeakManager::getInstance().abortPlay();  // Stop NPCs talking

         SPGResponse::getInstance().clearAllQueues();  // To avoid trigger bored event from now


         HTTPManager::log(std::format("waitstart|{}|{}|(Context location: {}) {} waits (roleplay fast-forwards and enters in time-lapse)", getCurrentTimeMillis(),
                                      GetGameTimeStamp(), GetPlayerLocation(),
                                      RE::PlayerCharacter::GetSingleton()->GetDisplayFullName()));
        
    });


    On<RE::TESWaitStopEvent>([](const RE::TESWaitStopEvent* event) {
        
        HTTPManager::log(std::format("waitstop|{}|{}|(Context location: {}) {} stops waiting (time has advanced quickly, check new hour) ", getCurrentTimeMillis(),
                                     GetGameTimeStamp(), GetPlayerLocation(),
                                     RE::PlayerCharacter::GetSingleton()->GetDisplayFullName()));

    });

    On<RE::TESSwitchRaceCompleteEvent>([](const RE::TESSwitchRaceCompleteEvent* event) {
        if (event->subject.get()) {
            auto subject = event->subject.get();
            auto actor=subject->As<RE::Actor>();
            if (actor) {
                std::string newRace = actor->GetRace()->GetFullName();

                logger::info("Switch race event {}, new race ", event->subject.get()->GetDisplayFullName(), newRace);
                HTTPManager::log(std::format("switchrace|{}|{}|{} transforms into {}", getCurrentTimeMillis(),
                                             GetGameTimeStamp(), event->subject.get()->GetDisplayFullName(), newRace));

                if (actor->IsPlayer()) {
                    RefreshPlayerTransformationState(true);
                } else {
                    AIAgentManager& aiam = AIAgentManager::getInstance();
                    const std::string agentName = aiam.getRenamedNpcNameByFormId(actor->GetFormID());
                    if (!agentName.empty()) {
                        RefreshAIAgentTransformationState(actor, agentName);
                    }
                }
            }
        }
    });

     
    On<RE::TESPackageEvent>([](const RE::TESPackageEvent* event) {
        //    kStart = 0,kChange = 1,kEnd = 2
        AIAgentManager& aiam = AIAgentManager::getInstance();

        auto actorPtr = event->actor.get();
        if (!actorPtr) return;
        auto actor = actorPtr->As<RE::Actor>();
         if (actor ) {
             if (!aiam.getRenamedNpcNameByFormId(actor->GetFormID()).empty()  ) {
                 bool is3DLoaded = actor->Is3DLoaded();
                 bool inHigh = actor->GetActorRuntimeData().currentProcess->InHighProcess();
                 if (!is3DLoaded || !inHigh ||
                     true) {  // Let server decide if we must attend to this event or not, we will send it anyway
                     auto package = RE::TESForm::LookupByID(event->package);
                     auto package2 = static_cast<RE::TESPackage*>(RE::TESForm::LookupByID(event->package));
                     auto location = actor->GetCurrentLocation();
                     std::string filename;
                     std::string locationName;
                     std::string packagename;

                     if (package->GetFile()) filename.assign(package->GetFile()->fileName);
                     if (location) locationName.assign(location->GetName());

                     std::vector<std::pair<RE::FormType, RE::FormID>> result;

                     if (const auto missingIDs = actor->extraList.GetByType<RE::ExtraLinkedRef>(); missingIDs) {
                         for (const auto& entry : missingIDs->linkedRefs) {
                             const auto form = RE::TESForm::LookupByID(entry.refr->GetFormID());
                             const auto ref = form ? form->AsReference() : nullptr;
                             if (ref) {
                                 result.push_back(std::make_pair(ref->GetFormType(), ref->GetFormID()));
                             }
                         }
                     }

                     logger::info(
                         "[TESPackageEvent] package event <{}>, actor <{}>,package <{:#x}>,pkgname <{}>, "
                         "source:<{}>,location <{}>, linkedrefs <{}> ",
                         (int)event->type, actor->GetDisplayFullName(), package->GetFormID(), packagename, filename,
                         locationName, result.size());

                     if (!aiam.getRenamedNpcNameByFormId(actor->GetFormID()).empty()) {
                         try {
                             json sData;
                             std::string status;
                             if ((int)event->type == 0)
                                 status = "start";
                             else if ((int)event->type == 1)
                                 status = "change";
                             else if ((int)event->type == 2)
                                 status = "end";

                             sData["event"] = status;
                             sData["actor"] = actor->GetDisplayFullName();
                             sData["refid"] = actor->GetFormID();
                             sData["pkgformid"] = package->GetFormID();
                             sData["pkgformname"] = packagename;
                             sData["source"] = filename;
                             sData["location"] = locationName;
                             sData["debug"] = result;

                             HTTPManager::log(std::format("backgroundaction|{}|{}|{}", getCurrentTimeMillis(),
                                                          GetGameTimeStamp(), sData.dump()));
                         } catch (nlohmann::json_abi_v3_11_2::detail::type_error& ex) {
                             logger::info("Error sending event. Review encoding, {}", ex.what());
                         }
                     }
                 } else {
                     std::string status;
                     if ((int)event->type == 0)
                         status = "start";
                     else if ((int)event->type == 1)
                         status = "change";
                     else if ((int)event->type == 2)
                         status = "end";
                     logger::info(
                         "[TESPackageEvent] Skipping package event for 3d loaded actor <{}>,package <{:#x}> , event {}",
                         actor->GetDisplayFullName(), event->package, status);
                 }
             } else {
                /* logger::info("[TESPackageEvent] Skipping package event for not tracked actor <{}>,package <{:#x}> ",
                              actor->GetDisplayFullName(), event->package);*/
             }
         }
        });

    
     
    On<RE::TESObjectLoadedEvent>([](const RE::TESObjectLoadedEvent* event) {
        
        auto formId = event->formID;
        if (event->loaded) {
            // logger::info("TESObjectLoadedEvent loaded {:#x} ", event->formID);
            // We will check is is a rolemastered NPC, we must restore appearance
            auto item = RE::TESForm::LookupByID<RE::TESObjectREFR>(formId);
            if (item) {
                if (item->GetFormType() != RE::FormType::ActorCharacter) {
                    return;
                }
                
                
                if (item->GetFactionOwner() == AIAgentRoleMasterFaction && AIAgentRoleMasterFaction != nullptr) {
                    RE::Actor* actor = item->As<RE::Actor>();
                    if (actor) {
                        std::thread([actor]() {
                            std::this_thread::sleep_for(std::chrono::seconds(5));

                            auto localActor = actor;
                            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                            auto args = RE::MakeFunctionArguments(std::move(localActor));
                            logger::info("AIAgentRoleMasterFaction NPC loaded {:#x} {}", localActor->GetFormID(),
                                         localActor->GetDisplayFullName());
                            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                "AIAgentAIMind", "AddDelayedNPC", args, callback);
                        }).detach();
                    }
                }
            }
        }
        
    });
    
    On<RE::TESFurnitureEvent>([](const RE::TESFurnitureEvent* event) {
        if (!event) {
            return;
        }

        auto actor = event->actor.get();
        auto furniture = event->targetFurniture.get();

        if (!actor || !furniture) {
            return;
        }

        AIAgentManager& aiam = AIAgentManager::getInstance();
        auto agent = aiam.getAgentByName(actor->GetDisplayFullName());
        if (!agent) {
            return;
        }

        const char* action = nullptr;
        auto eventType = event->type.get();
        std::string furnitureName = furniture->GetDisplayFullName();
        if (furnitureName.empty()) {
            logger::info("Furniture has no name, skipping");
        } else {
            switch (eventType) {
                case RE::TESFurnitureEvent::FurnitureEventType::kEnter:

                    logger::info("ENTER furniture {}", furnitureName);
                    RefreshAIAgentFurniture(agent->getActor(), agent->getActorName(), furnitureName);
                    break;

                case RE::TESFurnitureEvent::FurnitureEventType::kExit:
                    logger::info("EXIT furniture");
                    RefreshAIAgentFurniture(agent->getActor(), agent->getActorName(), "");
                    break;
            }
        }
        

        
    });
}
