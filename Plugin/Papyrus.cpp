#include "Papyrus.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Windows audio includes
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include "AudioManager.h"
#include "Commands.h"
#include "Conf.h"
#include "Globals.h"
#include "HTTPManager.h"
#include "HTTPUploader.h"
#include "Misc.h"
#include "SpeakManager.h"
#include "SpatialAwareness.h"
#include "SpatialSnapshotManager.h"
#include "SPGResponse.h"
#include "ThreadPool.h"
#include "Voicerec.h"
#include "PrismaUIBridge.h"
#include "ResourceFileReader.h"
#include "MusicManager.h"
#include "RE/D/DialogueMenu.h"
#include "RE/G/GFxValue.h"
#include "RE/Skyrim.h"
#include "RE/U/UI.h"
#include <SKSE/API.h>
#include <SKSE/Events.h>

// Forward declaration
extern int VoiceRecord(int bindedKey);

extern void RefreshAIAgentInventoryImpl(RE::Actor* npc, const std::string& agentName, bool forceUpdate, bool synchronous);

void SkipNextPlayerMenuTopicLocalPlayback();

#include "json.hpp"
using json = nlohmann::json;

namespace logger = SKSE::log;

namespace
{
    std::atomic<bool> g_playerMenuDialogueTtsActive{ false };
    std::atomic<std::uint64_t> g_playerMenuDialogueTtsRequestId{ 0 };
    constexpr auto kPlayerMenuDialogueTtsStartTimeout = std::chrono::seconds(7);

    std::string SanitizePlayerMenuDialogueLine(std::string line)
    {
        for (char& ch : line) {
            if (ch == '\r' || ch == '\n' || ch == '|') {
                ch = ' ';
            }
        }
        return trim(line);
    }

    std::string DecodePlayerMenuDialogueEventText(std::string line)
    {
        for (char& ch : line) {
            if (ch == '_') {
                ch = ' ';
            }
        }
        return SanitizePlayerMenuDialogueLine(std::move(line));
    }

    std::string ResolvePlayerMenuDialogueLine(const std::string& fallbackText)
    {
        std::string decodedFallback = DecodePlayerMenuDialogueEventText(fallbackText);
        if (!decodedFallback.empty()) {
            return decodedFallback;
        }

        auto* tm = RE::MenuTopicManager::GetSingleton();
        if (tm) {
            if (auto* responseNode = tm->selectedResponseNode) {
                if (auto* selectedDialogue = responseNode->front()) {
                    std::string selectedLine = selectedDialogue->topicText.c_str();
                    selectedLine = SanitizePlayerMenuDialogueLine(std::move(selectedLine));
                    if (!selectedLine.empty()) {
                        return selectedLine;
                    }
                }
            }

            if (tm->lastSelectedDialogue) {
                std::string selectedLine = tm->lastSelectedDialogue->topicText.c_str();
                selectedLine = SanitizePlayerMenuDialogueLine(std::move(selectedLine));
                if (!selectedLine.empty()) {
                    return selectedLine;
                }
            }
        }

        return decodedFallback;
    }

    void QueuePlayerMenuTopicTimerOff(std::string reason)
    {
        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::warn("[PlayerMenuTTS] Task interface unavailable; cannot release dialogue menu topic timer ({})",
                         reason);
            return;
        }

        taskInterface->AddTask([reason = std::move(reason)]() {
            auto* ui = RE::UI::GetSingleton();
            if (!ui || !ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
                return;
            }

            auto movie = ui->GetMovieView(RE::DialogueMenu::MENU_NAME);
            if (!movie) {
                logger::warn("[PlayerMenuTTS] Dialogue Menu movie unavailable; cannot release topic timer ({})",
                             reason);
                return;
            }

            RE::GFxValue args[1];
            args[0].SetString("off");
            if (!movie->Invoke("_root.DialogueMenu_mc.startTopicClickedTimer", nullptr, args, 1)) {
                logger::warn("[PlayerMenuTTS] Failed to release dialogue menu topic timer ({})", reason);
            }
        });
    }

    void QueuePlayerMenuFinishedModEvent()
    {
        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::warn("[PlayerMenuTTS] Task interface unavailable; cannot queue completion mod event");
            return;
        }

        taskInterface->AddTask([]() {
            QueuePlayerMenuTopicTimerOff("player TTS finished");

            auto* modCallbackEventSource = SKSE::GetModCallbackEventSource();
            if (!modCallbackEventSource) {
                logger::warn("[PlayerMenuTTS] Mod callback event source unavailable; cannot notify Papyrus");
                return;
            }

            SKSE::ModCallbackEvent event{
                RE::BSFixedString("AIAgent_PlayerMenuTTSFinished"),
                RE::BSFixedString(""),
                0.0f,
                nullptr
            };
            modCallbackEventSource->SendEvent(&event);
        });
    }

    bool IsPlayerMenuDialogueTtsRequestCurrent(std::uint64_t requestId)
    {
        return g_playerMenuDialogueTtsActive.load() &&
               g_playerMenuDialogueTtsRequestId.load() == requestId;
    }

    bool TryFinishPlayerMenuDialogueTtsRequest(std::uint64_t requestId)
    {
        if (g_playerMenuDialogueTtsRequestId.load() != requestId) {
            return false;
        }

        bool expected = true;
        return g_playerMenuDialogueTtsActive.compare_exchange_strong(expected, false);
    }

    std::string PlayerMenuDialogueTtsTaskKey(std::uint64_t requestId)
    {
        return "player_menu_tts_" + std::to_string(requestId);
    }

    bool IsPlayerMenuTtsPlaybackActive()
    {
        auto& speakManager = SpeakManager::getInstance();
        if (!speakManager.getProcessing()) {
            return false;
        }

        ScriptLine firstItem = speakManager.getFirstItem();
        if (firstItem.actor == "Player") {
            return true;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        return player && firstItem.actor == player->GetName();
    }

    void ReleasePlayerMenuDialogueTtsRequest(std::uint64_t requestId, const std::string& selectedLine,
                                             const char* reason, bool clearQueuedPlayerLines)
    {
        if (!TryFinishPlayerMenuDialogueTtsRequest(requestId)) {
            return;
        }

        logger::warn("[PlayerMenuTTS] Releasing held topic for '{}' ({})", selectedLine, reason);

        auto& speakManager = SpeakManager::getInstance();
        speakManager.clearPlayerPlaybackCompletedCallback();
        if (clearQueuedPlayerLines) {
            speakManager.deleteQueuedPlayerLines();
        }
        QueuePlayerMenuFinishedModEvent();
    }

    void QueuePlayerMenuDialogueTtsStartTimeout(std::uint64_t requestId, std::string selectedLine)
    {
        ThreadPool::getInstance().enqueue(
            "PlayerMenuTTSTimeout",
            [requestId, selectedLine]() {
                std::this_thread::sleep_for(kPlayerMenuDialogueTtsStartTimeout);
                if (!IsPlayerMenuDialogueTtsRequestCurrent(requestId)) {
                    return;
                }

                if (IsPlayerMenuTtsPlaybackActive()) {
                    return;
                }

                ThreadPool::getInstance().cancelTasksByKey(PlayerMenuDialogueTtsTaskKey(requestId));
                ReleasePlayerMenuDialogueTtsRequest(
                    requestId,
                    selectedLine,
                    "player TTS did not start within 7 seconds",
                    true);
            },
            "player_menu_tts",
            std::chrono::milliseconds(0));
    }

    std::string ResolvePlayerMenuListenerName()
    {
        auto* tm = RE::MenuTopicManager::GetSingleton();
        if (!tm) {
            return {};
        }

        auto lastSpeaker = tm->speaker.get();
        if (!lastSpeaker) {
            return {};
        }

        AIAgentManager& aiam = AIAgentManager::getInstance();
        for (const auto& agent : aiam.getAgents()) {
            if (agent && agent->getActor() && agent->getActor()->GetFormID() == lastSpeaker->GetFormID()) {
                return trim(agent->getActorName());
            }
        }

        return trim(lastSpeaker->GetDisplayFullName());
    }

    std::string ResolvePlayerMenuRequesterName()
    {
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            std::string displayName = trim(player->GetDisplayFullName());
            if (!displayName.empty()) {
                return displayName;
            }

            std::string playerName = trim(player->GetName());
            if (!playerName.empty()) {
                return playerName;
            }
        }

        std::string configuredPlayerName = trim(AIAgentManager::getInstance().getPlayerName());
        if (!configuredPlayerName.empty()) {
            return configuredPlayerName;
        }

        return "Player";
    }

    bool RequestPlayerMenuTtsPlayFromServer(const std::string& selectedLine, std::uint64_t requestId)
    {
        const std::string sanitizedLine = SanitizePlayerMenuDialogueLine(selectedLine);
        if (sanitizedLine.empty()) {
            return false;
        }

        const std::string payload =
            "player_menu_tts_play|" + getCurrentTimeMillis() + "|" +
            std::to_string(GetGameTimeStamp()) + "|" + ResolvePlayerMenuRequesterName() + ": " + sanitizedLine;

        const std::string listenerName = ResolvePlayerMenuListenerName();
        std::string line = HTTPManager::requestPlayerMenuTtsPlayResponse(payload, listenerName);

        if (!IsPlayerMenuDialogueTtsRequestCurrent(requestId)) {
            logger::info("[PlayerMenuTTS] Ignoring stale player_menu_tts_play response for '{}'", sanitizedLine);
            return true;
        }

        if (line.empty()) {
            logger::warn("[HTTPManager] player_menu_tts_play returned an empty response");
            return false;
        }

        SPGResponse& spgResponse = SPGResponse::getInstance();
        spgResponse.decodeAndEnqueue(line.c_str());

        const bool queuedPlayerLine = line.find("Player|ScriptQueue|") != std::string::npos;
        if (!queuedPlayerLine) {
            logger::warn("[HTTPManager] player_menu_tts_play response did not include Player ScriptQueue output");
        }

        return queuedPlayerLine;
    }
}

// Initialize the Papyrus mutex
std::mutex Papyrus::papyrusMutex;

extern void MutexSetMakeShotActive(bool newVal);
extern void MutexSetMakeShotNativeActive(bool newVal);
extern void MutexSetScreenShotSendMode(int newVal);

extern std::string globalHints;
extern bool NewActionMode;
extern int GlobalBoredEventTimeOut;
extern int GlobalDynamicProfileTimeOut;
extern int GlobalEndConversationCooldown;
extern std::chrono::high_resolution_clock::time_point controlLastBoredTriggerTS;
extern RE::TESFaction* AIAgentRoleMasterFaction;

extern float GlobalLegacyDistanceScaler;

bool GlobalAnimations = true;
bool GlobalEnable3DAudioPlayback = true;
bool GlobalInvertHeadingState = false;
bool GlobalCameraBasedAudio = false;

bool CombatDialogueEnabled = true;
bool CancelDialogueOnCombat = true;
bool CombatBarksEnabled = true;
extern int GlobalCombatBarksPeriod;
bool PreserveQueueDuringAction = false;
bool PauseDialogueWhenMenuOpen = false;
bool PlayerTtsTraditionalDialogueEnabled = false;
bool AIQuestProgressionEnabled = false;
bool AllowActorsOnScene = true;
bool GodMode = false;
bool AutoAddHostile = false;

bool AutoAddAllRaces = false;
bool AutoAddCreatureNPCs = false;

// Open Mic functionality
bool OpenMicEnabled = false;
float OpenMicSensitivity = 1000.0f;
float OpenMicEndDelay = 1.0f;
bool OpenMicMuted = false;

// Open mic monitoring thread
std::thread openMicThread;
std::atomic<bool> openMicMonitoringActive{false};
std::atomic<bool> openMicCurrentlyRecording{false};

// Helper function to trigger open mic recording start
void triggerOpenMicRecording() {
    if (OpenMicEnabled && !OpenMicMuted) {
        controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();
        RE::DebugNotification("[CHIM] recording....");
        logger::info("Open mic triggered recording");
        VoiceRecordControl::getInstance().setRecording(true);
    }
}

// Helper function to trigger open mic recording end
void triggerOpenMicRecordingEnd() {
    if (OpenMicEnabled) {
        RE::DebugNotification("[CHIM] Recording end");
        logger::info("Open mic ended recording");
        VoiceRecordControl::getInstance().setRecording(false);
        openMicCurrentlyRecording = false;
    }
}

// Open mic monitoring loop
void openMicMonitoringLoop() {
    logger::info("Open mic monitoring thread started");
    
    // Initialize audio input
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 16000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.wBitsPerSample * wfx.nChannels / 8;
    wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;

    HWAVEIN wi;
    if (waveInOpen(&wi, WAVE_MAPPER, &wfx, NULL, NULL, CALLBACK_NULL | WAVE_FORMAT_DIRECT) != MMSYSERR_NOERROR) {
        logger::error("Failed to open wave input for open mic monitoring");
        return;
    }

    const int BUFFER_SIZE = 16000 * 2 / 8; // 250ms of audio data
    char buffers[4][BUFFER_SIZE] = {};
    WAVEHDR headers[4] = {};

    // Initialize headers
    for (int i = 0; i < 4; ++i) {
        headers[i].lpData = buffers[i];
        headers[i].dwBufferLength = BUFFER_SIZE;
        waveInPrepareHeader(wi, &headers[i], sizeof(headers[i]));
        waveInAddBuffer(wi, &headers[i], sizeof(headers[i]));
    }

    waveInStart(wi);
    
    auto lastVoiceActivity = std::chrono::steady_clock::now();
    bool voiceDetected = false;
    double peakRmsSinceLastLog = 0.0;
    
    while (openMicMonitoringActive) {
        if (OpenMicMuted || !OpenMicEnabled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Debug logging every 5 seconds
        static auto lastDebugLog = std::chrono::steady_clock::now();
        static auto lastAudioLevelLog = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastDebugLog).count() >= 5) {
            logger::debug("Open mic monitoring active - enabled: {}, muted: {}, sensitivity: {}", 
                         OpenMicEnabled, OpenMicMuted, OpenMicSensitivity);
            lastDebugLog = now;
        }
        
        // Check for completed buffers
        for (auto& h : headers) {
            if (h.dwFlags & WHDR_DONE) {
                // Analyze audio level
                short* data = reinterpret_cast<short*>(h.lpData);
                size_t numSamples = h.dwBytesRecorded / sizeof(short);
                
                // Calculate RMS (Root Mean Square) for audio level
                double rms = 0.0;
                for (size_t i = 0; i < numSamples; ++i) {
                    rms += data[i] * data[i];
                }
                rms = sqrt(rms / numSamples);
                
                // Keep audio-level diagnostics useful without logging every input buffer.
                if (rms > peakRmsSinceLastLog) {
                    peakRmsSinceLastLog = rms;
                }
                if (std::chrono::duration_cast<std::chrono::seconds>(now - lastAudioLevelLog).count() >= 5) {
                    logger::debug("Open mic audio level - current: {}, peak: {}, threshold: {}",
                                 rms, peakRmsSinceLastLog, OpenMicSensitivity);
                    lastAudioLevelLog = now;
                    peakRmsSinceLastLog = 0.0;
                }

                if (rms > OpenMicSensitivity) {
                    lastVoiceActivity = std::chrono::steady_clock::now();
                    
                    if (!voiceDetected && !openMicCurrentlyRecording) {
                        // Start recording
                        logger::info("Open mic voice detected! Starting recording");
                        voiceDetected = true;
                        openMicCurrentlyRecording = true;
                        triggerOpenMicRecording();
                        
                        // Start the actual recording using existing system
                        VoiceRecord(-1); // Use -1 to indicate open mic
                    }
                } else if (voiceDetected) {
                    // Check if we've had silence for the configured delay
                    auto silenceDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - lastVoiceActivity).count();
                    
                    if (silenceDuration > (OpenMicEndDelay * 1000)) {
                        // End recording
                        voiceDetected = false;
                        triggerOpenMicRecordingEnd();
                    }
                }
                
                // Re-add buffer to queue
                waveInPrepareHeader(wi, &h, sizeof(h));
                waveInAddBuffer(wi, &h, sizeof(h));
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    // Cleanup
    waveInStop(wi);
    for (auto& h : headers) {
        waveInUnprepareHeader(wi, &h, sizeof(h));
    }
    waveInClose(wi);
    
    logger::info("Open mic monitoring thread ended");
}

extern int GlobalConfiguredTimeout;
int GlobalRechatPolicyAsap=0;

int sgmode = 0;

float DISTANCE_ACTIVATING_NPC_OUT = 2400;
float DISTANCE_ACTIVATING_NPC_IN = 1200;
bool ENABLE_AUTOADDNPC = false;

RE::BGSKeyword* AIAgentKeyWord = nullptr;

// Review this list

namespace
{
    constexpr auto kVoiceUploadBatchTimeout = std::chrono::seconds(30);

    bool VoiceUploadBatchTimedOut(const std::chrono::steady_clock::time_point& deadline)
    {
        return std::chrono::steady_clock::now() >= deadline;
    }
}

static bool sendAllVoice(const std::chrono::steady_clock::time_point& deadline, bool& timedOut) {
    std::unordered_map<std::string, std::string> skyrimVoices = {
        {"crdogvoice", "skyrim.esm\\crdogvoice\\da03_da03barbasmoreinfo0_0001cdaa_3.fuz"},
        {"crdragonpriestvoice", "skyrim.esm\\crdragonpriestvoice\\mg07__00080107_1.fuz"},
        {"crdragonvoice", "dawnguard.esm\\crdragonvoice\\dlc1vqdrag_dlc1vqdragontli_0001328f_1.fuz"},
        {"crdraugrvoice", "skyrim.esm\\crdraugrvoice\\dialoguedraugr__0001695e_1.fuz"},
        {"crdremoravoice", "skyrim.esm\\crdremoravoice\\dunmidden0_middenvelehkwea_00075c75_2.fuz"},
        {"crhagravenvoice", "skyrim.esm\\crhagravenvoice\\dunblindcl_dunblindcliffre_00077c23_1.fuz"},
        {"cruniquealduin", "skyrim.esm\\cruniquealduin\\mq206__0007a0da_1.fuz"},
        {"cruniqueodahviing", "skyrim.esm\\cruniqueodahviing\\mq301_mq301odahviingb2_00048f05_2.fuz"},
        {"cruniquepaarthurnax", "skyrim.esm\\cruniquepaarthurnax\\mq301_mq301paarthurnaxcall_000d2cf4_1.fuz"},
        {"dlc1femaleuniquefura", "dawnguard.esm\\dlc1femaleuniquefura\\dlc1rv01_dlc1rv01completet_000150e2_1.fuz"},
        {"dlc1femaleuniquevalerica",
         "dawnguard.esm\\dlc1femaleuniquevalerica\\dlc1vq05_dlc1vq05valericai_0000f84f_4.fuz"},
        {"dlc1femalevampire", "dawnguard.esm\\dlc1femalevampire\\dlc1dialoguevampire__0001983f_1.fuz"},
        {"dlc1ld_femalenorduniquekatria",
         "dawnguard.esm\\dlc1ld_femalenorduniquekatria\\dlc1ld_ark_dlc1ld_d2_katri_000163fc_1.fuz"},
        {"dlc1maleuniquedexion", "dawnguard.esm\\dlc1maleuniquedexion\\dlc1dialog_dlc1dialoguehun_0000fd6f_2.fuz"},
        {"dlc1maleuniqueflorentius",
         "dawnguard.esm\\dlc1maleuniqueflorentius\\dlc1dialog_dlc1dialoguehun_0000e7a9_1.fuz"},
        {"dlc1maleuniquegaran", "dawnguard.esm\\dlc1maleuniquegaran\\dlc1vampir_dlc1vampirebase_00018b65_1.fuz"},
        {"dlc1maleuniquegelebor", "dawnguard.esm\\dlc1maleuniquegelebor\\dlc1vq07_dlc1vq07gelebortl_00015068_2.fuz"},
        {"dlc1maleuniquegunmar", "dawnguard.esm\\dlc1maleuniquegunmar\\dlc1radian_dlc1radianttrol_0001056f_3.fuz"},
        {"dlc1maleuniqueharkon", "dawnguard.esm\\dlc1maleuniqueharkon\\dlc1vq03va_dlc1vq03vampire_000069a8_2.fuz"},
        {"dlc1maleuniqueisran", "dawnguard.esm\\dlc1maleuniqueisran\\dlc1vq03hunter__000098c7_2.fuz"},
        {"dlc1maleuniquejiub", "dawnguard.esm\\dlc1maleuniquejiub\\dlc1vqsain_dlc1vqsaintdrem_00014156_1.fuz"},
        {"dlc1maleuniquesnowelfghost",
         "dawnguard.esm\\dlc1maleuniquesnowelfghost\\dlc1vq07_dlc1vq07prelateca_00014f41_1.fuz"},
        {"dlc1maleuniquevyrthur", "dawnguard.esm\\dlc1maleuniquevyrthur\\dlc1vq07__0000d67b_2.fuz"},
        {"dlc1malevampire", "dawnguard.esm\\dlc1malevampire\\dlc1vq03va_dlc1vq03vampire_000098c4_2.fuz"},
        {"dlc1seranavoice", "dawnguard.esm\\dlc1seranavoice\\dlc1vq05_dlc1vq05relations_00014f82_1.fuz"},
        {"dlc2crgiantvoicekarstaag",
         "dragonborn.esm\\dlc2crgiantvoicekarstaag\\dlc2dunkar_dlc2dunkarstaag_00028203_1.fuz"},
        {"dlc2femaledarkelfcommoner",
         "dragonborn.esm\\dlc2femaledarkelfcommoner\\dlc2rrfavo_dlc2rrfavor06in_00024fa3_3.fuz"},
        {"dlc2femaleuniquefrea", "dragonborn.esm\\dlc2femaleuniquefrea\\dlc2dialog_dlc2dialogueska_00039211_4.fuz"},
        {"dlc2maledarkelfcommoner",
         "dragonborn.esm\\dlc2maledarkelfcommoner\\dlc2dialog_dlc2drrbeggarss_00034f84_3.fuz"},
        {"dlc2maledarkelfcynical", "dragonborn.esm\\dlc2maledarkelfcynical\\dlc2dunkol_dlc2dunkolbjorn_000275ac_3.fuz"},
        {"dlc2maleuniqueadril", "dragonborn.esm\\dlc2maleuniqueadril\\dlc2rrarri_dlc2rrarrivalsc_00039254_1.fuz"},
        {"dlc2maleuniquelleril", "dragonborn.esm\\dlc2maleuniquelleril\\dlc2dialog_dlc2drrmorvaynt_00023fd5_3.fuz"},
        {"dlc2maleuniquemiraak", "dragonborn.esm\\dlc2maleuniquemiraak\\dlc2mq02__0003a140_1.fuz"},
        {"dlc2maleuniquemodyn", "dragonborn.esm\\dlc2maleuniquemodyn\\dlc2dialog_dlc2dgcrimewant_0002c095_1.fuz"},
        {"dlc2maleuniqueneloth", "dragonborn.esm\\dlc2maleuniqueneloth\\dlc2mq04_dlc2mq04nelothnch_00019cb5_1.fuz"},
        {"dlc2maleuniquestorn", "dragonborn.esm\\dlc2maleuniquestorn\\dlc2mq05_dlc2mq05mq05storn_0001dfb3_1.fuz"},
        {"dlc2rieklingvoice", "dragonborn.esm\\dlc2rieklingvoice\\dlc2mh02_dlc2mh02chiefredg_0001fe0f_1.fuz"},
        {"femaleargonian", "skyrim.esm\\femaleargonian\\ms04_ms04avanchnzelinnkeep_00056551_1.fuz"},
        {"femalechild", "skyrim.esm\\femalechild\\dbeviction_dbnazireviction_0006f9a0_1.fuz"},
        {"femalecommander", "skyrim.esm\\femalecommander\\cw_cwcampaignfieldcomissio_000221e8_1.fuz"},
        {"femalecommoner", "skyrim.esm\\femalecommoner\\dialogueso_dialoguesolitud_000c0699_1.fuz"},
        {"femalecondescending", "skyrim.esm\\femalecondescending\\tg02b_tg02btoniliabranchto_000d33bf_1.fuz"},
        {"femalecoward", "skyrim.esm\\femalecoward\\dunsouthfr_dunboulderfallq_0003a6a7_3.fuz"},
        {"femaledarkelf", "skyrim.esm\\femaledarkelf\\da02_da02whosboethiah_0004d8be_3.fuz"},
        {"femaleelfhaughty", "skyrim.esm\\femaleelfhaughty\\dbeviction_dbnazireviction_0006f99f_1.fuz"},
        {"femaleeventoned", "skyrim.esm\\femaleeventoned\\ms01_ms01margretinfoinvisi_000d6686_2.fuz"},
        {"femalekhajiit", "skyrim.esm\\femalekhajiit\\caravanscene7__00072d27_1.fuz"},
        {"femalenord", "skyrim.esm\\femalenord\\dialogueguardsgeneral__000dd08b_1.fuz"},
        {"femaleoldgrumpy", "skyrim.esm\\femaleoldgrumpy\\dialogueriften__0008bacf_1.fuz"},
        {"femaleoldkindly", "skyrim.esm\\femaleoldkindly\\darksidecontractdialogue__0009bd39_1.fuz"},
        {"femaleorc", "skyrim.esm\\femaleorc\\dialoguetu_tutorialcombat_000dd63c_1.fuz"},
        {"femaleshrill", "skyrim.esm\\femaleshrill\\dialoguewi_dialoguewinterh_0006de34_2.fuz"},
        {"femalesultry", "skyrim.esm\\femalesultry\\dialogueso_dialoguesolitud_000c069b_2.fuz"},
        {"femaleuniqueastrid", "skyrim.esm\\femaleuniqueastrid\\darkbrotherhood__000fdbe6_1.fuz"},
        {"femaleuniqueazura", "skyrim.esm\\femaleuniqueazura\\da01_da01azurafinaltopic02_0009377e_1.fuz"},
        {"femaleuniqueboethiah", "skyrim.esm\\femaleuniqueboethiah\\da02_da02boethahslayfollow_0005fc7f_3.fuz"},
        {"femaleuniquedelphine", "skyrim.esm\\femaleuniquedelphine\\mq201_mq201delphineintrori_000410d4_1.fuz"},
        {"femaleuniqueelenwen", "skyrim.esm\\femaleuniqueelenwen\\mq302__000d94ba_1.fuz"},
        {"femaleuniqueghost", "skyrim.esm\\femaleuniqueghost\\t02_t02rukimocksplayertopi_00038281_1.fuz"},
        {"femaleuniquekarliah", "skyrim.esm\\femaleuniquekarliah\\tgdialogue_tgdialoguekarli_000d61dd_2.fuz"},
        {"femaleuniquemaven", "skyrim.esm\\femaleuniquemaven\\ms03_ms03mavenoffertopic_0001da36_1.fuz"},
        {"femaleuniquemephala", "skyrim.esm\\femaleuniquemephala\\da08__0010ebc3_1.fuz"},
        {"femaleuniquemeridia", "skyrim.esm\\femaleuniquemeridia\\da09_da09meridiadawnbreake_0004e4ca_1.fuz"},
        {"femaleuniquemirabelleervine",
         "skyrim.esm\\femaleuniquemirabelleervine\\mg06_mg06stage10mirabellee_0002757b_3.fuz"},
        {"femaleuniquenamira", "skyrim.esm\\femaleuniquenamira\\da11_da11voiceofnamiratopi_00089425_1.fuz"},
        {"femaleuniquenightmother", "skyrim.esm\\femaleuniquenightmother\\dbrecurring__000a030d_1.fuz"},
        {"femaleuniquenocturnal", "skyrim.esm\\femaleuniquenocturnal\\tg09__0001a2c8_5.fuz"},
        {"femaleuniquevaermina", "skyrim.esm\\femaleuniquevaermina\\da16miniscenes__000e155d_1.fuz"},
        {"femaleuniquevex", "skyrim.esm\\femaleuniquevex\\tgdialoguehqscene09__0003daab_1.fuz"},
        {"femaleyoungeager", "skyrim.esm\\femaleyoungeager\\db03__000c027a_3.fuz"},
        {"maleargonian", "skyrim.esm\\maleargonian\\dbeviction_dbnazireviction_0006f9a3_1.fuz"},
        {"malebandit", "skyrim.esm\\malebandit\\dunbrokenoarqst__0001d253_1.fuz"},
        {"malebrute", "skyrim.esm\\malebrute\\db07_db07playerphrasetopic_00028e27_2.fuz"},
        {"malechild", "skyrim.esm\\malechild\\dialogueri_dialogueriftenh_0008bbce_1.fuz"},
        {"malecommander", "skyrim.esm\\malecommander\\cw_cwcampaignfieldcomissio_000221e8_1.fuz"},
        {"malecommoner", "skyrim.esm\\malecommoner\\ms03_ms03louismotivationto_00074a4c_1.fuz"},
        {"malecommoneraccented", "skyrim.esm\\malecommoneraccented\\tgtq04_tgtq04torstenintrob_0007d672_3.fuz"},
        {"malecondescending", "skyrim.esm\\malecondescending\\dialogueda_dialoguedawnsta_00090e03_1.fuz"},
        {"malecoward", "skyrim.esm\\malecoward\\da15_da15queststart_0002bd02_2.fuz"},
        {"maledarkelf", "skyrim.esm\\maledarkelf\\dialoguefo_hirelingidles_000e1692_1.fuz"},
        {"maledrunk", "skyrim.esm\\maledrunk\\mq201party__000c0829_1.fuz"},
        {"maleelfhaughty", "dawnguard.esm\\maleelfhaughty\\dlc1rv08_dlc1rv08start3_0000ce01_2.fuz"},
        {"maleeventoned", "skyrim.esm\\maleeventoned\\dbrecurrin_dbrecurringcont_00087b80_1.fuz"},
        {"maleeventonedaccented", "skyrim.esm\\maleeventonedaccented\\db05_db05goodtimebranchtop_000a7041_1.fuz"},
        {"maleforsworn", "skyrim.esm\\maleforsworn\\duneldergl_duneldergleamt0_0001fb79_1.fuz"},
        {"maleguard", "skyrim.esm\\maleguard\\dialogueguardsgeneral__000dd08b_1.fuz"},
        {"malekhajiit", "skyrim.esm\\malekhajiit\\wemaiqthel_wemaiqtheliarhe_000b9df0_1.fuz"},
        {"malenord", "skyrim.esm\\malenord\\darkbrothe_nightgatehadrin_000da678_3.fuz"},
        {"malenordcommander", "skyrim.esm\\malenordcommander\\dialogueguardsgeneral__000dd0f5_1.fuz"},
        {"maleoldgrumpy", "skyrim.esm\\maleoldgrumpy\\darkbrotherhood__0006a3dc_1.fuz"},
        {"maleoldkindly", "skyrim.esm\\maleoldkindly\\dialoguema__000253b1_1.fuz"},
        {"maleorc", "dawnguard.esm\\maleorc\\dlc1vqfvbo_dlc1vqfvbooksbr_0001a3df_2.fuz"},
        {"maleslycynical", "skyrim.esm\\maleslycynical\\ms05_ms05viarmotask2_000534e7_2.fuz"},
        {"malesoldier", "skyrim.esm\\malesoldier\\db01misc_db01miscguardgree_000556f5_1.fuz"},
        {"maleuniqueamaundmotierre", "skyrim.esm\\maleuniqueamaundmotierre\\db04_amaundcontractinfobra_0003bce8_1.fuz"},
        {"maleuniqueancano", "skyrim.esm\\maleuniqueancano\\mg03_mg03ancanoforcegreetr_00107ab4_1.fuz"},
        {"maleuniquearngeir", "skyrim.esm\\maleuniquearngeir\\mq00_mqarngeirdragonbornto_00030317_6.fuz"},
        {"maleuniqueaventusaretino", "skyrim.esm\\maleuniqueaventusaretino\\db01_db01aventusgrelodalre_00051400_4.fuz"},
        {"maleuniquebrynjolf", "skyrim.esm\\maleuniquebrynjolf\\tg01_tg01brynjolfduringque_000206a8_1.fuz"},
        {"maleuniquecicero", "skyrim.esm\\maleuniquecicero\\darkbrothe_dbcicerostatewa_0009be4a_1.fuz"},
        {"maleuniqueclavicusvile", "skyrim.esm\\maleuniqueclavicusvile\\da03_da03vilegreet1c_000bd765_1.fuz"},
        {"maleuniquedbblackdoor", "skyrim.esm\\maleuniquedbblackdoor\\darkbrothe_dbblackdoordawn_00019512_1.fuz"},
        {"maleuniquedbguardian", "skyrim.esm\\maleuniquedbguardian\\darkbrotherhood__00074748_1.fuz"},
        {"maleuniquedbspectrallachance", "skyrim.esm\\maleuniquedbspectrallachance\\darkbrotherhood__000e0c89_1.fuz"},
        {"maleuniquedelvinmallory", "skyrim.esm\\maleuniquedelvinmallory\\darkbrothe_dbsancmalloryre_00098b68_1.fuz"},
        {"maleuniqueemperor", "skyrim.esm\\maleuniqueemperor\\db11_db11emperorplayerresp_0004fd52_3.fuz"},
        {"maleuniqueesbern", "skyrim.esm\\maleuniqueesbern\\mq00__000b0bc0_1.fuz"},
        {"maleuniquegallus", "skyrim.esm\\maleuniquegallus\\tg09_tg09galluspilgrimspat_0001a29e_2.fuz"},
        {"maleuniquegalmar", "skyrim.esm\\maleuniquegalmar\\cw02b__000a2028_1.fuz"},
        {"maleuniqueghost", "skyrim.esm\\maleuniqueghost\\dunvalthum_dunvalthumeqsth_0009a7d0_2.fuz"},
        {"maleuniqueghostsvaknir", "skyrim.esm\\maleuniqueghostsvaknir\\ms05_dunde__000e3653_1.fuz"},
        {"maleuniquehadvar", "skyrim.esm\\maleuniquehadvar\\cwsharedin_cwsharedinfosta_000e724d_1.fuz"},
        {"maleuniquehermaeusmora", "dragonborn.esm\\maleuniquehermaeusmora\\dlc2bookdungeoncontroller__00032164_1.fuz"},
        {"maleuniquehircine", "skyrim.esm\\maleuniquehircine\\da05_da05questingbeastimon_00015c64_1.fuz"},
        {"maleuniquekodlakwhitemane",
         "skyrim.esm\\maleuniquekodlakwhitemane\\c06_c06kodlakseeyalaterbra_000582d7_4.fuz"},
        {"maleuniquemalacath", "skyrim.esm\\maleuniquemalacath\\da06__000223d5_1.fuz"},
        {"maleuniquemehrunesdagon", "skyrim.esm\\maleuniquemehrunesdagon\\da07_da07dagonvoicebrancht_00097ef1_2.fuz"},
        {"maleuniquemercerfrey", "skyrim.esm\\maleuniquemercerfrey\\tg05_tg05mercerkarliahpurs_000b8387_2.fuz"},
        {"maleuniquemgaugur", "skyrim.esm\\maleuniquemgaugur\\mg04_mg04stage40augurthalm_000240b0_3.fuz"},
        {"maleuniquemolagbal", "skyrim.esm\\maleuniquemolagbal\\da10_da10molagbalstatuetop_000ddcee_2.fuz"},
        {"maleuniquenazir", "skyrim.esm\\maleuniquenazir\\db11_db11nazirbeginplayerr_000769d3_4.fuz"},
        {"maleuniqueperyite", "skyrim.esm\\maleuniqueperyite\\da13_da13peryitetoplevelto_000a8057_1.fuz"},
        {"maleuniqueseptimus", "skyrim.esm\\maleuniqueseptimus\\da04_da04septimuscubewhatt_000e4a36_3.fuz"},
        {"maleuniquesheogorath", "skyrim.esm\\maleuniquesheogorath\\da15_da15sheomeet1a_0002bcfc_6.fuz"},
        {"maleuniquetullius", "skyrim.esm\\maleuniquetullius\\cw_cwsharedinfo_000e6c7e_1.fuz"},
        {"maleuniqueulfric", "skyrim.esm\\maleuniqueulfric\\cw_cwsharedinfo_000ce099_4.fuz"},
        {"malewarlock", "skyrim.esm\\malewarlock\\dunhobsfallqst__00082fb8_1.fuz"},
        {"maleyoungeager", "skyrim.esm\\maleyoungeager\\wejs02_wejs02gourmet_000b5d3e_2.fuz"},
        {"specialfemaleuniquegormlaith", "skyrim.esm\\specialfemaleuniquegormlaith\\mq304__00096ff8_1.fuz"},
        {"specialmaleuniquefelldir", "skyrim.esm\\specialmaleuniquefelldir\\mq206__000cd9eb_2.fuz"},
        {"specialmaleuniquehakon", "skyrim.esm\\specialmaleuniquehakon\\mq305_mq305heroblockingtop_000af66c_1.fuz"},
        {"specialmaleuniquetsun", "skyrim.esm\\specialmaleuniquetsun\\mq304_mq304tsunintroa2_0004fa47_1.fuz"},
        {"femaleneivavoice",
         "Neiva_Deep_Water_Follower_1_Form.esp\\FemaleNeivaVoice\\NeivaDeepW_NeivaDeepWaterF_00000A6E_1.wav"},
        {"_kisharvoice", "Kishar.esp\\_KisharVoice\\_KisharDia__KisharDialogue_000F8FAC_1.wav"}};

    for (const auto& pair : skyrimVoices) {
        if (VoiceUploadBatchTimedOut(deadline)) {
            timedOut = true;
            break;
        }

        std::string audioData;
        std::cout << "Voice: " << pair.first << " | Path: " << pair.second << std::endl;
        std::string filename = pair.second;

        if (!filename.empty()) {
            filename[0] = std::toupper(filename[0]);  // Convert the first character to lowercase
        }
        audioData.assign("Sound\\Voice\\" + filename);

        std::string finalData;
        std::string readFailure;
        if (ResourceFileReader::Read(audioData, finalData, readFailure)) {
            if (VoiceUploadBatchTimedOut(deadline)) {
                timedOut = true;
                break;
            }

            logger::info("Uploading {} {}", pair.first, audioData);
            HTTPUploader& uploader = HTTPUploader::getInstance();
            std::string uploadResponse = uploader.UploadVoiceSample(finalData, pair.first, audioData);
            // At this point. the voice should be cloned
        } else {
            logger::warn("[VOICE] Could not read bundled sample {}: {}", audioData, readFailure);
        }
    }

    return !timedOut;
}

namespace
{
    struct VoiceSampleChoice
    {
        std::string path;
        std::size_t size = 0;
        int speechLength = 0;
    };

    std::string ToLowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::string NormalizeVoiceTypeKeyText(std::string value)
    {
        if (value.empty()) {
            return "";
        }
        value = ToLowerCopy(std::move(value));
        std::replace(value.begin(), value.end(), ' ', '_');
        return value;
    }

    bool IsVoiceAudioExtension(const std::filesystem::path& filePath)
    {
        const auto ext = ToLowerCopy(filePath.extension().string());
        return ext == ".wav" || ext == ".xwm" || ext == ".fuz";
    }

    std::string NormalizeVoiceTypeKey(RE::BGSVoiceType* voiceType)
    {
        if (!voiceType) {
            return "";
        }

        std::string key;
        const char* editorID = voiceType->GetFormEditorID();
        if (editorID && editorID[0] != '\0') {
            key.assign(editorID);
        } else {
            const char* fallbackName = voiceType->GetName();
            if (fallbackName && fallbackName[0] != '\0') {
                key.assign(fallbackName);
            }
        }

        if (key.empty()) {
            return "";
        }

        return NormalizeVoiceTypeKeyText(std::move(key));
    }

    std::string NormalizeResourcePath(std::string path)
    {
        if (path.empty()) {
            return "";
        }

        std::replace(path.begin(), path.end(), '/', '\\');

        while (!path.empty() && (path.front() == '\\' || path.front() == '.')) {
            if (path.front() == '.' && path.size() > 1 && path[1] == '\\') {
                path.erase(0, 2);
                continue;
            }
            if (path.front() == '\\') {
                path.erase(0, 1);
                continue;
            }
            break;
        }

        auto lowerPath = ToLowerCopy(path);
        constexpr std::string_view dataMarker = "data\\";
        std::size_t dataPos = lowerPath.find(dataMarker);
        if (dataPos != std::string::npos) {
            path = path.substr(dataPos + dataMarker.size());
            lowerPath = lowerPath.substr(dataPos + dataMarker.size());
        }

        constexpr std::string_view voicePrefix = "sound\\voice\\";
        std::size_t voicePos = lowerPath.find(voicePrefix);
        if (voicePos != std::string::npos && voicePos > 0) {
            path = path.substr(voicePos);
        }

        return path;
    }

    std::string ExtractVoiceTypeKeyFromVoicePath(const std::string& rawPath)
    {
        const std::string normalizedPath = NormalizeResourcePath(rawPath);
        if (normalizedPath.empty()) {
            return "";
        }

        const std::string lowerPath = ToLowerCopy(normalizedPath);
        constexpr std::string_view voicePrefix = "sound\\voice\\";
        if (!lowerPath.starts_with(voicePrefix)) {
            return "";
        }

        const std::size_t firstSep = normalizedPath.find('\\', voicePrefix.size());
        if (firstSep == std::string::npos) {
            return "";
        }

        const std::size_t secondSep = normalizedPath.find('\\', firstSep + 1);
        if (secondSep == std::string::npos || secondSep <= firstSep + 1) {
            return "";
        }

        return NormalizeVoiceTypeKeyText(normalizedPath.substr(firstSep + 1, secondSep - firstSep - 1));
    }

    std::size_t GetResourceVoiceSize(const std::string& resourcePath)
    {
        RE::BSResourceNiBinaryStream stream(resourcePath);
        if (!stream.good() || !stream.stream) {
            return 0;
        }
        return static_cast<std::size_t>(stream.stream->totalSize);
    }

    std::string NormalizeResourcePathFromDisk(const std::filesystem::path& filePath)
    {
        return NormalizeResourcePath(filePath.lexically_normal().string());
    }

    void UpsertBestVoiceSample(std::unordered_map<std::string, VoiceSampleChoice>& bestByVoiceType,
                               const std::string& voiceTypeKey, const std::string& samplePath,
                               std::size_t sampleSize, int speechLength)
    {
        if (voiceTypeKey.empty() || samplePath.empty() || sampleSize == 0) {
            return;
        }

        auto it = bestByVoiceType.find(voiceTypeKey);
        if (it == bestByVoiceType.end()) {
            bestByVoiceType.emplace(voiceTypeKey, VoiceSampleChoice{samplePath, sampleSize, speechLength});
            return;
        }

        const auto& existing = it->second;
        if (sampleSize > existing.size ||
            (sampleSize == existing.size && speechLength > existing.speechLength)) {
            it->second = VoiceSampleChoice{samplePath, sampleSize, speechLength};
        }
    }

    void LogMissedVoiceTypes(const std::unordered_map<std::string, std::string>& missedVoiceTypes)
    {
        if (missedVoiceTypes.empty()) {
            logger::info("[VOICE] Runtime longest-sample scan had no missed voice types");
            return;
        }

        std::vector<std::pair<std::string, std::string>> orderedMisses(missedVoiceTypes.begin(),
                                                                        missedVoiceTypes.end());
        std::sort(orderedMisses.begin(), orderedMisses.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        logger::warn("[VOICE] Runtime longest-sample scan missed {} voice types", orderedMisses.size());
        for (const auto& [voiceTypeKey, reason] : orderedMisses) {
            logger::warn("[VOICE] Missed voice type {} ({})", voiceTypeKey, reason);
        }
    }
}

static bool sendAllVoiceLongestSamples(const std::chrono::steady_clock::time_point& deadline, bool& timedOut)
{
    if (VoiceUploadBatchTimedOut(deadline)) {
        timedOut = true;
        return false;
    }

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        logger::warn("[VOICE] TESDataHandler not available for runtime voice sample scan");
        return false;
    }

    std::unordered_map<std::string, RE::BGSVoiceType*> voiceTypesByKey;
    auto& voiceTypes = dataHandler->GetFormArray<RE::BGSVoiceType>();
    for (auto* voiceType : voiceTypes) {
        if (!voiceType) {
            continue;
        }
        const std::string key = NormalizeVoiceTypeKey(voiceType);
        if (!key.empty() && !voiceTypesByKey.contains(key)) {
            voiceTypesByKey.emplace(key, voiceType);
        }
    }

    if (voiceTypesByKey.empty()) {
        logger::warn("[VOICE] Runtime scan found no voice types");
        return false;
    }

    logger::info("[VOICE] Runtime scan found {} loaded voice types", voiceTypesByKey.size());

    std::unordered_map<std::string, VoiceSampleChoice> bestByVoiceType;
    std::unordered_map<std::string, std::string> missedVoiceTypes;

    for (const auto& [voiceTypeKey, _] : voiceTypesByKey) {
        missedVoiceTypes.emplace(voiceTypeKey, "no candidate sample found");
    }

    // First pass: use dialogue-observed audio (works for both loose files and archives)
    for (const auto& entry : AudioFilesBufferManager::audioFilesBuffer) {
        if (VoiceUploadBatchTimedOut(deadline)) {
            timedOut = true;
            break;
        }

        if (entry.data.empty()) {
            continue;
        }

        std::string resourcePath = NormalizeResourcePath(entry.data);
        if (resourcePath.empty()) {
            resourcePath = entry.data;
        }

        std::size_t sampleSize = GetResourceVoiceSize(resourcePath);
        if (sampleSize == 0) {
            sampleSize = GetResourceVoiceSize(entry.data);
            if (sampleSize == 0) {
                continue;
            }
            resourcePath = entry.data;
        }

        std::string key = ExtractVoiceTypeKeyFromVoicePath(resourcePath);
        if ((key.empty() || !voiceTypesByKey.contains(key)) && entry.actor) {
            auto* base = entry.actor->GetActorBase();
            if (base && base->voiceType) {
                const std::string actorKey = NormalizeVoiceTypeKey(base->voiceType);
                if (!actorKey.empty()) {
                    key = actorKey;
                }
            }
        }

        if (key.empty()) {
            continue;
        }

        UpsertBestVoiceSample(bestByVoiceType, key, resourcePath, sampleSize, entry.speechLengt);
    }

    // Second pass: scan loose voice folders for each known voice type and pick the largest file.
    for (const auto& [voiceTypeKey, voiceType] : voiceTypesByKey) {
        if (VoiceUploadBatchTimedOut(deadline)) {
            timedOut = true;
            break;
        }

        if (!voiceType) {
            continue;
        }

        const char* editorID = voiceType->GetFormEditorID();
        RE::TESFile* sourceFile = voiceType->GetFile(0);
        if (!editorID || editorID[0] == '\0' || !sourceFile || !sourceFile->fileName || sourceFile->fileName[0] == '\0') {
            continue;
        }

        std::filesystem::path voiceFolder =
            std::filesystem::path("Data") / "Sound" / "Voice" / sourceFile->fileName / editorID;

        std::error_code ec;
        if (!std::filesystem::exists(voiceFolder, ec) || ec) {
            ec.clear();
            continue;
        }

        for (std::filesystem::recursive_directory_iterator it(
                 voiceFolder, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             it != end; it.increment(ec)) {
            if (VoiceUploadBatchTimedOut(deadline)) {
                timedOut = true;
                break;
            }

            if (ec) {
                ec.clear();
                continue;
            }

            if (!it->is_regular_file(ec) || ec) {
                ec.clear();
                continue;
            }

            const auto samplePath = it->path();
            if (!IsVoiceAudioExtension(samplePath)) {
                continue;
            }

            const std::string resourcePath = NormalizeResourcePathFromDisk(samplePath);
            if (resourcePath.empty()) {
                continue;
            }

            std::size_t sampleSize = GetResourceVoiceSize(resourcePath);
            if (sampleSize == 0) {
                sampleSize = static_cast<std::size_t>(std::filesystem::file_size(samplePath, ec));
                if (ec) {
                    ec.clear();
                    continue;
                }
            }

            UpsertBestVoiceSample(bestByVoiceType, voiceTypeKey, resourcePath, sampleSize, 0);
        }
    }

    if (bestByVoiceType.empty()) {
        logger::warn("[VOICE] Runtime scan found no candidate samples to upload");
        LogMissedVoiceTypes(missedVoiceTypes);
        return false;
    }

    HTTPUploader& uploader = HTTPUploader::getInstance();
    int uploadedCount = 0;

    for (const auto& [voiceTypeKey, sample] : bestByVoiceType) {
        if (VoiceUploadBatchTimedOut(deadline)) {
            timedOut = true;
            break;
        }

        if (sample.path.empty() || sample.size == 0) {
            missedVoiceTypes[voiceTypeKey] = "candidate metadata invalid";
            continue;
        }

        std::string finalData;
        std::string readFailure;
        if (!ResourceFileReader::Read(sample.path, finalData, readFailure)) {
            missedVoiceTypes[voiceTypeKey] = std::format("candidate read failed: {} ({})", sample.path, readFailure);
            continue;
        }

        logger::info("[VOICE] Uploading longest sample for {} -> {} ({} bytes)", voiceTypeKey, sample.path,
                     finalData.size());
        uploader.UploadVoiceSample(finalData, voiceTypeKey, sample.path);
        missedVoiceTypes.erase(voiceTypeKey);
        uploadedCount++;
    }

    logger::info("[VOICE] Runtime longest-sample upload complete: {} uploaded, {} voice types detected, {} missed",
                 uploadedCount, voiceTypesByKey.size(), missedVoiceTypes.size());
    LogMissedVoiceTypes(missedVoiceTypes);
    return uploadedCount > 0;
}


int setDrivenByAIReal(RE::ObjectRefHandle targetObject, bool salutation, bool warn, bool removewhenexisting, bool isManualAdd) {
    if (targetObject) {
        auto rawTarget = targetObject.get();
        // logger::info("Checking {}", rawTarget->GetFormType());

        if (targetObject.get()->GetFormType() == RE::FormType::ActorCharacter) {
            auto targetActor = targetObject.get()->As<RE::Actor>();

            if (!targetActor->IsPlayerTeammate() && false) {
                logger::info("{} is not a PlayerTeammate", targetObject.get()->GetName());

            } else if (targetActor->IsPlayer()) {
                logger::info("{} is  the player, refusing", targetObject.get()->GetName());

            } else {


                AIAgentManager& aiam = AIAgentManager::getInstance();
                std::string storedName = aiam.getRenamedNpcNameByFormId(targetActor->GetFormID());
                if (!storedName.empty()) {
                    targetActor->SetDisplayName(storedName.c_str(), true);
                }

                auto already = aiam.getAgentByName(targetActor->GetDisplayFullName());

                if (already && removewhenexisting) {
                    // Check if this is an auto-activated agent
                    if (!already->isManuallyAdded()) {
                        // Upgrade to manually-activated instead of removing
                        already->setManuallyAdded(true);
                        std::string s("[CHIM] ");
                        s.append(targetActor->GetDisplayFullName());
                        s.append(" is now active.");
                        if (warn) RE::DebugNotification(s.c_str());
                    } else {
                        // Already manually-activated, remove as before
                        std::string s("[CHIM] ");
                        s.append(targetActor->GetDisplayFullName());
                        s.append(" was already active. Removing from CHIM.");
                        targetActor->GetActorBase()->voiceType = already->getOriginalVoice();
                        aiam.deleteAgent(already);
                        if (warn) RE::DebugNotification(s.c_str());

                        auto npc = targetActor->GetDisplayFullName();
                        std::string action;
                        action.assign("remove");
                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                        auto args = RE::MakeFunctionArguments(std::move(npc), std::move(action));
                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                            "AIAgentAIMind", "SendExternalEventNPC", args, callback);
                    }

                } else {
                    std::shared_ptr<AIAgent> agent = aiam.createAgent();

                    agent.get()->setActor(targetActor);
                    agent->setAvailable(true);
                    agent->setClean(false);
                    agent->setRestored(true);
                    agent->setManuallyAdded(isManualAdd);
                    agent->setOriginalVoice(targetActor->GetActorBase()->voiceType);
                    aiam.addAgent(agent);

                    std::string category;
                    auto baseActor = agent->getActor()->GetActorBase();
                    if (baseActor) {
                        category.assign(baseActor->GetName());
                        std::string metainfo;
                        if (baseActor->GetSex()== RE::SEX::kMale) {
                            metainfo.append("@male");
                        } else if (baseActor->GetSex() == RE::SEX::kFemale) {
                            metainfo.append("@female");
                        } else  {
                            metainfo.append("@nogender");
                        }

                        std::string race;
                        if (baseActor->GetRace()->GetName()) {
                            metainfo.append("@").append(baseActor->GetRace()->GetName());
                        }

                        metainfo.append("@").append(std::format("{:08X}",agent->getActor()->GetFormID()));

                        /* Stats gathering */
                        auto stats=agent->getActor()->AsActorValueOwner();
                        
                        
                        float archery = stats->GetActorValue(RE::ActorValue::kArchery);
                        float block = stats->GetActorValue(RE::ActorValue::kBlock);
                        float onehanded = stats->GetActorValue(RE::ActorValue::kOneHanded);
                        float twohanded = stats->GetActorValue(RE::ActorValue::kTwoHanded);

                        float conjuration = stats->GetActorValue(RE::ActorValue::kConjuration);
                        float destruction = stats->GetActorValue(RE::ActorValue::kDestruction);
                        float restoration = stats->GetActorValue(RE::ActorValue::kRestoration);
                        float alteration = stats->GetActorValue(RE::ActorValue::kAlteration);
                        float illusion = stats->GetActorValue(RE::ActorValue::kIllusion);  // Use your illusion, great album

                        float heavyarmor = stats->GetActorValue(RE::ActorValue::kHeavyArmor);
                        float lightarmor = stats->GetActorValue(RE::ActorValue::kLightArmor);

                        float lockpicking = stats->GetActorValue(RE::ActorValue::kLockpicking);
                        float pickpocket = stats->GetActorValue(RE::ActorValue::kPickpocket);
                        float sneak = stats->GetActorValue(RE::ActorValue::kSneak);

                        float speech = stats->GetActorValue(RE::ActorValue::kSpeech);
                        float smithing = stats->GetActorValue(RE::ActorValue::kSmithing);
                        float alchemy = stats->GetActorValue(RE::ActorValue::kAlchemy);
                        float enchanting = stats->GetActorValue(RE::ActorValue::kEnchanting);  // enchanting? enchaaantiing
                        
                        

                        metainfo.append("@").append(std::format("{}", archery));
                        metainfo.append("@").append(std::format("{}", block));
                        metainfo.append("@").append(std::format("{}", onehanded));
                        metainfo.append("@").append(std::format("{}", twohanded));
                        metainfo.append("@").append(std::format("{}", conjuration));
                        metainfo.append("@").append(std::format("{}", destruction));
                        metainfo.append("@").append(std::format("{}", restoration));
                        metainfo.append("@").append(std::format("{}", alteration));
                        metainfo.append("@").append(std::format("{}", illusion));
                        metainfo.append("@").append(std::format("{}", heavyarmor));
                        metainfo.append("@").append(std::format("{}", lightarmor));
                        metainfo.append("@").append(std::format("{}", lockpicking));
                        metainfo.append("@").append(std::format("{}", pickpocket));
                        metainfo.append("@").append(std::format("{}", sneak));
                        metainfo.append("@").append(std::format("{}", speech));
                        metainfo.append("@").append(std::format("{}", smithing));
                        metainfo.append("@").append(std::format("{}", alchemy));
                        metainfo.append("@").append(std::format("{}", enchanting));
                        
                        /* Equipment gathering (10 slots: helmet, armor, boots, gloves, amulet, ring, cape, backpack, left hand, right hand) */
                        auto* helmet = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kHead);
                        auto* armor = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody);
                        auto* boots = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kFeet);
                        auto* gloves = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kHands);
                        auto* amulet = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kAmulet);
                        auto* ring = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kRing);
                        auto* cape = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kModChestPrimary);
                        auto* backpack = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kModBack);
                        auto* leftHand = targetActor->GetEquippedObject(true);   // Left hand
                        auto* rightHand = targetActor->GetEquippedObject(false); // Right hand

                        metainfo.append("@").append(helmet ? helmet->GetName() : "");
                        metainfo.append("@").append(armor ? armor->GetName() : "");
                        metainfo.append("@").append(boots ? boots->GetName() : "");
                        metainfo.append("@").append(gloves ? gloves->GetName() : "");
                        metainfo.append("@").append(amulet ? amulet->GetName() : "");
                        metainfo.append("@").append(ring ? ring->GetName() : "");
                        metainfo.append("@").append(cape ? cape->GetName() : "");
                        metainfo.append("@").append(backpack ? backpack->GetName() : "");
                        metainfo.append("@").append(leftHand ? leftHand->GetName() : "");
                        metainfo.append("@").append(rightHand ? rightHand->GetName() : "");

                        /* Stats gathering (core attributes) */
                        auto level = targetActor->GetLevel();
                        float health = stats->GetActorValue(RE::ActorValue::kHealth);
                        float healthMax = stats->GetBaseActorValue(RE::ActorValue::kHealth);
                        float magicka = stats->GetActorValue(RE::ActorValue::kMagicka);
                        float magickaMax = stats->GetBaseActorValue(RE::ActorValue::kMagicka);
                        float stamina = stats->GetActorValue(RE::ActorValue::kStamina);
                        float staminaMax = stats->GetBaseActorValue(RE::ActorValue::kStamina);
                        float scale = targetActor->GetScale();
                        
                        metainfo.append("@").append(std::format("{}", level));
                        metainfo.append("@").append(std::format("{}", health));
                        metainfo.append("@").append(std::format("{}", healthMax));
                        metainfo.append("@").append(std::format("{}", magicka));
                        metainfo.append("@").append(std::format("{}", magickaMax));
                        metainfo.append("@").append(std::format("{}", stamina));
                        metainfo.append("@").append(std::format("{}", staminaMax));
                        metainfo.append("@").append(std::format("{:.2f}", scale));

                        auto* modFiles = targetActor->sourceFiles.array;
                        if (!modFiles) {
                            if (targetActor->GetActorBase())
                                modFiles = targetActor->GetActorBase()->sourceFiles.array;
                        }

                        if (modFiles && modFiles->size() > 0) {
                            metainfo.append("@");
                            for (std::uint32_t i = 0; i < modFiles->size(); ++i) {
                                RE::TESFile* file = (*modFiles)[i];
                                if (file) {
                                    if (i > 0) metainfo.append("#");
                                    metainfo.append(file->fileName);
                                    
                                }
                            }
                        } else {
                            metainfo.append("@");
                        }

                        /* Faction gathering */
                        std::string factionData;
                        if (baseActor && baseActor->factions.size() > 0) {
                            for (std::uint32_t i = 0; i < baseActor->factions.size(); ++i) {
                                auto factionInfo = baseActor->factions[i];
                                if (factionInfo.faction) {
                                    if (!factionData.empty()) factionData.append("#");
                                    std::string stableFactionReference;
                                    if (auto* factionFile = factionInfo.faction->GetFile(0)) {
                                        const std::string pluginName(factionFile->GetFilename());
                                        const auto localFormId = static_cast<std::uint32_t>(factionInfo.faction->GetLocalFormID());
                                        if (!pluginName.empty()) {
                                            stableFactionReference = std::format("{}/{:08X}", pluginName, localFormId);
                                        }
                                    }
                                    // Format: formID:rank:PluginName.esp|LocalFormId
                                    factionData.append(std::format("{:08X}:{:d}:{}", 
                                        factionInfo.faction->GetFormID(), 
                                        static_cast<int>(factionInfo.rank),
                                        stableFactionReference));
                                }
                            }
                        }
                        metainfo.append("@").append(factionData);

                        /* Class gathering */
                        std::string classData;
                        if (baseActor && baseActor->npcClass) {
                            auto npcClass = baseActor->npcClass;
                            std::string className = npcClass->GetName() ? npcClass->GetName() : "";
                            
                            // Get training data from class data
                            std::string trainSkill = "";
                            int trainLevel = 0;
                            
                            // TESClass data structure contains training info
                            // Check if maximumTrainingLevel is greater than 0 to determine if this class trains
                            if (npcClass->data.maximumTrainingLevel > 0) {
                                // Convert CLASS_DATA::Skill to skill name string
                                auto skillValue = npcClass->data.teaches;
                                switch (skillValue.underlying()) {
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kOneHanded): trainSkill = "OneHanded"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kTwoHanded): trainSkill = "TwoHanded"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kArchery): trainSkill = "Archery"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kBlock): trainSkill = "Block"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kSmithing): trainSkill = "Smithing"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kHeavyArmor): trainSkill = "HeavyArmor"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kLightArmor): trainSkill = "LightArmor"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kPickpocket): trainSkill = "Pickpocket"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kLockpicking): trainSkill = "Lockpicking"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kSneak): trainSkill = "Sneak"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kAlchemy): trainSkill = "Alchemy"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kSpeech): trainSkill = "Speech"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kAlteration): trainSkill = "Alteration"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kConjuration): trainSkill = "Conjuration"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kDestruction): trainSkill = "Destruction"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kIllusion): trainSkill = "Illusion"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kRestoration): trainSkill = "Restoration"; break;
                                    case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kEnchanting): trainSkill = "Enchanting"; break;
                                    default: trainSkill = ""; break;
                                }
                                trainLevel = static_cast<int>(npcClass->data.maximumTrainingLevel);
                            }
                            
                            classData = std::format("{}:{:08X}:{}:{}", className, npcClass->GetFormID(), trainSkill, trainLevel);
                        }
                        metainfo.append("@").append(classData);

                        category.append(metainfo);
                    }

                    /* Try to generate voice*/

                    auto audiofile = AudioFilesBufferManager::findAudioFile(agent->getActor());
                    std::string finalData;
                    std::string readFailure;
                    if (ResourceFileReader::Read(audiofile, finalData, readFailure)) {
                        logger::info("Uploading voice sample {}, size {}", audiofile, finalData.size());

                        HTTPUploader& uploader = HTTPUploader::getInstance();

                        std::string uploadResponse =
                            uploader.UploadVoiceSample(finalData, agent->getActorName(), audiofile);
                        logger::info("Sending addnpc signal for {}@{}", targetActor->GetDisplayFullName(), category);
                        HTTPManager::log(std::format("addnpc|{}|{}|{}@{}", getCurrentTimeMillis(),
                                                     GetGameTimeStamp(), targetActor->GetDisplayFullName(), category));

                        // Voice sample uploaded successfully
                        agent->setNeedsVoiceSample(false);
                        agent->setVoiceSamplePath(audiofile);
                        // At this point. the voice should be cloned
                    } else {
                        logger::warn("[VOICE] Could not read voice sample for {} from {}: {}; will capture from dialogue events",
                                     targetActor->GetDisplayFullName(), audiofile, readFailure);
                        agent->setNeedsVoiceSample(true);
                        HTTPManager::log(std::format("addnpc|{}|{}|{}@{}", getCurrentTimeMillis(),
                                                     GetGameTimeStamp(), targetActor->GetDisplayFullName(),
                                                     category));
                    }

                    logger::info("{} IS NOW DRIVEN BY AI. Total AI Agents {}", targetActor->GetDisplayFullName(),
                                 aiam.getAgents().size());

                    std::string s("[CHIM] ");
                    s.append(targetActor->GetDisplayFullName());
                    s.append(" is now active.");
                    if (warn) RE::DebugNotification(s.c_str());

                    // commandAnimation("IdleDrunk", agent->getActor());

                    // targetActor->SetDialogueWithPlayer(true, false,nullptr); // Opens dialogue menu

                    auto npc = targetActor->GetDisplayFullName();
                    std::string action;
                    action.assign("add");
                    auto precallback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto preargs = RE::MakeFunctionArguments(std::move(npc), std::move(action));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "SendExternalEventNPC", preargs, precallback);

                    if (targetActor->GetFactionOwner() == AIAgentRoleMasterFaction &&
                        AIAgentRoleMasterFaction != nullptr) {
                        RE::Actor* actor = targetActor->As<RE::Actor>();
                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                        auto args = RE::MakeFunctionArguments(std::move(actor));
                        logger::info("[TESCellAttachDetachEvent] AIAgentRoleMasterFaction NPC loaded {:#x} {}",
                                     targetActor->GetFormID(), targetActor->GetDisplayFullName());
                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                            "AIAgentAIMind", "AddDelayedNPC", args, callback);
                    }

                    if (!salutation) return 0;


                    // Only runs if salutation is enabled
                    // Manually added  NPCs should be here.
                    
                    // Check if NPC is on conversation cooldown
                    if (agent->hasConversationCooldown()) {
                        logger::info("{} is on conversation cooldown, showing message", agent->getActorName());
                        std::string cooldownMsg = std::format("[CHIM] {} does not want to talk right now.", agent->getActorName());
                        RE::DebugNotification(cooldownMsg.c_str());
                        return 0;
                    }
                    
                    targetActor->EndDialogue();
                    targetActor->GetActorBase()->voiceType = NullVoiceType->As<RE::BGSVoiceType>();
                    

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(targetActor));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "PrepareForDialog", args, callback);

                    std::string msg =
                        std::format("{}|{}|{}|{}:{}", "im_alive", getCurrentTimeMillis(), GetGameTimeStamp(),
                                    RE::PlayerCharacter::GetSingleton()->GetName(), "Are you ok?");

                    // NPC still Under cursor
                    ThreadPool::getInstance().enqueue(
                        "InitDialogDelay",
                        [msg, targetActor]() {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            HTTPManager::stream(msg, targetActor);
                        },
                        targetActor->GetDisplayFullName());

                    // targetActor->AllowPCDialogue(false);
                }
            }
        } else {
            logger::info("{} is not an NPC", targetObject.get()->GetName());
        }

    } else {
        logger::info("No target!");
    }

    return 0;
}

int sendMessageReal(
    std::string msg,
    std::string type,
    const PlayerConversationRoutingContext& routingContext) {
    logger::info("Call from papyrus: sendMessage");
    controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();
    PrismaUIBridge::BumpDialogueStopGeneration();

    SpeakManager& speakManager = SpeakManager::getInstance();
    speakManager.setLastUsedTime();  // To avoid trigger bored event from now
    speakManager.abortPendingUtterances("player_interrupt");
    speakManager.deleteQueue();
    speakManager.deleteQueuedPlayerLines();
    if (speakManager.getProcessing()) {
        speakManager.abortPlay(true);
    }
    speakManager.stopRechatForNseconds(3);

    SPGResponse::getInstance().clearAllQueues();

    // New. Must evaluate impact. Anyway, the queue was being deleted
    logger::info("Cancelling all ongoing HTTP stream messages before sendMessage");
    ThreadPool::getInstance().cancelTasksByType("HTTPStream");
    ThreadPool::getInstance().cancelTasksByType("HTTPStreamRechat");

    // SpeakManager::getInstance().setProcessing(false);

    if (!Conf::getInstance().isOk()) {
        RE::DebugNotification("[CHIM] AIAgent.ini is missing or invalid.");
        return -1;
    }

    /* More context */
    auto player = RE::PlayerCharacter::GetSingleton();
    RE::TESObjectCELL* cell = player->GetParentCell();

    auto result = InspectLocations(player->AsReference());

    char timeDateString[200];
    RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, true);

    HTTPManager::log(std::format("infoloc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                 "(Context location: " + std::string(GetPlayerLocation()) + ", Buildings to go:" +
                                     result + ", Current Date in Skyrim World: " + timeDateString + ")"));

    result =
        InspectSurroundings(player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);
    HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                 "(beings in range:" + result + ")"));

    if (REL::Module::GetRuntime() != REL::Module::Runtime::VR) {
        std::string itemsResult = InspectNearbyItems(player->AsReference(), 256.0f);

        if (!itemsResult.empty()) {
            std::string logMessage = std::format("infoitems|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "(items in range:" + itemsResult + ")");
            HTTPManager::log(logMessage);
        }
    }

    PlayerConversationRoutingContext effectiveRoutingContext = routingContext;
    std::string typeRevised;

    if (type == "inputtext_i") {
        typeRevised.assign("inputtext_s");
        effectiveRoutingContext.mode = PlayerConversationSpeechMode::Close;
    } else {
        const std::string currentConversationMode = effectiveRoutingContext.symbolRoutingMode.empty()
            ? PrismaUIBridge::GetCurrentChatboxMode()
            : effectiveRoutingContext.symbolRoutingMode;
        effectiveRoutingContext.mode =
            PlayerConversationRouter::ParseSpeechMode(currentConversationMode);
        effectiveRoutingContext.narratorMode =
            effectiveRoutingContext.narratorMode || currentConversationMode == "NARRATOR";
    }

    if (type != "inputtext_i" && type.empty())
        if (player->IsSneaking())
            typeRevised.assign("inputtext_s");
        else
            typeRevised.assign("inputtext");
    else if (type != "inputtext_i")
        typeRevised.assign(type);

    /* If not is animation busy, some plugin said shen can't call functions atm. To be revised*/
    /*
    if (typeRevised == "inputext" || typeRevised == "inputtext_s") {
        if (AIAgent::getInstance().getAnimationBusy())
            typeRevised.assign("chatnf");
    }

     if (typeRevised == "diary" ) {
        if (AIAgent::getInstance().isAvailableforAnimation()) {
            SKSE::GetTaskInterface()->AddTask([=]() {
                //
                AIAgent::getInstance().getHerikaSPG()->NotifyAnimationGraph("IdleBook_Read");
            });
        }
    }
     */

    /*
    HTTPManager::stream(std::format("{}|{}|{}|(Context location: {}) {} : {}", typeRevised, getCurrentTimeMillis(),
                       GetGameTimeStamp(), GetPlayerLocation(),
                                    RE::PlayerCharacter::GetSingleton()->GetName(), msg));
    */

    if (typeRevised == "diary") {
        auto position = player->GetPosition();
        AIAgentManager& aiam = AIAgentManager::getInstance();
        bool sent = false;
        
        // Check if looking up -> diary for narrator
        auto camera = RE::PlayerCamera::GetSingleton();
        auto cameraState = camera->currentState.get();
        if (cameraState) {
            auto narrator = aiam.getAgentByName(NARRATOR_NAME);
            if (narrator) {
                RE::NiQuaternion rotation;
                cameraState->GetRotation(rotation);
                
                // Get pitch in radians
                float pitchRadians = GetPitchFromQuaternion(rotation);
                
                // Convert to degrees
                float pitchDegrees = pitchRadians * (180.0f / 3.141592654f);
                
                logger::info("Camera pitch for diary: {} degrees", pitchDegrees);
                
                // Since looking up gave -68°, we need to check for negative values
                if (pitchDegrees < -85.0f) {  // Looking up more than 85°
                    logger::info("Diary directed to narrator via camera pitch");
                    HTTPManager::stream(
                        std::format("diary|{}|{}|{}:{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                    RE::PlayerCharacter::GetSingleton()->GetName(), msg),
                        narrator->getActor());
                    sent = true;
                }
            }
        }
        
        // Crosshair and fallback logic (only if not already sent to narrator)
        if (!sent) {
            auto cameraObject = RE::CrosshairPickData::GetSingleton()->target;
            if (cameraObject) {
                if (cameraObject.get()->GetFormType() == RE::FormType::ActorCharacter) {
                    auto targetActor = cameraObject.get()->As<RE::Actor>();

                    if (!targetActor->IsPlayerTeammate() && false) {
                        logger::info("{} is not a PlayerTeammate", cameraObject.get()->GetName());
                    } else {
                        for (const auto& agent : aiam.getAgents()) {
                            if (agent->getActor()->GetFormID() == targetActor->GetFormID()) {
                                // Will send request to Follower in cursor
                                HTTPManager::stream(
                                    std::format("{}|{}|{}|{}:{}", typeRevised, getCurrentTimeMillis(), GetGameTimeStamp(),
                                                RE::PlayerCharacter::GetSingleton()->GetName(), msg),
                                    targetActor);
                                sent = true;
                                break;
                            }
                        }
                    }
                } else {
                    logger::info("{} is not an NPC", cameraObject.get()->GetName());
                }
            }

            if (!sent) {
                // Send to all
                std::string beings = InspectSurroundings(player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",",
                                                         DISTANCE_ACTIVATING_NPC_OUT);
                for (const auto& agent : aiam.getAgents()) {
                    float minDistance = 5000;

                    if (beings.find(agent->getActorName()) == std::string::npos) {
                        continue;
                    }

                    auto player = RE::PlayerCharacter::GetSingleton();
                    float distance = position.GetDistance(agent->getActor()->GetPosition());
                    if (distance < minDistance) {
                        // Will send request to Follower in cursor

                        HTTPManager::stream(
                            std::format("{}|{}|{}|{}:{}", typeRevised, getCurrentTimeMillis(), GetGameTimeStamp(),
                                        RE::PlayerCharacter::GetSingleton()->GetName(), msg),
                            agent->getActor());
                    }
                }
            }
        }
    } else if (typeRevised == "diary_followers") {
        // Handle diary_followers - send to all current followers only
        auto position = player->GetPosition();
        AIAgentManager& aiam = AIAgentManager::getInstance();
        int followerCount = 0;
        
        logger::info("Processing diary_followers request - checking all agents for followers");
        
        for (const auto& agent : aiam.getAgents()) {
            if (!agent->getActor()) {
                continue;
            }
            
            // Check if this agent is a follower (player teammate)
            if (agent->getActor()->IsPlayerTeammate()) {
                logger::info("Found follower: {}", agent->getActorName());
                
                // Send diary request to this follower
                HTTPManager::stream(
                    std::format("diary|{}|{}|{}:{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                RE::PlayerCharacter::GetSingleton()->GetName(), msg),
                    agent->getActor());
                
                followerCount++;
            }
        }
        
        logger::info("Sent diary requests to {} followers", followerCount);
        
        if (followerCount == 0) {
            logger::info("No followers found to write diary entries");
        }
    } else if (typeRevised == "diary_nearby") {
        // Handle diary_nearby - send to all NPCs within range (800 units)
        // The Narrator is now included and controlled by server-side toggle
        auto position = player->GetPosition();
        AIAgentManager& aiam = AIAgentManager::getInstance();
        int nearbyCount = 0;
        const float maxDistance = 800.0f; // Fixed range of 800 units
        
        logger::info("Processing diary_nearby request - checking all agents within {} units", maxDistance);
        
        for (const auto& agent : aiam.getAgents()) {
            if (!agent->getActor()) {
                continue;
            }
            
            std::string actorName = agent->getActorName();
            
            // Calculate distance from player to this NPC
            auto agentPosition = agent->getActor()->GetPosition();
            float distance = position.GetDistance(agentPosition);
            
            // Check if this agent is within range
            if (distance <= maxDistance) {
                logger::info("Found nearby NPC: {} (distance: {:.1f})", actorName, distance);
                
                // Send diary request to this nearby NPC (including The Narrator if in range)
                HTTPManager::stream(
                    std::format("diary|{}|{}|{}:{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                RE::PlayerCharacter::GetSingleton()->GetName(), msg),
                    agent->getActor());
                
                nearbyCount++;
            } else {
                logger::debug("NPC {} too far away: {:.1f} units", actorName, distance);
            }
        }
        
        logger::info("Sent diary requests to {} nearby NPCs", nearbyCount);
        
        if (nearbyCount == 0) {
            logger::info("No NPCs found within {} units to write diary entries", maxDistance);
        }
    } else {
        const bool interruptsActiveConversation =
            typeRevised == "inputtext" ||
            typeRevised == "inputtext_s" ||
            typeRevised == "ginputtext_s" ||
            typeRevised == "ginputtext" ||
            typeRevised == "narrator_inputtext";

        if (interruptsActiveConversation) {
            ThreadPool::getInstance().cancelTasksByType("HTTPStream");
            ThreadPool::getInstance().cancelTasksByType("HTTPStreamRechat");
            SPGResponse::getInstance().clearAllQueues();
            SpeakManager::getInstance().startRechatChainForPlayerInput();
        }

        SpeakManager::getInstance().deleteQueue();
        if (SpeakManager::getInstance().getProcessing())
            SpeakManager::getInstance().abortPlay(true);  // Stop NPCs talking immediately for new player input

        SpeakManager::getInstance().stopRechatForNseconds(3);  // To avoid rechat if any rechat is pending

        HTTPManager::streamPlayer(
            std::format("{}|{}|{}|{}:{}", typeRevised, getCurrentTimeMillis(), GetGameTimeStamp(),
                        RE::PlayerCharacter::GetSingleton()->GetName(), msg),
            effectiveRoutingContext);
    }

    AIAgentManager& aiam = AIAgentManager::getInstance();

    return 0;

}

// Share the race gate between proximity activation and crosshair promotion.
static bool isAutoActivationRaceAllowed(RE::Actor* actor, RE::TESRace* race) {
    if (AutoAddAllRaces || (race->AllowsPCDialogue() && !race->HasKeywordString("ActorTypeCreature"))) {
        return true;
    }
    if (!AutoAddCreatureNPCs) {
        return false;
    }

    if (race->HasKeywordString("ActorTypeDragon") || race->HasKeywordString("ActorTypeDwarven") ||
        (race->HasKeywordString("ActorTypeUndead") && race->HasKeywordString("ActorTypeCreature")) ||
        (race->HasKeywordString("ActorTypeAnimal") && actor->IsPlayerTeammate())) {
        return true;
    }

    // These vanilla/DLC races have no distinct actor-type keyword. Match exact
    // editor IDs, never display names or partial names that could admit other creatures.
    const std::string_view editorID{race->GetFormEditorID()};
    return editorID == "HagravenRace" || editorID == "GiantRace" ||
           editorID == "C00GiantOutsideWhiterunRace" || editorID == "DLC2GhostFrostGiantRace" ||
           editorID == "FalmerRace" || editorID == "FalmerFrozenVampRace" ||
           editorID == "SprigganRace" || editorID == "SprigganMatronRace" ||
           editorID == "SprigganSwarmRace" || editorID == "SprigganEarthMotherRace" ||
           editorID == "DLC2SprigganBurntRace" || editorID == "WerewolfBeastRace" ||
           editorID == "dlc2SpectralDragonRace" || editorID == "DLC2RigidSkeletonRace";
}

void addAllNPC() {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);  // Lock the function, unlocks at the end

    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }

    auto cell = player->GetParentCell();
    if (!cell) {
        return;
    }

    AIAgentManager& aiam = AIAgentManager::getInstance();
    const bool playerInterior = cell->IsInteriorCell();
    const float maxDistance = playerInterior ? DISTANCE_ACTIVATING_NPC_IN : DISTANCE_ACTIVATING_NPC_OUT;
    std::unordered_set<RE::FormID> queuedFormIds;

    // Actors lurking around
    if (const auto processLists = RE::ProcessLists::GetSingleton(); processLists) {
        for (auto& targetHandle : processLists->highActorHandles) {
            if (auto target = targetHandle.get(); target && target->GetActorRuntimeData().currentProcess) {
                auto actor = target.get();
                if (!actor || actor->GetFormID() == player->GetFormID()) {
                    continue;
                }

                std::string actorLabel(actor->GetDisplayFullName());
                if (actorLabel.empty()) {
                    continue;
                }

                const auto actorFormId = actor->GetFormID();
                if ((actorFormId != 0 && (aiam.getAgentByFormId(actorFormId) ||
                                           !queuedFormIds.insert(actorFormId).second)) ||
                    aiam.getAgentByName(actorLabel)) {
                    continue;
                }

                if (actor->IsDead()) {
                    continue;
                } else if (!actor->Is3DLoaded()) {
                    continue;
                }

                auto* actorCell = actor->GetParentCell();
                if (!actorCell || !actorCell->IsAttached()) {
                    continue;
                }
                if (playerInterior != actorCell->IsInteriorCell()) {
                    continue;
                }
                if (playerInterior && actorCell != cell) {
                    continue;
                }

                auto* race = actor->GetRace();
                if (!race) {
                    continue;
                }
                if (!isAutoActivationRaceAllowed(actor, race)) {
                    continue;
                } else if (actor->IsHostileToActor(player) && AutoAddHostile == false) {
                    continue;
                }

                float distance = player->GetPosition().GetDistance(actor->GetPosition());
                if (!std::isfinite(distance) || distance > maxDistance) {
                    continue;
                }
                logger::info("Auto-adding {}", actorLabel);

                auto actorHandle = actor->GetHandle();
                ThreadPool::getInstance().enqueue(
                    "AddAllNPC", [actorHandle]() { setDrivenByAIReal(actorHandle, false, false, false, false); },
                    actorLabel);
            }
        }
    }
}

bool promoteCrosshairTargetToAI() {
    static std::mutex mtx;
    static RE::FormID lastPromoteFormId = 0;
    static auto lastPromoteAttempt = std::chrono::steady_clock::time_point{};
    constexpr auto kCrosshairPromoteCooldown = std::chrono::milliseconds(750);

    std::lock_guard<std::mutex> lock(mtx);

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return false;

    auto* cell = player->GetParentCell();
    if (!cell) return false;

    auto* crosshairPickData = RE::CrosshairPickData::GetSingleton();
    if (!crosshairPickData || !crosshairPickData->target) return false;

    auto targetRef = crosshairPickData->target.get();
    if (!targetRef || targetRef->GetFormType() != RE::FormType::ActorCharacter) return false;

    auto* actor = targetRef->As<RE::Actor>();
    if (!actor || actor->IsPlayer() || actor->IsDead() || !actor->Is3DLoaded()) return false;

    auto* actorCell = actor->GetParentCell();
    if (!actorCell || !actorCell->IsAttached()) return false;

    const bool playerInterior = cell->IsInteriorCell();
    if (playerInterior != actorCell->IsInteriorCell()) return false;
    if (playerInterior && actorCell != cell) return false;

    std::string actorLabel(actor->GetDisplayFullName());
    if (actorLabel.empty()) return false;

    auto& aiam = AIAgentManager::getInstance();
    if (aiam.getAgentByName(actorLabel)) return false;

    const RE::FormID actorFormId = actor->GetFormID();
    for (const auto& agent : aiam.getAgents()) {
        if (agent && agent->GetFormId() == actorFormId) {
            return false;
        }
    }

    auto* race = actor->GetRace();
    if (!race) return false;
    if (!isAutoActivationRaceAllowed(actor, race)) return false;
    if (actor->IsHostileToActor(player) && AutoAddHostile == false) return false;

    const float maxDistance = playerInterior ? DISTANCE_ACTIVATING_NPC_IN : DISTANCE_ACTIVATING_NPC_OUT;
    const float distance = player->GetPosition().GetDistance(actor->GetPosition());
    if (!std::isfinite(distance) || distance > maxDistance) return false;

    const auto now = std::chrono::steady_clock::now();
    if (actorFormId == lastPromoteFormId && lastPromoteAttempt.time_since_epoch().count() != 0 &&
        now - lastPromoteAttempt < kCrosshairPromoteCooldown) {
        return true;
    }

    lastPromoteFormId = actorFormId;
    lastPromoteAttempt = now;

    logger::info("[AUTOADD] Fast-promoting crosshair NPC {} dist={:.0f}", actorLabel, distance);
    auto actorHandle = actor->GetHandle();
    ThreadPool::getInstance().enqueue(
        "AddAllNPC",
        [actorHandle]() { setDrivenByAIReal(actorHandle, false, false, false, false); },
        actorLabel);

    return true;
}

int Papyrus::setConfReal(std::string code, float f_Value, int i_value, std::string s_value) {
    if (code == "_sound_volume") {
        AudioManagerController::GetInstance().setVolume(f_Value);
        logger::info("Setting volume to {}/100", f_Value);

    } else if (code == "_head_voice_volume") {
        SpeakManager::getInstance().setHeadVoiceVolumePercent(f_Value);
        logger::info("Setting narrator/player TTS volume to {}/100", f_Value);

    } else if (code == "_sound_preclip") {
        SpeakManager::getInstance().setPreclip(f_Value);
        logger::info("Setting preclip to {}/1000 seconds", f_Value);

    } else if (code == "_sound_postclip") {
        SpeakManager::getInstance().setPostclip(f_Value);
        logger::info("Setting postclip to {}/1000 seconds", f_Value);

    } else if (code == "_sound_ds") {
        AudioManagerController::GetInstance().setDistanceScaler(f_Value);
        logger::info("Setting distance scaler to {} units", f_Value);
    } else if (code == "_lip_res") {
        SpeakManager::getInstance().setResolution(f_Value + 1);
        logger::info("Setting lip resolution to {} units", f_Value + 1);
    } else if (code == "_lip_int") {
        SpeakManager::getInstance().setAnimIntensity(f_Value);
        logger::info("Setting lip anim intensity to {} units", f_Value);
    } else if (code == "_sgmode") {
        sgmode = i_value;
        logger::info("Setting sgmode to {} ", i_value);
    } else if (code == "_timeout") {
        GlobalConfiguredTimeout = static_cast<int>(std::round(f_Value));
        logger::info("Setting _timeout to {} ", f_Value);

    } else if (code == "_animations") {
        if (f_Value > 0)
            GlobalAnimations = true;
        else
            GlobalAnimations = false;

        logger::info("Setting _animations to {} ", f_Value);

    } else if (code == "_invertheadingstate") {
        if (f_Value > 0)
            GlobalInvertHeadingState = true;
        else
            GlobalInvertHeadingState = false;

        logger::info("Setting _invertheadingstate to {} ", f_Value);

    } else if (code == "_enable_3d_audio_playback") {
        if (f_Value > 0)
            GlobalEnable3DAudioPlayback = true;
        else
            GlobalEnable3DAudioPlayback = false;

        logger::info("Setting _enable_3d_audio_playback to {} ", f_Value);

    } else if (code == "_camera_based_audio") {
        if (f_Value > 0)
            GlobalCameraBasedAudio = true;
        else
            GlobalCameraBasedAudio = false;

        logger::info("Setting _camera_based_audio to {} ", f_Value);

    } else if (code == "_combat_dialogue") {
        if (f_Value > 0)
            CombatDialogueEnabled = true;
        else
            CombatDialogueEnabled = false;
        logger::info("Setting _combat_dialogue to {} ", f_Value);

    } else if (code == "_cancel_dialogue_on_combat") {
        if (f_Value > 0)
            CancelDialogueOnCombat = true;
        else
            CancelDialogueOnCombat = false;
        logger::info("Setting _cancel_dialogue_on_combat to {} ", CancelDialogueOnCombat);

    } else if (code == "_combat_barks") {
        if (f_Value > 0)
            CombatBarksEnabled = true;
        else
            CombatBarksEnabled = false;
        logger::info("Setting _combat_barks to {}", CombatBarksEnabled);

    } else if (code == "_combat_barks_period") {
        GlobalCombatBarksPeriod = static_cast<int>(f_Value);
        logger::info("Setting _combat_barks_period to {}s", GlobalCombatBarksPeriod);

    } else if (code == "_curve_legacy_distance") {
        
        float fValueCasted = static_cast<int>(f_Value);
        if (fValueCasted < 0.01) {
            AudioManagerController::GetInstance().setLegacyDistanceScaler(1.0);
            AudioManagerController::GetInstance().setLegacyAudioNoattenuation(true);
        } else {
            AudioManagerController::GetInstance().setLegacyAudioNoattenuation(false);
            GlobalLegacyDistanceScaler = fValueCasted;
            AudioManagerController::GetInstance().setLegacyDistanceScaler(fValueCasted);
            
        }

        logger::info("Setting _curve_legacy_distance to {}", fValueCasted);

    } else if (code == "_pause_dialogue_when_menu_open") {
        if (f_Value > 0)
            PauseDialogueWhenMenuOpen = true;
        else
            PauseDialogueWhenMenuOpen = false;
        logger::info("Setting _pause_dialogue_when_menu_open to {} ", f_Value);

    } else if (code == "_player_tts_traditional_dialogue") {
        if (f_Value > 0)
            PlayerTtsTraditionalDialogueEnabled = true;
        else
            PlayerTtsTraditionalDialogueEnabled = false;
        logger::info("Setting _player_tts_traditional_dialogue to {} ", f_Value);

    } else if (code == "_preserve_queue") {
        if (f_Value > 0)
            PreserveQueueDuringAction = true;
        else
            PreserveQueueDuringAction = false;
        logger::info("Setting _preserve_queue to {} ", f_Value);

    } else if (code == "_toggleAddAllNPC") {
        if (f_Value > 0)
            ENABLE_AUTOADDNPC = true;
        else
            ENABLE_AUTOADDNPC = false;

        logger::info("Setting _toggleAddAllNPC to {} ", f_Value);

    } else if (code == "_max_distance_inside") {
        if (f_Value > 0) DISTANCE_ACTIVATING_NPC_IN = f_Value;

        logger::info("Setting _max_distance_inside to {} ", f_Value);

    } else if (code == "_max_distance_outside") {
        if (f_Value > 0) DISTANCE_ACTIVATING_NPC_OUT = f_Value;

        logger::info("Setting _max_distance_outside to {} ", f_Value);

    } else if (code == "_spatial_hearing_inside") {
        if (f_Value > 0) SpatialAwareness::SetInteriorMaxDistance(f_Value);

        logger::info("Setting _spatial_hearing_inside to {} ", f_Value);

    } else if (code == "_spatial_hearing_outside") {
        if (f_Value > 0) SpatialAwareness::SetExteriorMaxDistance(f_Value);

        logger::info("Setting _spatial_hearing_outside to {} ", f_Value);

    } else if (code == "_auto_hearing_radius_m" || code == "_player_auto_include_radius_m") {
        if (f_Value > 0) {
            SpatialAwareness::SetAutoHearingRadiusMeters(f_Value);
            SpatialSnapshotManager::InvalidateDynamicSpatialState();
        }

        logger::info("Setting _auto_hearing_radius_m to {} ", f_Value);

    } else if (code == "_playback_dropoff_inside") {
        SpeakManager::getInstance().setPlaybackDropoffInside(f_Value);
        logger::info("Setting _playback_dropoff_inside to {} ", f_Value);

    } else if (code == "_playback_dropoff_outside") {
        SpeakManager::getInstance().setPlaybackDropoffOutside(f_Value);
        logger::info("Setting _playback_dropoff_outside to {} ", f_Value);

    } else if (code == "_bored_period") {
        GlobalBoredEventTimeOut = f_Value;
        logger::info("Setting bored period {}", f_Value);

    } else if (code == "_dynamic_profile_period") {
        GlobalDynamicProfileTimeOut = f_Value * 60; // Convert minutes to seconds
        logger::info("Setting dynamic profile period to {} minutes ({} seconds)", f_Value, GlobalDynamicProfileTimeOut);

    } else if (code == "_end_conversation_cooldown") {
        GlobalEndConversationCooldown = f_Value;
        logger::info("Setting end conversation cooldown to {}s", f_Value);

    } else if (code == "_rechat_policy_asap") {
        GlobalRechatPolicyAsap = 0;
        logger::info("Ignoring _rechat_policy_asap={} and forcing smart rechat on", f_Value);

    } else if (code == "_pause_dialogue_when_menu_open") {
        if (f_Value > 0)
            PauseDialogueWhenMenuOpen = true;
        else
            PauseDialogueWhenMenuOpen = false;

        logger::info("Setting _pause_dialogue_when_menu_open to {} ", f_Value);

    } else if (code == "_restrict_onscene") {
        if (f_Value > 0)
            AllowActorsOnScene = false;
        else
            AllowActorsOnScene = true;

        logger::info("Setting _restrict_onscene to {}, so AllowActorsOnScene is {}", f_Value, AllowActorsOnScene);

    } else if (code == "_autoadd_hostile") {
        if (f_Value > 0)
            AutoAddHostile = true;
        else
            AutoAddHostile = false;

        logger::info("Setting _autoadd_hostile to {} ", AutoAddHostile);

    } else if (code == "_autoadd_creature_npcs") {
        AutoAddCreatureNPCs = f_Value > 0;
        logger::info("Setting _autoadd_creature_npcs to {} ", AutoAddCreatureNPCs);

    } else if (code == "_autoadd_allraces") {
        if (f_Value > 0)
            AutoAddAllRaces = true;
        else
            AutoAddAllRaces = false;

        logger::info("Setting _autoadd_allraces to {} ", AutoAddAllRaces);
    } else if (code == "_godmode") {
        // Always false, modes will be handled by server
        GodMode = false;
        logger::info("Setting _godmode to {}", GodMode);

    } else if (code == "_openmic_enabled") {
        if (f_Value > 0) {
            OpenMicEnabled = true;
            // Start open mic monitoring
            if (!openMicMonitoringActive) {
                openMicMonitoringActive = true;
                if (openMicThread.joinable()) {
                    openMicThread.join();
                }
                openMicThread = std::thread(openMicMonitoringLoop);
                logger::info("Auto-started open mic monitoring");
            }
        } else {
            OpenMicEnabled = false;
            // Stop open mic monitoring
            if (openMicMonitoringActive) {
                openMicMonitoringActive = false;
                if (openMicThread.joinable()) {
                    openMicThread.join();
                }
                logger::info("Auto-stopped open mic monitoring");
            }
        }
        logger::info("Setting _openmic_enabled to {}", OpenMicEnabled);

    } else if (code == "_openmic_sensitivity") {
        OpenMicSensitivity = f_Value;
        logger::info("Setting _openmic_sensitivity to {}", OpenMicSensitivity);

    } else if (code == "_openmic_enddelay") {
        OpenMicEndDelay = f_Value;
        logger::info("Setting _openmic_enddelay to {}", OpenMicEndDelay);

    } else if (code == "_openmic_toggle_mute") {
        OpenMicMuted = !OpenMicMuted;
        logger::info("Toggling _openmic_muted to {}", OpenMicMuted);
        if (OpenMicMuted) {
            RE::DebugNotification("[CHIM] Open mic muted");
        } else {
            RE::DebugNotification("[CHIM] Open mic unmuted");
        }

    } else if (code == "_history_panel_enabled") {
        if (f_Value > 0) {
            PrismaUIBridge::SetEnabled(true);
            logger::info("Enabling history panel");
        } else {
            PrismaUIBridge::SetEnabled(false);
            PrismaUIBridge::HideHistoryPanel();
            logger::info("Disabling history panel");
        }

    } else if (code == "_history_panel_toggle") {
        if (PrismaUIBridge::IsAvailable()) {
            PrismaUIBridge::ToggleHistoryPanel();
            logger::info("Toggling history panel");
        }

    } else {
        logger::info("Unknown configuration code: {}", code);
        return -1;
    }

    return 0;
}

int Papyrus::sendMessage(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                         std::string msg, std::string type) {
    ScopedPapyrusLock lock("sendMessage");


    auto result = sendMessageReal(msg, type);
    return result;
}

int Papyrus::sendMessageToActor(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                RE::StaticFunctionTag*, std::string msg, std::string type, RE::Actor* targetActor) {
    ScopedPapyrusLock lock("sendMessageToActor");

    PlayerConversationRoutingContext routingContext{};
    if (targetActor) {
        routingContext.explicitTargetFormId = targetActor->GetFormID();
        if (const auto* targetName = targetActor->GetDisplayFullName()) {
            routingContext.explicitTargetName = targetName;
        }
    }

    return sendMessageReal(msg, type, routingContext);
}

int Papyrus::logMessage(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                        std::string msg, std::string type) {
    ScopedPapyrusLock lock("logMessage");
    const bool isSetConf = type == "setconf" || type == "setConf";
    const bool isCellTelemetry = type == "named_cell" || type == "named_cell_static";

    // Cell telemetry carries its complete payload and should not trigger an unrelated actor scan.
    if (!isSetConf && !isCellTelemetry) {
        if (auto player = RE::PlayerCharacter::GetSingleton()) {
            auto result = InspectSurroundings(
                player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);
            HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "(beings in range:" + result + ")"));
        }
    }

    if (isSetConf) {
        constexpr std::string_view modePrefix = "chim_mode@";
        if (msg.starts_with(modePrefix)) {
            PrismaUIBridge::SetCurrentChatboxMode(
                msg.substr(modePrefix.size()), "Papyrus Mode Selection", false);
        }
    }

    HTTPManager::log(std::format("{}|{}|{}|{}", type, getCurrentTimeMillis(), GetGameTimeStamp(), msg));
    return 0;
}

int Papyrus::logMessageForActor(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                RE::StaticFunctionTag*, std::string msg, std::string type, std::string actor) {
    ScopedPapyrusLock lock("logMessageForActor");
    AIAgentManager& aiam = AIAgentManager::getInstance();
    auto actorPtr = aiam.getAgentByName(actor);
    if (actorPtr) {
        auto player = RE::PlayerCharacter::GetSingleton();
        RE::TESObjectCELL* cell = player->GetParentCell();
        auto result =
            InspectSurroundings(player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);

        HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "(beings in range:" + result + ")"));

        HTTPManager::log(std::format("{}|{}|{}|{}", type, getCurrentTimeMillis(), GetGameTimeStamp(), msg),
                         actorPtr->getActor());
    } else {
        // Fallback, using forcedActor
        HTTPManager::log(std::format("{}|{}|{}|{}", type, getCurrentTimeMillis(), GetGameTimeStamp(), msg), actor);
    }
    return 0;
}

int Papyrus::requestMessage(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                            RE::StaticFunctionTag*, std::string msg, std::string type) {
    
    controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();
    ScopedPapyrusLock lock("requestMessage");
    SpeakManager::getInstance().getLastUsedTime();  // To avoid trigger bored event from now

    auto player = RE::PlayerCharacter::GetSingleton();
    RE::TESObjectCELL* cell = player->GetParentCell();
    auto result =
        InspectSurroundings(player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);

    HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                 "(beings in range:" + result + ")"));

    HTTPManager::stream(std::format("{}|{}|{}|(Context location: {}){}:{}", type, getCurrentTimeMillis(),
                                    GetGameTimeStamp(), GetPlayerLocation(),
                                    RE::PlayerCharacter::GetSingleton()->GetName(), msg));
    return 0;
}

int Papyrus::requestMessageForActor(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                    RE::StaticFunctionTag*, std::string msg, std::string type, std::string npc) {
    ScopedPapyrusLock lock("requestMessageForActor");
    controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();
    SpeakManager::getInstance().getLastUsedTime();  // To avoid trigger bored event from now
    AIAgentManager& aiam = AIAgentManager::getInstance();
    
    // Special handling for diary requests with empty npc name (looking at sky/ground)
    // Route through sendMessageReal which includes camera pitch detection for Narrator
    if (type == "diary" && (npc.empty() || trim(npc).empty())) {
        logger::info("[requestMessageForActor] Diary request with no target, routing through sendMessageReal");
        return sendMessageReal(msg, type);
    }
    
    auto actorPtr = aiam.getAgentByName(npc);
    const bool isAutonomousDirective = type == "instruction" || type == "suggestion";
    const auto requestText = isAutonomousDirective
        ? msg
        : std::format("{}:{}", RE::PlayerCharacter::GetSingleton()->GetName(), msg);

    if (actorPtr) {
        auto player = RE::PlayerCharacter::GetSingleton();
        RE::TESObjectCELL* cell = player->GetParentCell();
        auto result =
            InspectSurroundings(player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);

        HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "(beings in range:" + result + ")"));

        HTTPManager::stream(
            std::format("{}|{}|{}|(Context location: {}){}", type, getCurrentTimeMillis(), GetGameTimeStamp(),
                        GetPlayerLocation(), requestText),
            actorPtr->getActor());
    } else {
        // Fallback
        HTTPManager::stream(std::format("{}|{}|{}|(Context location: {}){}", type, getCurrentTimeMillis(),
                                        GetGameTimeStamp(), GetPlayerLocation(), requestText));
    }

    return 0;
}

int Papyrus::setAnimationBusy(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                              RE::StaticFunctionTag*, int busy, std::string actor) {
    ScopedPapyrusLock lock("setAnimationBusy");
    logger::debug("Papyrus::setAnimationBusy - Start");
    AIAgentManager& aiam = AIAgentManager::getInstance();
    actor = trim(actor);
    auto actorPtr = aiam.getAgentByName(actor);

    if (actorPtr) {
        if (busy == 0)
            actorPtr->setAnimationBusy(false);
        else
            actorPtr->setAnimationBusy(true);

        logger::info("Set animation busy {} {} ", busy, actor);
        logger::debug("Papyrus::setAnimationBusy - End");
        return 0;
    } else {
        logger::debug("Papyrus::setAnimationBusy - failed");
        return -1;
    }
}


int Papyrus::setLocked(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                              RE::StaticFunctionTag*, int busy, std::string actor) {
    ScopedPapyrusLock lock("setLocked");
    logger::debug("Papyrus::setLocked - Start");
    AIAgentManager& aiam = AIAgentManager::getInstance();
    actor = trim(actor);
    auto actorPtr = aiam.getAgentByName(actor);

    if (actorPtr) {
        if (busy == 0)
            actorPtr->setExternalLocked(false);
        else
            actorPtr->setExternalLocked(true);

        logger::info("Set setExternalLocked  {} {} ", busy, actor);
        logger::debug("Papyrus::setLocked - End");
        return 0;
    } else {
        logger::debug("Papyrus::setLocked - failed {}",actor);
        return -1;
    }
}

int Papyrus::isActorTalking(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                            RE::StaticFunctionTag*, std::string actor) {
    ScopedPapyrusLock lock("isActorTalking");
    AIAgentManager& aiam = AIAgentManager::getInstance();
    actor = trim(actor);
    auto actorPtr = aiam.getAgentByName(actor);

    if (!actorPtr) {
        return 0;
    }

    return actorPtr->isTalking() ? 1 : 0;
}

int Papyrus::getPlayerBountyForGuard(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                     RE::StaticFunctionTag*, std::string guardName) {
    ScopedPapyrusLock lock("getPlayerBountyForGuard");
    AIAgentManager& aiam = AIAgentManager::getInstance();
    guardName = trim(guardName);
    auto agentPtr = aiam.getAgentByName(guardName);

    if (!agentPtr) {
        return 0;
    }

    auto actor = agentPtr->getActor();
    if (!actor) {
        return 0;
    }

    auto crimeFaction = actor->GetCrimeFaction();
    if (!crimeFaction) {
        logger::info("[getPlayerBountyForGuard] No crime faction for {}", guardName);
        return 0;
    }

    auto crimeGold = crimeFaction->GetCrimeGold();
    logger::info("[getPlayerBountyForGuard] {} crime faction 0x{:08x}, bounty={}", guardName,
                 crimeFaction->GetFormID(), crimeGold);
    return static_cast<int>(crimeGold);
}

int Papyrus::requestMoveInventoryItemConfirmation(RE::BSScript::Internal::VirtualMachine* a_vm,
                                                  RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                                                  RE::Actor* source, RE::Actor* target, RE::TESForm* itemForm,
                                                  int amount, std::string realName) {
    ScopedPapyrusLock lock("requestMoveInventoryItemConfirmation");

    if (!source || !target || !itemForm || amount <= 0) {
        logger::warn("[PAPYRUS] requestMoveInventoryItemConfirmation: invalid arguments");
        return 0;
    }

    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] requestMoveInventoryItemConfirmation: Prisma UI not available");
        return 0;
    }

    if (realName.empty()) {
        realName = "Gold";
    }

    auto sourceHandle = source->GetHandle();
    auto targetHandle = target->GetHandle();
    const RE::FormID itemFormId = itemForm->GetFormID();
    const std::string itemName = realName;
    const std::string sourceName = source->GetDisplayFullName();
    const std::string targetName = target->GetDisplayFullName();
    const std::string message = sourceName + " will transfer " + std::to_string(amount) + " gold to " + targetName + ".";

    bool shown = PrismaUIBridge::ShowConfirmation(
        "Confirm gold transfer", message, "No", "Confirm",
        [sourceHandle, targetHandle, itemFormId, amount, itemName](bool accepted) {
            auto dispatch = [sourceHandle, targetHandle, itemFormId, amount, itemName, accepted]() mutable {
                auto sourceRef = sourceHandle.get();
                auto targetRef = targetHandle.get();
                auto* resolvedSource = sourceRef ? sourceRef->As<RE::Actor>() : nullptr;
                auto* resolvedTarget = targetRef ? targetRef->As<RE::Actor>() : nullptr;
                auto* resolvedItem = RE::TESForm::LookupByID(itemFormId);

                if (!resolvedSource || !resolvedTarget || !resolvedItem) {
                    logger::warn("[PAPYRUS] ConfirmMoveInventoryItem skipped; source/target/item no longer valid");
                    return;
                }

                int amountArg = amount;
                std::string itemNameArg = itemName;
                bool acceptedArg = accepted;
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                auto args = RE::MakeFunctionArguments(std::move(resolvedSource), std::move(resolvedTarget),
                                                      std::move(resolvedItem), std::move(amountArg),
                                                      std::move(itemNameArg), std::move(acceptedArg));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "ConfirmMoveInventoryItem", args, callback);
            };

            if (auto* taskInterface = SKSE::GetTaskInterface()) {
                taskInterface->AddTask(std::move(dispatch));
            } else {
                dispatch();
            }
        });

    return shown ? 1 : 0;
}

int Papyrus::requestArrestConfirmation(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                       RE::StaticFunctionTag*, RE::Actor* player, RE::Actor* guard,
                                       RE::TESFaction* crimeFaction) {
    ScopedPapyrusLock lock("requestArrestConfirmation");

    if (!player || !guard || !crimeFaction) {
        logger::warn("[PAPYRUS] requestArrestConfirmation: invalid arguments");
        return 0;
    }

    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] requestArrestConfirmation: Prisma UI not available");
        return 0;
    }

    auto playerHandle = player->GetHandle();
    auto guardHandle = guard->GetHandle();
    const RE::FormID crimeFactionId = crimeFaction->GetFormID();
    const std::string guardName = guard->GetDisplayFullName();
    const std::string message = guardName + " is placing you under arrest. Submit?";

    bool shown = PrismaUIBridge::ShowConfirmation(
        "Confirm arrest", message, "Resist", "Submit",
        [playerHandle, guardHandle, crimeFactionId](bool accepted) {
            auto dispatch = [playerHandle, guardHandle, crimeFactionId, accepted]() mutable {
                auto playerRef = playerHandle.get();
                auto guardRef = guardHandle.get();
                auto* resolvedPlayer = playerRef ? playerRef->As<RE::Actor>() : nullptr;
                auto* resolvedGuard = guardRef ? guardRef->As<RE::Actor>() : nullptr;
                auto* resolvedFaction = RE::TESForm::LookupByID<RE::TESFaction>(crimeFactionId);

                if (!resolvedPlayer || !resolvedGuard || !resolvedFaction) {
                    logger::warn("[PAPYRUS] ConfirmArrestPlayer skipped; player/guard/faction no longer valid");
                    return;
                }

                bool acceptedArg = accepted;
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                auto args = RE::MakeFunctionArguments(std::move(resolvedPlayer), std::move(resolvedGuard),
                                                      std::move(resolvedFaction), std::move(acceptedArg));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "ConfirmArrestPlayer", args, callback);
            };

            if (auto* taskInterface = SKSE::GetTaskInterface()) {
                taskInterface->AddTask(std::move(dispatch));
            } else {
                dispatch();
            }
        });

    return shown ? 1 : 0;
}

int Papyrus::commandEnded(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                          std::string command) {
    // EndCommand(command, akACtor->GetDisplayFullName());
    logger::info("Should remove this function");
    return 0;
}

int Papyrus::commandEndedForActor(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                  RE::StaticFunctionTag*, std::string command, std::string npc) {
    ScopedPapyrusLock lock("commandEndedForActor");
    if (command.contains("attack") || command.contains("Attack")) {
        AIAgentManager& aiam = AIAgentManager::getInstance();
        auto agentPtr = aiam.getAgentByName(npc);

        if (!agentPtr) {
            logger::info("No AI actor found, can't end command");
            return -1;
        } else {
            auto victim = agentPtr.get()->getAttackTarget();
            auto follower = agentPtr.get()->getActor();
            if (victim) {
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                auto args = RE::MakeFunctionArguments(std::move(follower), std::move(victim->AsReference()));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "StartCombat", args, callback);
            }
        }

        agentPtr.get()->setCurrentCommand("");
        agentPtr.get()->setCommandBusy(false);
        return 0;
    } else {
        EndCommand(command, npc);
        return 0;
    }
    return 0;
}

int Papyrus::getHerikaFormId(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                             RE::StaticFunctionTag*) {
    /*
    if (!AIAgent::getInstance().getHerikaSPG()) {
        logger::info("Herika not instantiated");
        return 0;
    }

    return AIAgent::getInstance().getHerikaSPG()->GetFormID();
    */

    return 0;
}

int Papyrus::recordSoundEx(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                           RE::StaticFunctionTag*, int bindedKey) {
    
    SpeakManager::getInstance().setLastUsedTime();  // To avoid trigger bored event from now
    SpeakManager::getInstance().deleteQueue();
    SPGResponse::getInstance().clearAllQueues();  // To avoid trigger bored event from now
    if (SpeakManager::getInstance().getProcessing()) {
        SpeakManager::getInstance().abortPlay(true);
        SpeakManager::getInstance().setProcessing(false);
    }

    // New. Must evaluate impact. Anyway, the queue was being deleted
    ThreadPool::getInstance().cancelTasksByType("HTTPStream");
    ThreadPool::getInstance().cancelTasksByType("HTTPStreamRechat");
    AudioManagerController::GetInstance().Stop();

    // Disabled: froze game thread on STT press; makeSTT() does the same logging post-STT off-thread.
    /*
    auto player = RE::PlayerCharacter::GetSingleton();
    if (player) {
        char timeDateString[200];
        RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, true);

        RE::TESObjectCELL* cell = player->GetParentCell();
        auto result = InspectLocations(player->AsReference());
        HTTPManager::log(std::format("infoloc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "(Context location: " + std::string(GetPlayerLocation()) + ", buildings to go:" +
                                         result + ", Current Date in Skyrim World: " + timeDateString + ")"));

        result = InspectSurroundings(player, true, HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);
        HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "beings in range:" + result + ")"));
    }
    */

    VoiceRecordControl::getInstance().setRecording(true);

    VoiceRecord(bindedKey);
    return 0;
}

int Papyrus::stopRecording(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                           RE::StaticFunctionTag*, int bindedKey) {
    controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();
    VoiceRecordControl::getInstance().setRecording(false);
    return 0;
}

int Papyrus::startOpenMicMonitoring(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                   RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("startOpenMicMonitoring");
    logger::info("Starting open mic monitoring");
    
    if (!openMicMonitoringActive) {
        openMicMonitoringActive = true;
        
        // Stop existing thread if running
        if (openMicThread.joinable()) {
            openMicThread.join();
        }
        
        // Start new monitoring thread
        openMicThread = std::thread(openMicMonitoringLoop);
        logger::info("Open mic monitoring thread started");
        RE::DebugNotification("[CHIM] Open mic monitoring started");
    }
    
    return 0;
}

int Papyrus::stopOpenMicMonitoring(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                  RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("stopOpenMicMonitoring");
    logger::info("Stopping open mic monitoring");
    
    if (openMicMonitoringActive) {
        openMicMonitoringActive = false;
        
        // Wait for thread to finish
        if (openMicThread.joinable()) {
            openMicThread.join();
        }
        
        logger::info("Open mic monitoring thread stopped");
        RE::DebugNotification("[CHIM] Open mic monitoring stopped");
    }
    
    return 0;
}

int Papyrus::setOpenMicMuted(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                            RE::StaticFunctionTag*, bool muted) {
    ScopedPapyrusLock lock("setOpenMicMuted");
    OpenMicMuted = muted;
    logger::info("Setting open mic muted to {}", muted);
    if (muted) {
        RE::DebugNotification("[CHIM] Open mic muted");
    } else {
        RE::DebugNotification("[CHIM] Open mic unmuted");
    }
    return 0;
}

std::string Papyrus::getCurrentRecordingDeviceName(RE::BSScript::Internal::VirtualMachine* a_vm,
                                                   RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("getCurrentRecordingDeviceName");
    return GetCurrentRecordingDeviceName();
}

int Papyrus::beginChimMcmSnapshot(RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("beginChimMcmSnapshot");
    PrismaUIBridge::BeginChimMcmSnapshot();
    return 1;
}

int Papyrus::publishChimMcmEntry(
    RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::StaticFunctionTag*,
    std::string page, std::string section, std::string key, std::string label, std::string description,
    std::string type, std::string value, std::string options) {
    ScopedPapyrusLock lock("publishChimMcmEntry");
    std::array<std::string, 6> parsedOptions{};
    std::istringstream optionStream(options);
    for (auto& option : parsedOptions) {
        std::getline(optionStream, option, '|');
    }
    const auto parseFloat = [](const std::string& raw) {
        try {
            return raw.empty() ? 0.0f : std::stof(raw);
        } catch (...) {
            return 0.0f;
        }
    };
    PrismaUIBridge::PublishChimMcmEntry(
        page, section, key, label, description, type, value,
        parseFloat(parsedOptions[0]), parseFloat(parsedOptions[1]), parseFloat(parsedOptions[2]), parsedOptions[3],
        parsedOptions[4] == "1", parsedOptions[5] == "1");
    return 1;
}

int Papyrus::commitChimMcmSnapshot(
    RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::StaticFunctionTag*, int revision) {
    ScopedPapyrusLock lock("commitChimMcmSnapshot");
    PrismaUIBridge::CommitChimMcmSnapshot(revision);
    return 1;
}

int Papyrus::beginChimMcmAgents(RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("beginChimMcmAgents");
    PrismaUIBridge::BeginChimMcmAgents();
    return 1;
}

int Papyrus::publishChimMcmAgent(
    RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::StaticFunctionTag*,
    std::string bucket, int formId, std::string name) {
    ScopedPapyrusLock lock("publishChimMcmAgent");
    PrismaUIBridge::PublishChimMcmAgent(bucket, formId, name);
    return 1;
}

int Papyrus::commitChimMcmAgents(RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("commitChimMcmAgents");
    PrismaUIBridge::CommitChimMcmAgents();
    return 1;
}

int Papyrus::publishChimMcmCommandResult(
    RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::StaticFunctionTag*,
    std::string request, bool ok, std::string message) {
    ScopedPapyrusLock lock("publishChimMcmCommandResult");
    PrismaUIBridge::PublishChimMcmCommandResult(request, ok, message);
    return 1;
}

int Papyrus::setConf(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                     std::string code, float f_Value, int i_value, std::string s_value) {
    ScopedPapyrusLock lock("setConf");
  
    setConfReal(code, f_Value, i_value, s_value);

    controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now();

    return 0;
}

int Papyrus::setDrivenByAI(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                           RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("setDrivenByAI");

    auto targetObject = RE::CrosshairPickData::GetSingleton()->targetActor;

    if (!targetObject && REL::Module::GetRuntime() != REL::Module::Runtime::VR) {
        logger::info("Checking NPC via grabbed ref");
        auto targetObjectRef = RE::PlayerCharacter::GetSingleton()->GetGrabbedRef();
        targetObject = targetObjectRef.get();
        if (targetObject) logger::info("Checked NPC via grabbed ref {}", targetObject.get()->GetDisplayFullName());
    } else if (!targetObject) {
        logger::debug("Skipping grabbed ref target fallback in VR");
    }

    ThreadPool::getInstance().enqueue(
        "SetDrivenByAI", [targetObject]() { setDrivenByAIReal(targetObject, true, true, true, true); },
        targetObject ? targetObject.get()->GetDisplayFullName() : "unknown");

    return 0;
}

std::string GetLocationSpecialRefsStringImpl(RE::FormID a_locationFormID) {
    std::ostringstream ss;

    auto locationForm = RE::TESForm::LookupByID(a_locationFormID);
    if (!locationForm) {
        return "";
    }

    auto location = locationForm->As<RE::BGSLocation>();
    if (!location) {
        return "";
    }

    // Lookup the ref types once
    auto insideMarkerRefType = RE::TESForm::LookupByID<RE::BGSLocationRefType>(0x000130fc);
    auto bossTreasureMarkerRefType = RE::TESForm::LookupByID<RE::BGSLocationRefType>(0x000130f9);  // BossTreasureMarker
    auto locationCenterRefType = RE::TESForm::LookupByID<RE::BGSLocationRefType>(0x0001bdf1);
    auto outsideEntranceMarkerRefType = RE::TESForm::LookupByID<RE::BGSLocationRefType>(0x000130fb);
    auto mapMarkerRefType = RE::TESForm::LookupByID<RE::BGSLocationRefType>(0x00010f63c);

    RE::TESObjectREFR* stdMarker = nullptr;

    if (location->worldLocMarker) {
        if (location->worldLocMarker.get()) stdMarker = location->worldLocMarker.get().get();
    }

    if (!insideMarkerRefType && !bossTreasureMarkerRefType && !locationCenterRefType && !outsideEntranceMarkerRefType &&
        !mapMarkerRefType && !stdMarker) {
        return "";
    }

    bool first = true;

    for (auto& refData : location->specialRefs) {
        if (!refData.refData.refID || !refData.type) {
            continue;
        }

        auto typeID = refData.type->GetFormID();
        auto refID = refData.refData.refID;

        // Only include the three types we care about
        if (refData.type != insideMarkerRefType && refData.type != bossTreasureMarkerRefType &&
            refData.type != locationCenterRefType && refData.type != outsideEntranceMarkerRefType &&
            refData.type != mapMarkerRefType) {
            continue;
        }

        if (!first) {
            ss << ";";
        }

        ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << typeID << ":0x" << std::hex << std::setw(8)
           << std::setfill('0') << refID;

        first = false;
    }
    if (stdMarker) {
        if (!first) {
            ss << ";";
        }

        ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << mapMarkerRefType->GetFormID() << ":0x"
           << std::hex << std::setw(8) << std::setfill('0') << stdMarker->GetFormID();
    }

    return ss.str();
}

int Papyrus::setDrivenByAIA(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                            RE::StaticFunctionTag*, RE::Actor* forcedActor, bool salutation) {
    ScopedPapyrusLock lock("setDrivenByAIA");
    if (forcedActor) {
        ThreadPool::getInstance().enqueue(
            "SetDrivenByAIA",
            [forcedActor, salutation]() { setDrivenByAIReal(forcedActor->GetHandle(), salutation, true, true, true); },
            forcedActor->GetDisplayFullName());
        return 0;
    } else {
        logger::debug("Papyrus::setDrivenByAIA - End (forcedActor is null)");
        return -1;
    }
}

int Papyrus::removeAgentByName(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                               RE::StaticFunctionTag*, std::string name) {
    ScopedPapyrusLock lock("removeAgentByName");
    AIAgentManager& aiam = AIAgentManager::getInstance();
    auto already = aiam.getAgentByName(name);

    if (already) {
        auto actor = already->getActor();
        if (actor) {
            logger::info("About to restore voice for {}", name);
            actor->GetActorBase()->voiceType = already->getOriginalVoice();
        }
        aiam.deleteAgent(already);
    }
    std::string action;
    action.assign("remove");
    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
    auto args = RE::MakeFunctionArguments(std::move(name), std::move(action));
    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "SendExternalEventNPC",
                                                                               args, callback);
    return 0;
}

int Papyrus::get_conf_i(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                        std::string code) {
    ScopedPapyrusLock lock("get_conf_i");
    int result = -1;
    if (code == "_sgmode") {
        result = sgmode;
    } else if (code == "_lip_res") {
        result = SpeakManager::getInstance().getResolution();
        
    } else if (code == "_lip_int") {
        result = SpeakManager::getInstance().getAnimIntensity();
    } else if (code == "_timeout") {
        result = GlobalConfiguredTimeout;

    } else if (code == "_animations") {
        result=GlobalAnimations ? 1 : 0;

    } else if (code == "_enable_3d_audio_playback") {
        result = GlobalEnable3DAudioPlayback ? 1 : 0;

    } else if (code == "_camera_based_audio") {
        result = GlobalCameraBasedAudio ? 1 : 0;

    } else if (code == "_combat_dialogue") {
        result = CombatDialogueEnabled ? 1 : 0;

    } else if (code == "_cancel_dialogue_on_combat") {
        result = CancelDialogueOnCombat ? 1 : 0;

    } else if (code == "_combat_barks") {
        result = CombatBarksEnabled ? 1 : 0;

    } else if (code == "_combat_barks_period") {
        result = GlobalCombatBarksPeriod;

    } else if (code == "_max_distance_inside") {
        result = DISTANCE_ACTIVATING_NPC_IN;

    } else if (code == "_max_distance_outside") {
        result = DISTANCE_ACTIVATING_NPC_OUT;

    } else if (code == "_spatial_hearing_inside") {
        result = static_cast<int>(SpatialAwareness::GetSettings().interiorMaxDistance);

    } else if (code == "_spatial_hearing_outside") {
        result = static_cast<int>(SpatialAwareness::GetSettings().exteriorMaxDistance);

    } else if (code == "_auto_hearing_radius_m" || code == "_player_auto_include_radius_m") {
        result = static_cast<int>(std::lround(SpatialAwareness::GetAutoHearingRadiusMeters()));

    } else if (code == "_playback_dropoff_inside") {
        result = static_cast<int>(SpeakManager::getInstance().getPlaybackDropoffInside());

    } else if (code == "_playback_dropoff_outside") {
        result = static_cast<int>(SpeakManager::getInstance().getPlaybackDropoffOutside());

    } else if (code == "_bored_period") {
        result = GlobalBoredEventTimeOut;

    } else if (code == "_dynamic_profile_period") {
        result = GlobalDynamicProfileTimeOut / 60; // Convert seconds back to minutes

    } else if (code == "_rechat_policy_asap") {
        result = 0;

    } else if (code == "_pause_dialogue_when_menu_open") {
        result = PauseDialogueWhenMenuOpen;

    } else if (code == "_player_tts_traditional_dialogue") {
        result = PlayerTtsTraditionalDialogueEnabled ? 1 : 0;

    } else if (code == "_restrict_onscene") {
        
        result = AllowActorsOnScene ? 0 : 1;
    } else if (code == "_godmode") {
        result = GodMode ? 1 : 0;

    } else if (code == "_openmic_enabled") {
        result = OpenMicEnabled ? 1 : 0;

    } else if (code == "_openmic_sensitivity") {
        result = static_cast<int>(OpenMicSensitivity);

    } else if (code == "_openmic_enddelay") {
        result = static_cast<int>(OpenMicEndDelay * 10); // Convert to tenths of seconds

    } else if (code == "_openmic_muted") {
        result = OpenMicMuted ? 1 : 0;

    } else if (code == "_combat_barks_period") {
        result = GlobalCombatBarksPeriod;

    } else if (code == "_curve_legacy_distance") {
        result = AudioManagerController::GetInstance().getDistanceScaler();

    }  else {
        logger::info("Unknown configuration code: {}", code);
        result=-1;
    } 
    return result;
}

int Papyrus::setNewActionMode(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                              RE::StaticFunctionTag*, int mode) {
    ScopedPapyrusLock lock("setNewActionMode");
    if (mode == 1)
        NewActionMode = true;
    else
        NewActionMode = false;
    return 0;
}

RE::Actor* Papyrus::getClosestAgent(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                    RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("getClosestAgent");
    auto result = findClosestAgent();
    return result;
}

RE::Actor* Papyrus::getAgentByName(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                   RE::StaticFunctionTag*, std::string npcName) {
    ScopedPapyrusLock lock("getAgentByName");
    AIAgentManager& aiam = AIAgentManager::getInstance();
    npcName = trim(npcName);
    logger::info("[PAPYRUS] getAgentByName <{}> called", npcName);
    auto npc = aiam.getAgentByName(npcName);
    
    std::string beings = InspectSurroundings(RE::PlayerCharacter::GetSingleton()->AsReference(), true,
                                             HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);
    if (npc) {
        logger::info("[PAPYRUS] getAgentByName <{}>, agent exists", npcName);
        if (npc->getActor())
            if (npc->isPresent(beings)) {
                return npc->getActor();
            }
    }

    return nullptr;
}

std::vector<RE::Actor*> Papyrus::findAllNearbyAgents(RE::BSScript::Internal::VirtualMachine* a_vm,
                                                     RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    
    auto startTime = std::chrono::high_resolution_clock::now();
    ScopedPapyrusLock lock("findAllNearbyAgents");
    AIAgentManager& aiam = AIAgentManager::getInstance();
        
    std::string beings = InspectSurroundings(RE::PlayerCharacter::GetSingleton()->AsReference(), true,
                                             HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);

    std::vector<RE::Actor*> actorList;

    for (const auto& agent : aiam.getAgents()) {
        if (agent->isPresent(beings)) {
            RE::Actor* actor = agent->getActor();
            if (actor) {
                actorList.push_back(actor);
            }
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();
    logger::debug("[findAllNearbyAgents] Time taken: {} microseconds", duration);

    return actorList;
}

std::vector<RE::Actor*> Papyrus::findAllAgents(RE::BSScript::Internal::VirtualMachine* a_vm,
                                               RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("findAllAgents");
    AIAgentManager& aiam = AIAgentManager::getInstance();

    std::vector<RE::Actor*> actorList;

    for (const auto& agent : aiam.getAgents()) {
        if (agent) {
            RE::Actor* actor = agent->getActor();
            if (actor) {
                actorList.push_back(actor);
            }
        }
    }

    return actorList;
}

std::vector<RE::Actor*> Papyrus::findAllNearbyNonAgents(RE::BSScript::Internal::VirtualMachine* a_vm,
                                                        RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("findAllNearbyNonAgents");
    
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return {};

    auto cell = player->GetParentCell();
    if (!cell) return {};

    AIAgentManager& aiam = AIAgentManager::getInstance();
    std::vector<RE::Actor*> nonAgentList;

    // Minimal filtering - MCM should be an override to manually add ANY nearby NPC/creature
    if (const auto processLists = RE::ProcessLists::GetSingleton(); processLists) {
        for (auto& targetHandle : processLists->highActorHandles) {
            if (auto target = targetHandle.get(); target && target->GetActorRuntimeData().currentProcess) {
                std::string actorLabel(target->GetDisplayFullName());
                if (!actorLabel.empty()) {
                    auto actor = targetHandle.get().get();

                    // Check if already an AI agent
                    auto already = aiam.getAgentByName(actorLabel);
                    if (already) continue;

                    // Only essential checks - no filtering by race, hostility, or creature type
                    if (actor->IsDead())
                        continue;
                    else if (!actor->Is3DLoaded())
                        continue;
                    else if (actor == player) // Don't include the player
                        continue;

                    // Check distance (use same distance settings as auto-activate for consistency)
                    float distance = player->GetPosition().GetDistance(actor->GetPosition());
                    float maxDistance = DISTANCE_ACTIVATING_NPC_OUT;
                    if (player->GetParentCell()->IsInteriorCell()) maxDistance = DISTANCE_ACTIVATING_NPC_IN;

                    if (distance > maxDistance) continue;
                    
                    // Add to our list - this includes ALL nearby actors (NPCs, creatures, etc.)
                    nonAgentList.push_back(actor);
                }
            }
        }
    }

    return nonAgentList;
}

std::vector<RE::Actor*> Papyrus::findAllNearbyActors(RE::BSScript::Internal::VirtualMachine* a_vm,
                                                     RE::VMStackID a_stackID, RE::StaticFunctionTag*, bool onlyBgL) {
    ScopedPapyrusLock lock("findAllNearbyActors");
    AIAgentManager& aiam = AIAgentManager::getInstance();

    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) return {};

    auto cell = player->GetParentCell();
    if (!cell) return {};
    std::vector<RE::Actor*> actors;
    // Minimal filtering - MCM should be an override to manually add ANY nearby NPC/creature
    if (const auto processLists = RE::ProcessLists::GetSingleton(); processLists) {
        for (auto& targetHandle : processLists->highActorHandles) {
            if (auto target = targetHandle.get(); target && target->GetActorRuntimeData().currentProcess) {
                std::string actorLabel(target->GetDisplayFullName());
                if (!actorLabel.empty()) {
                    auto actor = targetHandle.get().get();

                    // Only essential checks - no filtering by race, hostility, or creature type
                    if (actor->IsDead())
                        continue;
                    else if (!actor->Is3DLoaded())
                        continue;
                    else if (!actor->CanTalkToPlayer())
                        continue;
                    else if (actor == player)  // Don't include the player
                        continue;

                    if (onlyBgL) {
                        if (aiam.getRenamedNpcNameByFormId(actor->GetFormID())=="") {
                            logger::debug("Skipping non-BgL actor: {} (0x{:08x})", actorLabel,
                                          actor->GetFormID());
                            continue;
                        }
                    }

                    // Add to our list - this includes ALL nearby actors (NPCs, creatures, etc.)
                    actors.push_back(actor);
                }
            }
        }
    }

    return actors;
}

RE::TESObjectREFR* Papyrus::findLocationsToSafeSpawn(RE::BSScript::Internal::VirtualMachine* a_vm,
                                                     RE::VMStackID a_stackID, RE::StaticFunctionTag*, float maxdistance,
                                                     bool restriction) {
    ScopedPapyrusLock lock("findLocationsToSafeSpawn");
    RE::TESObjectREFR* ref = nullptr;
    auto player = RE::PlayerCharacter::GetSingleton();

    float minDistance = 0.0f;
    auto cell = player->GetParentCell();
    auto position = player->GetPosition();

    if (cell) {
        cell->ForEachReference([&ref, &player, &position, &minDistance, restriction,
                                &maxdistance](RE::TESObjectREFR& object) -> RE::BSContainer::ForEachResult {
            RE::TESForm* baseForm = object.GetBaseObject();
            if ((restriction == false) ||
                (baseForm->formType == RE::FormType::Armor || baseForm->formType == RE::FormType::Book ||
                 baseForm->formType == RE::FormType::Misc || baseForm->formType == RE::FormType::Weapon ||
                 baseForm->formType == RE::FormType::Scroll || baseForm->formType == RE::FormType::IdleMarker ||
                 baseForm->formType == RE::FormType::Apparatus || 
                 baseForm->formType == RE::FormType::Container ||
                 baseForm->formType == RE::FormType::Debris || 
                 baseForm->formType == RE::FormType::Book || 
                 baseForm->formType == RE::FormType::ActorCharacter ||
                 baseForm->formType == RE::FormType::Furniture ||
                 baseForm->formType == RE::FormType::NPC
                 
                    )) {
                std::string name(object.GetName());
                if (name.empty() && restriction) return RE::BSContainer::ForEachResult::kContinue;

                name.assign(object.GetName());
                if (name == "Generic Note") return RE::BSContainer::ForEachResult::kContinue;
                if (name == "Generic Amulet") return RE::BSContainer::ForEachResult::kContinue;
                if (name == "Generic Ring") return RE::BSContainer::ForEachResult::kContinue;
                if (name == "Generic Necklace") return RE::BSContainer::ForEachResult::kContinue;

                if (object.IsDeleted() || object.IsDisabled() || !object.Is3DLoaded())
                    return RE::BSContainer::ForEachResult::kContinue;

                float localDistance = position.GetDistance(object.GetPosition());
                if ((localDistance > minDistance) && (localDistance < maxdistance)) {
                    ref = &object;
                    minDistance = localDistance;
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }

    if (ref) {
        logger::info("findLocationsToSafeSpawn {} {} 0x{:08x} {}", ref->GetDisplayFullName(), minDistance,
                     ref->GetFormID(), ref->GetBaseObject()->GetFormType());
    } else {
        logger::info("Ref not found");
    }
    return ref;
}

RE::TESObjectREFR* getLocationMarkerForImpl(RE::BGSLocation* a_loc) {

    auto marker = a_loc->worldLocMarker.get();
    auto result = marker ? marker.get() : nullptr;
    return result;
}

RE::TESObjectREFR* Papyrus::getLocationMarkerFor(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                                 RE::StaticFunctionTag*, RE::BGSLocation* a_loc) {
    ScopedPapyrusLock lock("getLocationMarkerFor");
    if (!a_loc) {
        a_vm->TraceStack("Location is None", a_stackID);
        return nullptr;
    }

    
    return getLocationMarkerForImpl(a_loc);
}


RE::TESObjectREFR* getWorldLocationMarkerForImpl(RE::BGSLocation* a_loc) {
    
    logger::info("getWorldLocationMarkerFor: Location is {},{:08X}", a_loc->GetName(), a_loc->GetFormID());
    auto marker = a_loc->worldLocMarker.get();
    if (!marker) {
        logger::info("getWorldLocationMarkerFor: Location has no world marker, using horseLocMarker");
        marker = a_loc->horseLocMarker.get();
    }

    /*
    if (a_loc->GetFormID() == 0x0001f87c) { // Whiterun Stables. Location marker points to a inside cell.
        logger::info("getWorldLocationMarkerFor: Special case for Whiterun, using horseLocMarker");
        RE::TESObjectREFR *hres = RE::TESForm::LookupByID(0x00072879)->AsReference();  // Whiterun Stables Horse Marker
        return hres;
    }
    */
    if (marker && marker.get()) {
        auto worldMarker = marker.get();
        if (worldMarker->GetParentCell()) {
            if (!worldMarker->GetParentCell()->IsExteriorCell()) {
                auto currentLocation = worldMarker->GetCurrentLocation();
                if (currentLocation && currentLocation->parentLoc) {
                    auto parentLoc = currentLocation->parentLoc;
                    auto parentMarker = parentLoc->worldLocMarker.get();
                    if (parentMarker) {
                        logger::info("getWorldLocationMarkerFor: returning parent location world marker");
                        marker = parentMarker;
                    } else {
                        logger::error("getWorldLocationMarkerFor: parent location has no world marker");
                        marker = nullptr;
                    }
                } else {
                    logger::error(
                        "getWorldLocationMarkerFor: world marker is in interior cell with no parent location");
                    marker = nullptr;
                }
            } else {
                logger::info("getWorldLocationMarkerFor: world marker is in exterior cell, returning it");
                // marker = marker;
            }
        } else {
            logger::info("getWorldLocationMarkerFor: world marker has no parent cell, returning it");
            // marker = nullptr;
        }
    }

    auto result = marker ? marker.get() : nullptr;

    // Use LocationCenterMarker if no world marker found

    if (!result) {
        logger::info("getWorldLocationMarkerFor: Location has no world marker");
        RE::BSTArray<RE::SpecialRefData>* refs = &a_loc->specialRefs;

        // Iterate over specialRefs using begin()/end()
        for (auto it = refs->begin(); it != refs->end(); ++it) {
            const auto& refData = *it;
            if (!refData.type || refData.type->formType != RE::FormType::LocationRefType) {
                continue;
            }

            auto typeFormID = refData.type->GetFormID();
            if (typeFormID == 0x10f63c) {  // MapMarkerRefType
                RE::TESForm* t = RE::TESForm::LookupByID(refData.refData.refID);

                logger::info("getWorldLocationMarkerFor: Found special ref MapMarkerRefType {:08X}",
                             refData.refData.refID);

                if (!t) {
                    logger::warn("getWorldLocationMarkerFor: MapMarkerRefType {:08X} could not be resolved",
                                 refData.refData.refID);
                    continue;
                }

                auto localresult = t->AsReference();
                if (localresult) {
                    result = localresult;
                    break;
                }

                logger::warn("getWorldLocationMarkerFor: MapMarkerRefType {:08X} is not a reference",
                             refData.refData.refID);
            } else if (typeFormID == 0x1bdf1) {  // LocationCenterMarker
                RE::TESForm* t = RE::TESForm::LookupByID(refData.refData.refID);

                logger::info("getWorldLocationMarkerFor: Found special ref LocationCenterMarker {:08X}",
                             refData.refData.refID);

                if (!t) {
                    logger::warn("getWorldLocationMarkerFor: LocationCenterMarker {:08X} could not be resolved",
                                 refData.refData.refID);
                    continue;
                }

                auto localresult = t->AsReference();
                if (!localresult) {
                    logger::warn("getWorldLocationMarkerFor: LocationCenterMarker {:08X} is not a reference",
                                 refData.refData.refID);
                    continue;
                }

                auto parentCell = localresult->GetParentCell();
                if (parentCell && !parentCell->IsInteriorCell()) {
                    result = localresult;
                    break;
                }
                // If no parent cell, maybe is an interior
                // Watch this for issues it can generate
                // result = localresult; // Thhis breaks GPS coords
            }
        }
    }

    if (!result)
        logger::debug("getWorldLocationMarkerFor: returning EMPTY world marker");
    else
        logger::debug("getWorldLocationMarkerFor: marker {:08X}", result->GetFormID());
    return result;
}

RE::TESObjectREFR* Papyrus::getWorldLocationMarkerFor(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                                 RE::StaticFunctionTag*, RE::BGSLocation* a_loc) {
    ScopedPapyrusLock lock("getWorldLocationMarkerFor");
    if (!a_loc) {
        a_vm->TraceStack("Location is None", a_stackID);
        logger::error("getWorldLocationMarkerFor: Location is None");
        return nullptr;
    }
    return getWorldLocationMarkerForImpl(a_loc);
    
}

// This method have been expanded to return more than one type of marker, depending on the modifier parameter:

RE::TESObjectREFR* getLocationCenterMarkerImpl( RE::BGSLocation* a_loc, int modifier) {
    
    logger::info("getLocationCenterMarker: Location is {},{:08X}", a_loc->GetName(), a_loc->GetFormID());

    RE::TESObjectREFR* result = nullptr;

    RE::BSTArray<RE::SpecialRefData>* refs = &a_loc->specialRefs;

    auto insideMarkerRefType = RE::TESForm::LookupByID<RE::BGSLocationRefType>(0x000130fc);
    auto bossTreasureMarkerRefType = RE::TESForm::LookupByID<RE::BGSLocationRefType>(0x000130f9);  // BossTreasureMarker
    auto locationCenterRefType = RE::TESForm::LookupByID<RE::BGSLocationRefType>(0x0001bdf1);
    auto outsideEntranceMarkerRefType = RE::TESForm::LookupByID<RE::BGSLocationRefType>(0x000130fb);
    auto mapMarkerRefType = RE::TESForm::LookupByID<RE::BGSLocationRefType>(0x0010f63c);

    // Iterate over specialRefs using begin()/end()
    for (auto it = refs->begin(); it != refs->end(); ++it) {
        const auto& refData = *it;
        if (refData.type) {
            if (refData.type->formType == RE::FormType::LocationRefType) {
                if (modifier == 0) {
                    if (refData.type == locationCenterRefType) {  // LocationCenterMarker
                        RE::TESForm* t = RE::TESForm::LookupByID(refData.refData.refID);

                        logger::info("getLocationCenterMarker: Found special ref LocationCenterMarker {:08X}",
                                     refData.refData.refID);
                        if (t) {
                            result = t->AsReference();
                            break;
                        }
                    }
                } else if (modifier == 1) {
                    if (refData.type == insideMarkerRefType) {  // insideMarkerRefType
                        RE::TESForm* t = RE::TESForm::LookupByID(refData.refData.refID);
                        logger::info("getLocationCenterMarker: Found special ref insideMarkerRefType {:08X}",
                                     refData.refData.refID);
                        if (t) {
                            result = t->AsReference();
                            break;
                        }
                    }
                } else if (modifier == 2) {
                    if (refData.type == bossTreasureMarkerRefType) {  // bossTreasureMarkerRefType
                        RE::TESForm* t = RE::TESForm::LookupByID(refData.refData.refID);
                        logger::info("getLocationCenterMarker: Found special ref bossTreasureMarkerRefType {:08X}",
                                     refData.refData.refID);
                        if (t) {
                            result = t->AsReference();
                            break;
                        }
                    }
                } else if (modifier == 3) {
                    if (refData.type == outsideEntranceMarkerRefType) {  // Outside entrance marker
                        RE::TESForm* t = RE::TESForm::LookupByID(refData.refData.refID);
                        logger::info("getLocationCenterMarker: Found special ref outsideEntranceMarkerRefType {:08X}",
                                     refData.refData.refID);
                        if (t) {
                            result = t->AsReference();
                            break;
                        }
                    }
                } else if (modifier == 4) {
                    if (refData.type == mapMarkerRefType) {  // MapMarker ref type
                        RE::TESForm* t = RE::TESForm::LookupByID(refData.refData.refID);
                        logger::info("getLocationCenterMarker: Found special ref MapMarkerReftType  {:08X}",
                                     refData.refData.refID);
                        if (t) {
                            result = t->AsReference();
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!result)
        logger::debug("getLocationCenterMarker: returning EMPTY world marker");
    else
        logger::debug("getLocationCenterMarker: marker {:08X}", result->GetFormID());

    return result;
}

RE::TESObjectREFR* Papyrus::getLocationCenterMarker(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                                      RE::StaticFunctionTag*, RE::BGSLocation* a_loc, int modifier) {
    ScopedPapyrusLock lock("getLocationCenterMarker");
    if (!a_loc) {
        a_vm->TraceStack("Location is None", a_stackID);
        logger::error("getLocationCenterMarker: Location is None");
        return nullptr;
    }
    return getLocationCenterMarkerImpl(a_loc, modifier);
    
    
}

RE::TESObjectREFR* Papyrus::getNearestDoor(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                           RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("getNearestDoor");
    auto player = RE::PlayerCharacter::GetSingleton();

    auto cell = player->GetParentCell();
    RE::TESObjectREFR* buffer = nullptr;
    if (cell) {
        cell->ForEachReference([&buffer](RE::TESObjectREFR& object) -> RE::BSContainer::ForEachResult {
            RE::TESForm* baseForm = object.GetBaseObject();
            auto ref2 = &object;
            if (baseForm->formType == RE::FormType::Door) {
                auto door = baseForm->As<RE::TESObjectDOOR>();
                if (door) {
                    RE::ExtraDataList* extra = &ref2->extraList;

                    for (RE::ExtraDataList::iterator entry = extra->begin(); entry != extra->end(); ++entry) {
                        RE::BSExtraData* extraData = &(*entry);

                        if (extraData->GetType() == RE::ExtraDataType::kTeleport) {
                            RE::ExtraTeleport* extraTeleport = static_cast<RE::ExtraTeleport*>(extraData);
                            RE::TESObjectREFR* destination = extraTeleport->teleportData->linkedDoor.get().get();

                            RE::TESObjectCELL* cellDoor = destination->GetParentCell();
                            if (cellDoor) {
                                if (cellDoor->IsExteriorCell()) {
                                    buffer = destination;
                                    return RE::BSContainer::ForEachResult::kStop;
                                }
                            } else {  // Outside door?
                                auto destForm = RE::TESForm::LookupByID(destination->formID)->As<RE::TESObjectREFR>();
                                auto loc = destForm->GetCurrentLocation();

                                if (destForm->GetParentCell()) {
                                    if (destForm->GetParentCell()->IsExteriorCell()) {
                                        buffer = destForm;
                                        return RE::BSContainer::ForEachResult::kStop;
                                    }
                                } else if (loc->worldLocMarker) {
                                    buffer = loc->worldLocMarker.get().get();
                                    return RE::BSContainer::ForEachResult::kStop;
                                }
                            }
                        }
                    }
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }

    if (buffer)
        logger::info("Teleport door found {}", buffer->GetDisplayFullName());
    else
        logger::info("Teleport door not found");

    return buffer;
}



int Papyrus::sendAllVoices(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("sendAllVoices");
    // Keep legacy vanilla upload behavior, then enhance with runtime longest-sample upload.
    bool timedOut = false;
    const auto deadline = std::chrono::steady_clock::now() + kVoiceUploadBatchTimeout;
    sendAllVoice(deadline, timedOut);
    if (!timedOut) {
        sendAllVoiceLongestSamples(deadline, timedOut);
    }

    if (timedOut) {
        return 2;
    }
    return 0;
}

int Papyrus::setAIKeyWord(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                          RE::Actor* target) {
    ScopedPapyrusLock lock("setAIKeyWord");
    RE::FormID AIAgentKeyWordFormId =
        RE::TESDataHandler::GetSingleton()->LookupFormID((RE::FormID)0x217a8, "AIAgent.esp");
    AIAgentKeyWord = RE::TESForm::LookupByID(AIAgentKeyWordFormId)->As<RE::BGSKeyword>();
    if (AIAgentKeyWord) {
        RE::BGSKeywordForm* casted = target->As<RE::BGSKeywordForm>();
        if (casted) {
            casted->AddKeyword(AIAgentKeyWord);
            return 0;
        } else {
            return 2;
        }
    }
    return 1;
}

int Papyrus::testAddAllNPCAround(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("testAddAllNPCAround");
    logger::debug("Papyrus::testAddAllNPCAround - Start");

    addAllNPC();

    return 0;
}

int Papyrus::testRemoveAll(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("testRemoveAll");

    AIAgentManager& aiam = AIAgentManager::getInstance();

    // Will only clear present agents
    // for (const auto& agent : aiam.getAgents()) {
    //    agent->markToBeDeleted();
    //}
    // Will aggresively clear the agent vector
    aiam.removeAllAgents();

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

    return 0;
}

int Papyrus::recipeManager(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                           RE::FormID formid) {
    ScopedPapyrusLock lock("recipeManager");
    return 0;
}

int Papyrus::sendRequest(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                         RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("sendRequest");
    HTTPManager::log(std::format("request|{}|{}|(Context location: {}, {})", getCurrentTimeMillis(), GetGameTimeStamp(),
                                 GetPlayerLocation(), BuildCurrentWorldContextDetails()));
    return 0;
}

int Papyrus::hardResetExpression(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                 RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("hardResetExpression");
    // Deal with mfgfix */
    /* auto herika = AIAgent::getInstance().getHerikaSPG();
    if (herika) {
        logger::info("Phoneme and modifier reset");
        auto fgen = AIAgent::getInstance().getHerikaSPG()->GetFaceGenAnimationData();
        if (fgen  ) {
            RE::BSSpinLockGuard locker(fgen->lock);
            fgen->ClearExpressionOverride();
            fgen->Reset(0.0f, true, true, true, false);
            herika->GetActorRuntimeData().currentProcess->Update3DModel(herika);
            //for (int i = 0; i <= 15; i++) fgen->phenomeKeyFrame.SetValue(i, 0.0f);
        }
    }
    */
    return 0;
}

int Papyrus::shotAndUpload(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                           RE::StaticFunctionTag*, std::string hints, int mode) {
    ScopedPapyrusLock lock("shotAndUpload");
    logger::info("shotAndUpload fired, Hints: {}", hints);
    globalHints.assign(hints);

    if (mode == 1) {
        MutexSetScreenShotSendMode(0);
        MutexSetMakeShotActive(true);
    } else if (mode == 3) {
        MutexSetScreenShotSendMode(1);
        MutexSetMakeShotActive(true);
        
    } else if (mode == 0) {
        MutexSetScreenShotSendMode(0);
        MutexSetMakeShotNativeActive(true);
        RE::MenuControls::GetSingleton()->screenshotHandler->screenshotQueued = true;
    } else if (mode == 2) {
        MutexSetScreenShotSendMode(1);
        MutexSetMakeShotNativeActive(true);
        RE::MenuControls::GetSingleton()->screenshotHandler->screenshotQueued = true;
    } else if (mode == 4) {
        MutexSetScreenShotSendMode(2);
        MutexSetMakeShotNativeActive(true);
        RE::MenuControls::GetSingleton()->screenshotHandler->screenshotQueued = true;
    } else if (mode == 5) {
        MutexSetScreenShotSendMode(2);
        MutexSetMakeShotActive(true);
    }  
    return 0;
}

int Papyrus::isGameVR(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("isGameVR");
    int result = ((REL::Module::GetRuntime() == REL::Module::Runtime::VR)) ? 1 : 0;
    return result;
}


int Papyrus::isUsingFurniture(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,RE::Actor *actor) {
    ScopedPapyrusLock lock("isUsingFurniture");
    if (actor)
        if (actor->GetOccupiedFurniture()) 
            if (actor->GetOccupiedFurniture().get()) {
                logger::info("[PAPYRUS] isUsingFurniture is true for {}", actor->GetDisplayFullName());
                return 1;
            }
    
    return 0;
    
}

// Add these functions to Papyrus.cpp

int Papyrus::getInt(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                    RE::StaticFunctionTag* ,std::string key, std::string jsonStr) {
    try {
        auto j = nlohmann::json::parse(jsonStr);
        if (j.contains(key)) {
            return j.at(key).get<int>();
        }
    } catch (const std::exception& e) {
        logger::warn("[getInt] Failed to parse or find key '{}': {}", key, e.what());
    }
    return 0;
}

RE::FormID Papyrus::getForm(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                            RE::StaticFunctionTag*, std::string key, std::string jsonStr) {
    try {
        auto j = nlohmann::json::parse(jsonStr);
        if (j.contains(key)) {
            // Accept both int and hex string
            if (j.at(key).is_number_integer()) {
                return static_cast<RE::FormID>(j.at(key).get<uint32_t>());
            } else if (j.at(key).is_string()) {
                std::string val = j.at(key).get<std::string>();
                try {
                    uint32_t intval = 0;
                    intval = std::stoul(val, nullptr, 0);
                    RE::TESForm* form = RE::TESForm::LookupByID(intval);
                    if (form)
                        return static_cast<RE::FormID>(std::stoul(val, nullptr, 0));
                    else
                        logger::warn("[getForm] No form found for FormID '{}'", val);
                } catch (...) {
                }
            }
        }
    } catch (const std::exception& e) {
        logger::warn("[getForm] Failed to parse or find key '{}': {}", key, e.what());
    }
    return 0;
}


RE::Actor* Papyrus::getActor(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                            RE::StaticFunctionTag*, std::string key, std::string jsonStr) {
    try {
        auto j = nlohmann::json::parse(jsonStr);
        if (j.contains(key)) {
            // Accept both int and hex string
            uint32_t intval = 0;
            if (j.at(key).is_string()) {
                try {
                    std::string refHex = j.at(key).get<std::string>();
                    logger::info("[BackgroundCmd] Attempting to parse '{}'", refHex);
                    intval = std::stoul(refHex, nullptr, 0);
                    logger::info("[BackgroundCmd] Parsed value: 0x{:08x}", intval);
                } catch (const std::exception&) {
                    // Try parsing as hex explicitly if the first attempt fails
                    try {
                        std::string refHex = j.at(key).get<std::string>();
                        intval = std::stoul(refHex, nullptr, 16);
                        logger::info("[BackgroundCmd] Fallback hex parse: 0x{:08x}", intval);
                    } catch (const std::exception&) {
                        logger::warn("Invalid integer parameter: '{}'", jsonStr);
                        intval = 0;  // default fallback
                    }
                }
            }
            if (intval) {
                RE::TESForm* form = RE::TESForm::LookupByID(intval);
                if (form) {
                    auto reference = form->As<RE::TESObjectREFR>();
                    if (reference) {
                        auto actor = reference->As<RE::Actor>();
                        return actor;
                    }
                    else
                        logger::warn("[getActor] FormID '{}',0x{:08x} is not an Actor", intval, intval);
                } else {
                    logger::warn("[getActor] No form found for FormID '{}'", intval);
                }

            }
        }
    } catch (const std::exception& e) {
        logger::warn("[getActor] Failed to parse or find key '{}': {}", key, e.what());
    }
    return nullptr;
}


RE::TESObjectREFR* Papyrus::getReference(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                             RE::StaticFunctionTag*, std::string key, std::string jsonStr) {
    try {
        auto j = nlohmann::json::parse(jsonStr);
        if (j.contains(key)) {
            // Accept both int and hex string
            uint32_t intval = 0;
            if (j.at(key).is_string()) {
                try {
                    std::string refHex = j.at(key).get<std::string>();
                    logger::info("[BackgroundCmd] Attempting to parse '{}'", refHex);
                    intval = std::stoul(refHex, nullptr, 0);
                    logger::info("[BackgroundCmd] Parsed value: 0x{:08x}", intval);
                } catch (const std::exception&) {
                    // Try parsing as hex explicitly if the first attempt fails
                    try {
                        std::string refHex = j.at(key).get<std::string>();
                        intval = std::stoul(refHex, nullptr, 16);
                        logger::info("[BackgroundCmd] Fallback hex parse: 0x{:08x}", intval);
                    } catch (const std::exception&) {
                        logger::warn("Invalid integer parameter: '{}'", jsonStr);
                        intval = 0;  // default fallback
                    }
                }
            }
            if (intval) {
                RE::TESForm* form = RE::TESForm::LookupByID(intval);
                if (form) {
                    auto reference = form->As<RE::TESObjectREFR>();
                    if (reference) {
                        return reference;
                    } else
                        logger::warn("[getReference] FormID '{}',0x{:08x} is not an Actor", intval, intval);
                } else {
                    logger::warn("[getReference] No form found for FormID '{}'", intval);
                }
            }
        }
    } catch (const std::exception& e) {
        logger::warn("[getReference] Failed to parse or find key '{}': {}", key, e.what());
    }
    return nullptr;
}


RE::BGSListForm* Papyrus::getFormList(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                         RE::StaticFunctionTag*, std::string key, std::string jsonStr) {
    try {
        auto j = nlohmann::json::parse(jsonStr);
        if (j.contains(key)) {
            // Accept both int and hex string
            uint32_t intval = 0;
            if (j.at(key).is_string()) {
                try {
                    std::string refHex = j.at(key).get<std::string>();
                    logger::info("[BackgroundCmd] Attempting to parse '{}'", refHex);
                    intval = std::stoul(refHex, nullptr, 0);
                    logger::info("[BackgroundCmd] Parsed value: 0x{:08x}", intval);
                } catch (const std::exception&) {
                    // Try parsing as hex explicitly if the first attempt fails
                    try {
                        std::string refHex = j.at(key).get<std::string>();
                        intval = std::stoul(refHex, nullptr, 16);
                        logger::info("[BackgroundCmd] Fallback hex parse: 0x{:08x}", intval);
                    } catch (const std::exception&) {
                        logger::warn("Invalid integer parameter: '{}'", jsonStr);
                        intval = 0;  // default fallback
                    }
                }
            }
            if (intval) {
                RE::TESForm* form = RE::TESForm::LookupByID(intval);
                if (form) {
                    auto reference = form->As<RE::BGSListForm>();
                    if (reference) {
                        return reference;
                    } else
                        logger::warn("[getFormList] FormID '{}',0x{:08x} is not an Actor", intval, intval);
                } else {
                    logger::warn("[getFormList] No form found for FormID '{}'", intval);
                }
            }
        }
    } catch (const std::exception& e) {
        logger::warn("[getFormList] Failed to parse or find key '{}': {}", key, e.what());
    }
    return nullptr;
}

RE::TESEffectShader* Papyrus::getEffectShader(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                      RE::StaticFunctionTag*, std::string key, std::string jsonStr) {
    try {
        auto j = nlohmann::json::parse(jsonStr);
        if (j.contains(key)) {
            // Accept both int and hex string
            uint32_t intval = 0;
            if (j.at(key).is_string()) {
                try {
                    std::string refHex = j.at(key).get<std::string>();
                    logger::info("[BackgroundCmd] Attempting to parse '{}'", refHex);
                    intval = std::stoul(refHex, nullptr, 0);
                    logger::info("[BackgroundCmd] Parsed value: 0x{:08x}", intval);
                } catch (const std::exception&) {
                    // Try parsing as hex explicitly if the first attempt fails
                    try {
                        std::string refHex = j.at(key).get<std::string>();
                        intval = std::stoul(refHex, nullptr, 16);
                        logger::info("[BackgroundCmd] Fallback hex parse: 0x{:08x}", intval);
                    } catch (const std::exception&) {
                        logger::warn("Invalid integer parameter: '{}'", jsonStr);
                        intval = 0;  // default fallback
                    }
                }
            }
            if (intval) {
                RE::TESForm* form = RE::TESForm::LookupByID(intval);
                if (form) {
                    auto reference = form->As<RE::TESEffectShader>();
                    if (reference) {
                        return reference;
                    } else
                        logger::warn("[getEffectShader] FormID '{}',0x{:08x} is not an EffectShader", intval, intval);
                } else {
                    logger::warn("[getEffectShader] No form found for FormID '{}'", intval);
                }
            }
        }
    } catch (const std::exception& e) {
        logger::warn("[getFormList] Failed to parse or find key '{}': {}", key, e.what());
    }
    return nullptr;
}

std::string Papyrus::getString(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                               RE::StaticFunctionTag* ,std::string key, std::string jsonStr) {
    try {
        auto j = nlohmann::json::parse(jsonStr);
        if (j.contains(key)) {
            return j.at(key).get<std::string>();
        }
    } catch (const std::exception& e) {
        logger::warn("[getString] Failed to parse or find key '{}': {}", key, e.what());
    }
    return "";
}

float Papyrus::getFloat(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                        RE::StaticFunctionTag*, std::string key, std::string jsonStr) {
    try {
        auto j = nlohmann::json::parse(jsonStr);
        if (j.contains(key)) {
            return j.at(key).get<float>();
        }
    } catch (const std::exception& e) {
        logger::warn("[getFloat] Failed to parse or find key '{}': {}", key, e.what());
    }
    return 0.0f;
}

// This is a helper function to compile and run a Papyrus script. Borrowed from OstimNG

inline void CompileAndRunImpl(RE::Script* script, RE::ScriptCompiler* compiler, RE::COMPILER_NAME name,
                              RE::TESObjectREFR* targetRef) {
    using func_t = decltype(CompileAndRunImpl);
    REL::Relocation<func_t> func{RELOCATION_ID(21416, REL::Module::get().version().patch() < 1130 ? 21890 : 441582)};
    return func(script, compiler, name, targetRef);
}

int Papyrus::SayTo(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                   RE::Actor* source, RE::Actor* target, RE::TESForm* topic) {
    
    ScopedPapyrusLock lock("SayTo");
    const auto scriptFactory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
    const auto script = scriptFactory ? scriptFactory->Create() : nullptr;
    
    
    if (script) {
        logger::info("[PAPYRUS] Papyrus::SayTo - Source: {} (0x{:08x}), Target: {} (0x{:08x}), Topic: {} (0x{:08x})",
                     source ? source->GetDisplayFullName() : "None",
                     source ? source->GetFormID() : 0,
                     target ? target->GetDisplayFullName() : "None",
                     target ? target->GetFormID() : 0,
                     topic ? topic->GetName() : "None", topic ? topic->GetFormID() : 0);
        
        std::string conCommand =
            "SayTo " + std::format("{:x}", target->GetFormID()) + " " + std::format("{:x}", (topic->GetFormID()));

        logger::info("{}", conCommand);

        script->SetCommand(conCommand);

        RE::TESObjectREFR *sourceRef = source->As<RE::TESObjectREFR>();
        RE::ScriptCompiler compiler;
        CompileAndRunImpl(script, &compiler, RE::COMPILER_NAME::kSystemWindowCompiler,source);

        delete script;
    }
    return 0;
}


int Papyrus::isInContainer(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                        RE::StaticFunctionTag*, RE::TESObjectREFR *item) {
    ScopedPapyrusLock lock("GetContainer");
    
    // Wont' work, RE::TESObjectREFR implies to be in the world
    
    if (!item->loadedData) {
        logger::info("[PAPYRUS] isInContainer: Item {} is in a container", item->GetDisplayFullName());
        return 1;
    }
    logger::info("[PAPYRUS] isInContainer: Item {} is NOT in a container", item->GetDisplayFullName());
    return 0;
   
}

  std::string Papyrus::GetDoorActivationText(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                           RE::StaticFunctionTag*, RE::TESObjectREFR* ref) {
    ScopedPapyrusLock lock("GetDoorActivationText");
    RE::BSString actText;

    auto resolvedRef = ref->data.objectReference->formID;
    auto refForm = RE::TESForm::LookupByID(resolvedRef);

    RE::TESObjectDOOR* door = refForm->As<RE::TESObjectDOOR>();
    if (door) {
        door->GetActivateText(ref, actText);
        std::string locNameFull(actText);

        // With the following code:

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
        
        return locName;
    } else {
        logger::info("[PAPYRUS] GetDoorActivationText: Reference {} is not a door", ref->GetDisplayFullName());
        return "";
    }

  }


  std::vector<RE::FormID> Papyrus::findAllAgentsFormId(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                                 RE::StaticFunctionTag*) {
      ScopedPapyrusLock lock("findAllAgentsFormId");
      AIAgentManager& aiam = AIAgentManager::getInstance();

      std::vector<RE::FormID> actorList;

      for (const auto& agent : aiam.getAgents()) {
          if (agent) {
              actorList.push_back(agent->GetFormId());

          }
      }

      return actorList;
  }


RE::TESObjectREFR* Papyrus::loadReference(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                            RE::StaticFunctionTag*, int FormId) {
      
    RE::TESForm* form = RE::TESForm::LookupByID(FormId);
    if (form) {
        auto reference = form->As<RE::TESObjectREFR>();
        if (reference) {
            return reference;
        } else
            logger::warn("[loadReference] FormID '{}',0x{:08x} is not an TESObjectREFR", FormId, FormId);
    } else {
        logger::warn("[loadReference] No form found for FormID '{}',0x{:08x}", FormId, FormId);
    }

      
    return nullptr;
}
  


int sendLocationFastImpl(RE::BGSLocation* a_loc, std::string tags, RE::TESObjectCELL* referenceCell) {
    

    RE::TESObjectREFR* destMarker = getWorldLocationMarkerForImpl(a_loc);
    if (!destMarker) {
        destMarker = getLocationCenterMarkerImpl(a_loc, 0);
    }

    if (!destMarker) {
        destMarker = getLocationCenterMarkerImpl(a_loc, 1);
    }

    if (!destMarker) {
        destMarker = getLocationCenterMarkerImpl(a_loc, 3);
    }

    if (!destMarker) {
        destMarker = getLocationCenterMarkerImpl(a_loc, 4);
    }

    if (!destMarker) {
        destMarker = getLocationMarkerForImpl(a_loc);
    }

    if (!destMarker) {
        logger::error("sendLocationFast: No destination marker found for location {},{:08X}", a_loc->GetName(),
                      a_loc->GetFormID());
        return 0;
    }

    if (destMarker->IsDisabled()) {
        logger::error("sendLocationFast: Destination marker is disabled for location {},{:08X}", a_loc->GetName(),
                      a_loc->GetFormID());
        return 0;
    }

    RE::BGSLocation* currParent = nullptr;
    RE::BGSLocation* currParent2 = nullptr;
    RE::TESFaction* factionOwner = nullptr;

    currParent = a_loc->parentLoc;
    if (currParent) {
        currParent2 = currParent->parentLoc;
    }
    if (a_loc->GetFormID() == 0x0005F428) {
        logger::debug("sendLocationFast: Debug line");
        
    }
    RE::TESObjectREFR* locationCenterMarkerRef = getLocationCenterMarkerImpl(a_loc, 0);
    RE::TESObjectREFR* insideEntranceMarkerRef = getLocationCenterMarkerImpl(a_loc, 1);
    RE::TESObjectREFR* outsideEntranceMarkerRef = getLocationCenterMarkerImpl(a_loc, 3);
    RE::TESObjectREFR* mapMarkerRefType = getLocationCenterMarkerImpl(a_loc, 4);
    RE::TESObjectREFR* rawLocationMarker = getLocationMarkerForImpl(a_loc);

    RE::TESObjectCELL* localCell = nullptr;
    localCell = destMarker->GetParentCell();
    if (referenceCell) {
        localCell = referenceCell;
        logger::debug("sendLocationFast: Using reference cell {:08X} for location {},{:08X}", localCell->GetFormID(),
                      a_loc->GetName(), a_loc->GetFormID());
    }

    if (locationCenterMarkerRef && locationCenterMarkerRef->GetParentCell()) {
        localCell = locationCenterMarkerRef->GetParentCell();
        logger::debug("sendLocationFast: Using location center marker cell {:08X} for location {},{:08X}",
                      localCell->GetFormID(), a_loc->GetName(), a_loc->GetFormID());
    }

    int flags = 0;

    if (insideEntranceMarkerRef) {
        if (insideEntranceMarkerRef->GetParentCell() && insideEntranceMarkerRef->GetParentCell()->IsInteriorCell()) {
            flags += 1;  // 01
        }
    } else {
        flags += 2;  // 10
    }

    if (locationCenterMarkerRef) {
        if (locationCenterMarkerRef->GetParentCell() && locationCenterMarkerRef->GetParentCell()->IsInteriorCell()) {
            flags += 1 * 4;  // 01 << 2
        }
    } else {
        flags += 2 * 4;  // 10 << 2
    }

    if (rawLocationMarker) {
        if (rawLocationMarker->GetParentCell() && rawLocationMarker->GetParentCell()->IsInteriorCell()) {
            flags += 1 * 16;  // 01 << 4
        }
    } else {
        flags += 2 * 16;  // 10 << 4
    }

    if (outsideEntranceMarkerRef) {
        if (outsideEntranceMarkerRef->GetParentCell() && outsideEntranceMarkerRef->GetParentCell()->IsInteriorCell()) {
            flags += 1 * 64;  // 01 << 6
        }
    } else {
        flags += 2 * 64;  // 10 << 6
    }

    std::string parName = "";
    std::string parName2 = "";
    std::string types = tags;

    auto keyword = a_loc->GetKeywords();
    for (const auto& kw : keyword) {
        if (kw) {
            auto tag=GetLocationTag(kw->GetFormID());       
            if (!tag.empty()) {
                types.append(tag).append(",");
            }
        }
    }

    if (currParent) {
        parName = currParent->GetName();
    }
    if (currParent2) {
        parName2 = currParent2->GetName();
    }

    logger::debug(
        "sendLocationFast: Sending location {},{:08X} with flags {:02X}, parent {}, parent2 {}, cell {:08X}, tags {}",
        a_loc->GetName(), a_loc->GetFormID(), flags, parName, parName2, localCell ? localCell->GetFormID() : 0, tags);

    std::string specialRefs = "";

    specialRefs = GetLocationSpecialRefsStringImpl(a_loc->GetFormID());

    std::string isCleared = "0";
    if (a_loc->IsCleared()) {
        isCleared = "1";
    }

    RE::TESWorldSpace* cws = nullptr;
    std::string worldspaceName = "";
    cws = locationCenterMarkerRef ? locationCenterMarkerRef->GetWorldspace() : nullptr;
    if (!cws) cws = insideEntranceMarkerRef ? insideEntranceMarkerRef->GetWorldspace() : nullptr;
    if (!cws) cws = outsideEntranceMarkerRef ? outsideEntranceMarkerRef->GetWorldspace() : nullptr;
    if (!cws) cws = mapMarkerRefType ? mapMarkerRefType->GetWorldspace() : nullptr;

    if (cws) worldspaceName = cws->GetFullName();

    /* 
    * int result = AIAgentFunctions.logMessage(curr.GetName() + "/" + curr.GetFormID() + "/" + parName + "/" + parName2 + "/" +
    types+"/"+isInterior+"//"+destMarker.GetPositionX()+"/"+destMarker.GetPositionY()+"/"+specialRefs+"/"+isCleared+"/"+worldspaceName,"util_location_name")
    * */

    // When dealing with coordinates, we must send global world coordinates
    // Check mapMarkerRefType, and world is 0x3c,01x1a26f,1691d,0x037edf,0x16bb4,0x016d71
    // Check rawLocationMarker, and world is 0x3c,01x1a26f,1691d,0x037edf,0x16bb4,0x016d71
    // I so, send coords.
    auto isValidWorldMarker = [&](RE::TESObjectREFR* ref) -> bool {
        if (!ref) {
            return false;
        }

        auto* worldspace = ref->GetWorldspace();
        if (!worldspace) {
            return false;
        }

        constexpr std::array<RE::FormID, 6> allowedWorldspaces = {0x3C, 0x1A26F, 0x1691D, 0x037EDF, 0x016BB4, 0x016D71};

        return std::find(allowedWorldspaces.begin(), allowedWorldspaces.end(), worldspace->GetFormID()) !=
               allowedWorldspaces.end();
    };

    RE::TESObjectREFR* destMarkerWorld = nullptr;

    if (isValidWorldMarker(mapMarkerRefType)) {
        destMarkerWorld = mapMarkerRefType;
    } else if (isValidWorldMarker(rawLocationMarker)) {
        destMarkerWorld = rawLocationMarker;
    }
    
    if (!destMarkerWorld) {
        // We don't have coords. Case, Silver-Blood Inn. Interior location, no world marker. We will send 0,0 coords.
        // Let's check parent location for coords.
        if (currParent) {
            RE::TESObjectREFR* parentMarker = getWorldLocationMarkerForImpl(currParent);
            if (parentMarker && isValidWorldMarker(parentMarker)) {
                destMarkerWorld = parentMarker;
                logger::debug("sendLocationFast: Using parent location marker for world coordinates for location {},{:08X}",
                              a_loc->GetName(), a_loc->GetFormID());
            }
        } else {
            logger::warn("sendLocationFast: No valid world marker found for location {},{:08X}", a_loc->GetName(),
                         a_loc->GetFormID());
        }
    }
    float x = destMarkerWorld ? destMarkerWorld->GetPositionX() : 0.0f;
    float y = destMarkerWorld ? destMarkerWorld->GetPositionY() : 0.0f;

    std::string locName = a_loc->GetName();

    HTTPManager::log(std::format("util_location_name|{}|{}|{}/{}/{}/{}/{}/{}//{}/{}/{}/{}/{}", getCurrentTimeMillis(), GetGameTimeStamp(), 
        locName, a_loc->GetFormID(), parName,parName2, types,
                      flags, x, y, specialRefs, isCleared, worldspaceName));


    return 1;
}

int Papyrus::sendLocationFast(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                              RE::StaticFunctionTag*, RE::BGSLocation* a_loc, std::string tags,
                              RE::TESObjectCELL* referenceCell) {
    ScopedPapyrusLock lock("sendLocationFast");
    if (!a_loc) {
        a_vm->TraceStack("Location is None", a_stackID);
        logger::error("getLocationCenterMarker: Location is None");
        return -1;
    }

    return sendLocationFastImpl(a_loc, tags, referenceCell);
}

int Papyrus::sendFactionFast(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                      RE::StaticFunctionTag*, RE::TESFaction *faction,std::string name) {

    
    ScopedPapyrusLock lock("sendFactionFast");
    if (!faction) {
        a_vm->TraceStack("faction is None", a_stackID);
        logger::error("sendFactionFast: faction is None");
        return 0;
    }
    
    
    logger::info("sendFactionsFast: Sending faction {},{:08X} with name '{}'", name, faction->GetFormID(),
                    name);

    std::string vendorRef = "";
    if (name.empty()) {
        name = faction->GetFullName();
    }

    if (name.empty()) {
        name = faction->GetFormEditorID();
    }

    if (name.empty()) {
        name = faction->GetFormID() ? std::format("{:08X}", faction->GetFormID()) : "UnknownFaction";
    }

    if (name.empty()) {
        auto fDescription = faction->As<RE::TESDescription>();
        RE::BSString description;
        fDescription->GetDescription(description, nullptr);
        name.assign(description.c_str());
        name = trim(name);
    }

    if (!name.empty()) {
        std::replace(name.begin(), name.end(), '/', '_');
    }

    if (faction->vendorData.merchantContainer) {
        vendorRef = std::format("{:08X}", faction->vendorData.merchantContainer->GetFormID());
    }
    HTTPManager::log(std::format("util_faction_name|{}|{}|{:08X}/{}/{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                    faction->GetFormID(), name,vendorRef));
    
    
    return 1;
}



// Prisma UI History Panel functions
int Papyrus::toggleHistoryPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("toggleHistoryPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] toggleHistoryPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ToggleHistoryPanel();
    return 1;
}

int Papyrus::showHistoryPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("showHistoryPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] showHistoryPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ShowHistoryPanel();
    return 1;
}

int Papyrus::hideHistoryPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("hideHistoryPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] hideHistoryPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::HideHistoryPanel();
    return 1;
}

int Papyrus::focusHistoryPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*, bool pauseGame) {
    ScopedPapyrusLock lock("focusHistoryPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] focusHistoryPanel: Prisma UI not available");
        return 0;
    }
    
    return PrismaUIBridge::FocusHistoryPanel(pauseGame) ? 1 : 0;
}

int Papyrus::unfocusHistoryPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("unfocusHistoryPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] unfocusHistoryPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::UnfocusHistoryPanel();
    return 1;
}

int Papyrus::toggleHistoryPanelFocus(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*, bool pauseGame) {
    ScopedPapyrusLock lock("toggleHistoryPanelFocus");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] toggleHistoryPanelFocus: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ToggleHistoryPanelFocus(pauseGame);
    return 1;
}

int Papyrus::toggleOverlayPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("toggleOverlayPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] toggleOverlayPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ToggleOverlayPanel();
    return 1;
}

int Papyrus::cycleOverlayStatusPanels(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("cycleOverlayStatusPanels");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] cycleOverlayStatusPanels: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::CycleOverlayStatusPanels();
    return 1;
}

int Papyrus::cycleHistoryDiariesPanels(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("cycleHistoryDiariesPanels");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] cycleHistoryDiariesPanels: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::CycleHistoryDiariesPanels();
    return 1;
}

int Papyrus::showOverlayPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("showOverlayPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] showOverlayPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ShowOverlayPanel();
    return 1;
}

int Papyrus::hideOverlayPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("hideOverlayPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] hideOverlayPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::HideOverlayPanel();
    return 1;
}

int Papyrus::toggleDiariesPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("toggleDiariesPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] toggleDiariesPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ToggleDiariesPanel();
    return 1;
}

int Papyrus::showDiariesPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("showDiariesPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] showDiariesPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ShowDiariesPanel();
    return 1;
}

int Papyrus::hideDiariesPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("hideDiariesPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] hideDiariesPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::HideDiariesPanel();
    return 1;
}

int Papyrus::toggleBrowserPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("toggleBrowserPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] toggleBrowserPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ToggleBrowserPanel();
    return 1;
}

int Papyrus::showBrowserPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("showBrowserPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] showBrowserPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ShowBrowserPanel();
    return 1;
}

int Papyrus::hideBrowserPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("hideBrowserPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] hideBrowserPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::HideBrowserPanel();
    return 1;
}

int Papyrus::toggleAIViewPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("toggleAIViewPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] toggleAIViewPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ToggleAIViewPanel();
    return 1;
}

int Papyrus::showAIViewPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("showAIViewPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] showAIViewPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ShowAIViewPanel();
    return 1;
}

int Papyrus::hideAIViewPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("hideAIViewPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] hideAIViewPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::HideAIViewPanel();
    return 1;
}

int Papyrus::toggleDebuggerPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("toggleDebuggerPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] toggleDebuggerPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ToggleDebuggerPanel();
    return 1;
}

int Papyrus::showDebuggerPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("showDebuggerPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] showDebuggerPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ShowDebuggerPanel();
    return 1;
}

int Papyrus::hideDebuggerPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("hideDebuggerPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] hideDebuggerPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::HideDebuggerPanel();
    return 1;
}

int Papyrus::toggleStatusHUDPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("toggleStatusHUDPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] toggleStatusHUDPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ToggleStatusHUDPanel();
    return 1;
}

int Papyrus::showStatusHUDPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("showStatusHUDPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] showStatusHUDPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ShowStatusHUDPanel();
    return 1;
}

int Papyrus::hideStatusHUDPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("hideStatusHUDPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] hideStatusHUDPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::HideStatusHUDPanel();
    return 1;
}

int Papyrus::toggleChatboxPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("toggleChatboxPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] toggleChatboxPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ToggleChatboxPanel();
    return 1;
}

int Papyrus::showChatboxPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("showChatboxPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] showChatboxPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ShowChatboxPanel();
    return 1;
}

int Papyrus::hideChatboxPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("hideChatboxPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] hideChatboxPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::HideChatboxPanel();
    return 1;
}

int Papyrus::focusChatboxPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("focusChatboxPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] focusChatboxPanel: Prisma UI not available");
        return 0;
    }
    
    bool success = PrismaUIBridge::FocusChatboxPanel();
    return success ? 1 : 0;
}

int Papyrus::unfocusChatboxPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("unfocusChatboxPanel");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] unfocusChatboxPanel: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::UnfocusChatboxPanel();
    return 1;
}

int Papyrus::isChatboxPanelVisible(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("isChatboxPanelVisible");
    
    if (!PrismaUIBridge::IsAvailable()) {
        return 0;
    }
    
    return PrismaUIBridge::IsChatboxPanelVisible() ? 1 : 0;
}

int Papyrus::isChatboxPanelFocused(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("isChatboxPanelFocused");
    
    if (!PrismaUIBridge::IsAvailable()) {
        return 0;
    }
    
    return PrismaUIBridge::IsChatboxPanelFocused() ? 1 : 0;
}

int Papyrus::isAnyPrismaHotkeyPanelFocused(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("isAnyPrismaHotkeyPanelFocused");

    if (!PrismaUIBridge::IsAvailable()) {
        return 0;
    }

    return PrismaUIBridge::IsAnyHotkeyPanelFocused() ? 1 : 0;
}

int Papyrus::toggleSettingsMenu(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("toggleSettingsMenu");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] toggleSettingsMenu: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ToggleSettingsMenu();
    return 1;
}

std::string Papyrus::getSettingsMenuPendingAction(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("getSettingsMenuPendingAction");
    
    if (!PrismaUIBridge::IsAvailable()) {
        return "";
    }
    
    return PrismaUIBridge::GetPendingSettingsAction();
}

int Papyrus::clearSettingsMenuPendingAction(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("clearSettingsMenuPendingAction");
    
    if (!PrismaUIBridge::IsAvailable()) {
        return 0;
    }
    
    PrismaUIBridge::ClearPendingSettingsAction();
    return 1;
}

int Papyrus::toggleMasterMenu(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*) {
    ScopedPapyrusLock lock("toggleMasterMenu");
    
    if (!PrismaUIBridge::IsAvailable()) {
        logger::warn("[PAPYRUS] toggleMasterMenu: Prisma UI not available");
        return 0;
    }
    
    PrismaUIBridge::ToggleMasterMenu();
    return 1;
}

int Papyrus::startMusicScene(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                             RE::StaticFunctionTag*, std::string songName, std::string singerName) {
    ScopedPapyrusLock lock("startMusicScene");
    AIAgentManager& aiam = AIAgentManager::getInstance();
    auto actorPtr = aiam.getAgentByName(singerName);
    if (actorPtr) {
        MusicManager::getInstance().stopSong();
        if (MusicManager::getInstance().downloadSong(songName)) {
            logger::info("[Command] Playing song: {}", songName);
            MusicManager::getInstance().playSong(actorPtr.get());
        } else {
            logger::error(
                "[Command] Failed to download "
                "song: {}",
                songName);
        }
    } else {
        logger::error("[Command] No agent found with name: {}", singerName);
        return -1;
    }

    return 0;
}

int Papyrus::stopMusicScene(RE::BSScript::Internal::VirtualMachine * a_vm, RE::VMStackID a_stackID,
                                RE::StaticFunctionTag*, std::string singerName) {
    ScopedPapyrusLock lock("stopMusicScene");
    MusicManager::getInstance().stopSong();
    return 0;
}



std::string Papyrus::GetLocationSpecialRefsString(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                                  RE::StaticFunctionTag*, RE::FormID a_locationFormID) {
    return GetLocationSpecialRefsStringImpl(a_locationFormID);
}

int Papyrus::PostGameData(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                          std::string jsondata) {
    
    //logger::debug("[PostGameData] Received JSON data: {}", jsondata);
    // Unescape single quotes to double quotes for valid JSON
    std::string jsondataUnescaped = jsondata;
    std::replace(jsondataUnescaped.begin(), jsondataUnescaped.end(), '|', '"');
    //logger::debug("[PostGameData] Received JSON data: {}", jsondataUnescaped);
    json finalData = nlohmann::json::parse(jsondataUnescaped, nullptr, false);
    HTTPManager::postGameData("gamedata.php", finalData);
    
    return 0;
}


namespace
{
int StartPlayerMenuDialogueTTSUnlocked(std::string fallbackText) {
    if (!PlayerTtsTraditionalDialogueEnabled) {
        return 0;
    }

    SkipNextPlayerMenuTopicLocalPlayback();

    if (g_playerMenuDialogueTtsActive.exchange(true)) {
        return 1;
    }

    std::string selectedLine = ResolvePlayerMenuDialogueLine(fallbackText);
    if (selectedLine.empty()) {
        logger::warn("[PlayerMenuTTS] No player dialogue line available for menu playback");
        g_playerMenuDialogueTtsActive.store(false);
        return 0;
    }

    auto& speakManager = SpeakManager::getInstance();
    if (speakManager.getProcessing()) {
        logger::warn("[PlayerMenuTTS] SpeakManager busy; cannot start player dialogue-menu TTS for '{}'",
                     selectedLine);
        g_playerMenuDialogueTtsActive.store(false);
        return 0;
    }

    speakManager.clearPlayerPlaybackCompletedCallback();
    speakManager.deleteQueuedPlayerLines();
    const std::uint64_t requestId = g_playerMenuDialogueTtsRequestId.fetch_add(1) + 1;
    speakManager.setPlayerPlaybackCompletedCallback([requestId](const ScriptLine&, int) {
        if (TryFinishPlayerMenuDialogueTtsRequest(requestId)) {
            QueuePlayerMenuFinishedModEvent();
        }
    });

    try {
        QueuePlayerMenuDialogueTtsStartTimeout(requestId, selectedLine);
        ThreadPool::getInstance().enqueue("RequestPlayerMenuTTS", [selectedLine, requestId]() {
            const bool queued = RequestPlayerMenuTtsPlayFromServer(selectedLine, requestId);
            if (!queued) {
                logger::warn(
                    "[PlayerMenuTTS] player_menu_tts_play did not queue a player line for '{}'; releasing held topic",
                    selectedLine);
                ReleasePlayerMenuDialogueTtsRequest(
                    requestId,
                    selectedLine,
                    "player_menu_tts_play did not queue a player line",
                    true);
                return;
            }
        }, PlayerMenuDialogueTtsTaskKey(requestId));
    } catch (const std::exception& e) {
        logger::error("[PlayerMenuTTS] Failed to enqueue player dialogue-menu request: {}", e.what());
        speakManager.clearPlayerPlaybackCompletedCallback();
        speakManager.deleteQueuedPlayerLines();
        g_playerMenuDialogueTtsActive.store(false);
        return 0;
    }
    return 1;
}
}

int Papyrus::startPlayerMenuDialogueTTS(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                        RE::StaticFunctionTag*, std::string fallbackText) {
    ScopedPapyrusLock lock("startPlayerMenuDialogueTTS");
    return StartPlayerMenuDialogueTTSUnlocked(std::move(fallbackText));
}

int Papyrus::startPlayerMenuDialogueTTSNative(std::string fallbackText) {
    ScopedPapyrusLock lock("startPlayerMenuDialogueTTSNative");
    return StartPlayerMenuDialogueTTSUnlocked(std::move(fallbackText));
}

void Papyrus::releasePlayerMenuTopicTimer(std::string reason) {
    QueuePlayerMenuTopicTimerOff(std::move(reason));
}

int Papyrus::scanActorsAroundOffline(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                     RE::StaticFunctionTag*, RE::Actor* target) {
    
    ScopedPapyrusLock lock("scanActorsAroundOffline");
        // Run GetLowProcessActorNamesFromRef in a separate thread - no need to wait for result
    ThreadPool::getInstance().enqueue(
        "ScanActorsAroundOffline",
        [target]() {
            GetLowProcessActorNamesFromRef(target);
            // Result is handled internally by GetLowProcessActorNamesFromRef
        },
        target ? target->GetDisplayFullName() : "unknown");

    return 0;
}

int addBasicProfileReal(RE::ObjectRefHandle targetObject) {
    if (targetObject) {
        auto rawTarget = targetObject.get();
        // logger::info("Checking {}", rawTarget->GetFormType());

        if (targetObject.get()->GetFormType() == RE::FormType::ActorCharacter) {
            auto targetActor = targetObject.get()->As<RE::Actor>();

            if (!targetActor->IsPlayerTeammate() && false) {
                logger::info("{} is not a PlayerTeammate", targetObject.get()->GetName());

            } else if (targetActor->IsPlayer()) {
                logger::info("{} is  the player, refusing", targetObject.get()->GetName());

            } else if (!targetActor->GetRace()->GetPlayable()) {
                logger::info("{} is not playable, refusing", targetObject.get()->GetName());

            } else {
                std::string category;

                auto baseActor = targetActor->GetActorBase();

                if (baseActor) {
                    category.assign(baseActor->GetName());
                    std::string metainfo;
                    if (baseActor->GetSex() == RE::SEX::kMale) {
                        metainfo.append("@male");
                    } else if (baseActor->GetSex() == RE::SEX::kFemale) {
                        metainfo.append("@female");
                    } else {
                        metainfo.append("@nogender");
                    }

                    std::string race;
                    if (baseActor->GetRace()->GetName()) {
                        metainfo.append("@").append(baseActor->GetRace()->GetName());
                    }

                    metainfo.append("@").append(std::format("{:08X}", targetActor->GetFormID()));

                    /* Stats gathering */
                    auto stats = targetActor->AsActorValueOwner();

                    float archery = stats->GetActorValue(RE::ActorValue::kArchery);
                    float block = stats->GetActorValue(RE::ActorValue::kBlock);
                    float onehanded = stats->GetActorValue(RE::ActorValue::kOneHanded);
                    float twohanded = stats->GetActorValue(RE::ActorValue::kTwoHanded);

                    float conjuration = stats->GetActorValue(RE::ActorValue::kConjuration);
                    float destruction = stats->GetActorValue(RE::ActorValue::kDestruction);
                    float restoration = stats->GetActorValue(RE::ActorValue::kRestoration);
                    float alteration = stats->GetActorValue(RE::ActorValue::kAlteration);
                    float illusion = stats->GetActorValue(RE::ActorValue::kIllusion);  // Use your illusion, great album

                    float heavyarmor = stats->GetActorValue(RE::ActorValue::kHeavyArmor);
                    float lightarmor = stats->GetActorValue(RE::ActorValue::kLightArmor);

                    float lockpicking = stats->GetActorValue(RE::ActorValue::kLockpicking);
                    float pickpocket = stats->GetActorValue(RE::ActorValue::kPickpocket);
                    float sneak = stats->GetActorValue(RE::ActorValue::kSneak);

                    float speech = stats->GetActorValue(RE::ActorValue::kSpeech);
                    float smithing = stats->GetActorValue(RE::ActorValue::kSmithing);
                    float alchemy = stats->GetActorValue(RE::ActorValue::kAlchemy);
                    float enchanting = stats->GetActorValue(RE::ActorValue::kEnchanting);  // enchanting? enchaaantiing

                    metainfo.append("@").append(std::format("{}", archery));
                    metainfo.append("@").append(std::format("{}", block));
                    metainfo.append("@").append(std::format("{}", onehanded));
                    metainfo.append("@").append(std::format("{}", twohanded));
                    metainfo.append("@").append(std::format("{}", conjuration));
                    metainfo.append("@").append(std::format("{}", destruction));
                    metainfo.append("@").append(std::format("{}", restoration));
                    metainfo.append("@").append(std::format("{}", alteration));
                    metainfo.append("@").append(std::format("{}", illusion));
                    metainfo.append("@").append(std::format("{}", heavyarmor));
                    metainfo.append("@").append(std::format("{}", lightarmor));
                    metainfo.append("@").append(std::format("{}", lockpicking));
                    metainfo.append("@").append(std::format("{}", pickpocket));
                    metainfo.append("@").append(std::format("{}", sneak));
                    metainfo.append("@").append(std::format("{}", speech));
                    metainfo.append("@").append(std::format("{}", smithing));
                    metainfo.append("@").append(std::format("{}", alchemy));
                    metainfo.append("@").append(std::format("{}", enchanting));

                    /* Equipment gathering (10 slots: helmet, armor, boots, gloves, amulet, ring, cape, backpack,
                     * left hand, right hand) */
                    auto* helmet = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kHead);
                    auto* armor = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody);
                    auto* boots = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kFeet);
                    auto* gloves = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kHands);
                    auto* amulet = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kAmulet);
                    auto* ring = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kRing);
                    auto* cape = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kModChestPrimary);
                    auto* backpack = targetActor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kModBack);
                    auto* leftHand = targetActor->GetEquippedObject(true);    // Left hand
                    auto* rightHand = targetActor->GetEquippedObject(false);  // Right hand

                    metainfo.append("@").append(helmet ? helmet->GetName() : "");
                    metainfo.append("@").append(armor ? armor->GetName() : "");
                    metainfo.append("@").append(boots ? boots->GetName() : "");
                    metainfo.append("@").append(gloves ? gloves->GetName() : "");
                    metainfo.append("@").append(amulet ? amulet->GetName() : "");
                    metainfo.append("@").append(ring ? ring->GetName() : "");
                    metainfo.append("@").append(cape ? cape->GetName() : "");
                    metainfo.append("@").append(backpack ? backpack->GetName() : "");
                    metainfo.append("@").append(leftHand ? leftHand->GetName() : "");
                    metainfo.append("@").append(rightHand ? rightHand->GetName() : "");

                    /* Stats gathering (core attributes) */
                    auto level = targetActor->GetLevel();
                    float health = stats->GetActorValue(RE::ActorValue::kHealth);
                    float healthMax = stats->GetBaseActorValue(RE::ActorValue::kHealth);
                    float magicka = stats->GetActorValue(RE::ActorValue::kMagicka);
                    float magickaMax = stats->GetBaseActorValue(RE::ActorValue::kMagicka);
                    float stamina = stats->GetActorValue(RE::ActorValue::kStamina);
                    float staminaMax = stats->GetBaseActorValue(RE::ActorValue::kStamina);
                    float scale = targetActor->GetScale();

                    metainfo.append("@").append(std::format("{}", level));
                    metainfo.append("@").append(std::format("{}", health));
                    metainfo.append("@").append(std::format("{}", healthMax));
                    metainfo.append("@").append(std::format("{}", magicka));
                    metainfo.append("@").append(std::format("{}", magickaMax));
                    metainfo.append("@").append(std::format("{}", stamina));
                    metainfo.append("@").append(std::format("{}", staminaMax));
                    metainfo.append("@").append(std::format("{:.2f}", scale));

                    auto* modFiles = targetActor->sourceFiles.array;
                    if (!modFiles) {
                        if (targetActor->GetActorBase()) modFiles = targetActor->GetActorBase()->sourceFiles.array;
                    }

                    if (modFiles && modFiles->size() > 0) {
                        metainfo.append("@");
                        for (std::uint32_t i = 0; i < modFiles->size(); ++i) {
                            RE::TESFile* file = (*modFiles)[i];
                            if (file) {
                                if (i > 0) metainfo.append("#");
                                metainfo.append(file->fileName);
                            }
                        }
                    } else {
                        metainfo.append("@");
                    }

                    /* Faction gathering */
                    std::string factionData;
                    if (baseActor && baseActor->factions.size() > 0) {
                        for (std::uint32_t i = 0; i < baseActor->factions.size(); ++i) {
                            auto factionInfo = baseActor->factions[i];
                            if (factionInfo.faction) {
                                if (!factionData.empty()) factionData.append("#");
                                std::string stableFactionReference;
                                if (auto* factionFile = factionInfo.faction->GetFile(0)) {
                                    const std::string pluginName(factionFile->GetFilename());
                                    const auto localFormId =
                                        static_cast<std::uint32_t>(factionInfo.faction->GetLocalFormID());
                                    if (!pluginName.empty()) {
                                        stableFactionReference = std::format("{}/{:08X}", pluginName, localFormId);
                                    }
                                }
                                // Format: formID:rank:PluginName.esp|LocalFormId
                                factionData.append(std::format("{:08X}:{:d}:{}", factionInfo.faction->GetFormID(),
                                                               static_cast<int>(factionInfo.rank),
                                                               stableFactionReference));
                            }
                        }
                    }
                    metainfo.append("@").append(factionData);

                    /* Class gathering */
                    std::string classData;
                    if (baseActor && baseActor->npcClass) {
                        auto npcClass = baseActor->npcClass;
                        std::string className = npcClass->GetName() ? npcClass->GetName() : "";

                        // Get training data from class data
                        std::string trainSkill = "";
                        int trainLevel = 0;

                        // TESClass data structure contains training info
                        // Check if maximumTrainingLevel is greater than 0 to determine if this class trains
                        if (npcClass->data.maximumTrainingLevel > 0) {
                            // Convert CLASS_DATA::Skill to skill name string
                            auto skillValue = npcClass->data.teaches;
                            switch (skillValue.underlying()) {
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kOneHanded):
                                    trainSkill = "OneHanded";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kTwoHanded):
                                    trainSkill = "TwoHanded";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kArchery):
                                    trainSkill = "Archery";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kBlock):
                                    trainSkill = "Block";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kSmithing):
                                    trainSkill = "Smithing";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kHeavyArmor):
                                    trainSkill = "HeavyArmor";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kLightArmor):
                                    trainSkill = "LightArmor";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kPickpocket):
                                    trainSkill = "Pickpocket";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kLockpicking):
                                    trainSkill = "Lockpicking";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kSneak):
                                    trainSkill = "Sneak";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kAlchemy):
                                    trainSkill = "Alchemy";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kSpeech):
                                    trainSkill = "Speech";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kAlteration):
                                    trainSkill = "Alteration";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kConjuration):
                                    trainSkill = "Conjuration";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kDestruction):
                                    trainSkill = "Destruction";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kIllusion):
                                    trainSkill = "Illusion";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kRestoration):
                                    trainSkill = "Restoration";
                                    break;
                                case static_cast<uint8_t>(RE::CLASS_DATA::Skill::kEnchanting):
                                    trainSkill = "Enchanting";
                                    break;
                                default:
                                    trainSkill = "";
                                    break;
                            }
                            trainLevel = static_cast<int>(npcClass->data.maximumTrainingLevel);
                        }

                        classData =
                            std::format("{}:{:08X}:{}:{}", className, npcClass->GetFormID(), trainSkill, trainLevel);
                    }
                    metainfo.append("@").append(classData);

                    category.append(metainfo);

                    HTTPManager::log(std::format("addbgnpc|{}|{}|{}@{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                 targetActor->GetDisplayFullName(), category));
                }
            }

        } else {
            logger::info("{} is not an NPC", targetObject.get()->GetName());
        }
    }
    return 0;
}


int Papyrus::addBasicProfile(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                     RE::StaticFunctionTag*, RE::Actor* target) {
    ScopedPapyrusLock lock("addBasicProfile");
    
    if (target->GetHandle()) {
        addBasicProfileReal(target->GetHandle());
        RefreshAIAgentInventoryImpl(target, target->GetDisplayFullName(), true, true);
    } else {
        logger::warn("[addBasicProfile] Target actor has no valid handle.");
    }
    return 0;
}

 int Papyrus::updateRemoteInventory(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                    RE::StaticFunctionTag*,RE::Actor* target) {

     ScopedPapyrusLock lock("updateRemoteInventory");

     if (target->GetHandle()) {
         RefreshAIAgentInventoryImpl(target, target->GetDisplayFullName(), true, false);
     } else {
         logger::warn("[addBasicProfile] Target actor has no valid handle.");
     }
     return 0;
}


 int Papyrus::sendNPCFast(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                         RE::Actor* actor) {

    ScopedPapyrusLock lock("sendNPCFast");
    
    if (!actor) {
        a_vm->TraceStack("actor is Empty", a_stackID);
        logger::error("sendNPCFast: actor is Empty");
        return -1;
    }
    int i = 1;
    //for (const auto& actor : actorList) {
        RE::Actor* actorPtr = actor ? actor->As<RE::Actor>() : nullptr;
        
        // Avoid sending too many actors at once to prevent potential performance issues
        // std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (i % 500 == 0) {
            logger::info("sendNPCFast: Processed {} actors", i);
        }

        if (!actorPtr) {
            logger::warn("sendNPCFast: Encountered a null actor in the list");
            return 0;
        }

        if (actorPtr->GetFormType() != RE::FormType::ActorCharacter) {
            logger::warn("sendNPCFast: Actor {} (0x{:08X}) is not an ActorCharacter, skipping",
                         actorPtr->GetDisplayFullName(), actorPtr->GetFormID());
            return 0;
        }

        if (actorPtr->IsDisabled()) {
            logger::warn("sendNPCFast: Actor {} (0x{:08X}) is disabled, skipping", actorPtr->GetDisplayFullName(),
                         actorPtr->GetFormID());
            return 0;
        }

        
        
        if (actorPtr->GetActorBase() && actorPtr->GetActorBase()->IsUnique()) {
            
            addBasicProfileReal(actorPtr->GetHandle());
                  
            RE::TESObjectCELL* cell = actorPtr->GetParentCell();
            RE::BGSLocation* loc = actorPtr->GetCurrentLocation();
            if (loc)
                sendLocationFastImpl(loc, "", cell);

            return i;
        }

        return 0;
    //}

    
}


 int Papyrus::removeFromRenamedNPCList(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                      RE::StaticFunctionTag*, RE::Actor *actor) {
    ScopedPapyrusLock lock("removeFromRenamedNPCList");
    if (!actor) {
        logger::error("removeFromRenamedNPCList, no actor");
    }

    AIAgentManager& aiam = AIAgentManager::getInstance();
    logger::info("Removing actor {} from renamed list", actor->GetDisplayFullName());
    aiam.removeRenamedNpcByFormId(actor->GetFormID());

    return 0;


}

bool Papyrus::RegisterSGPFuncs(RE::BSScript::IVirtualMachine* a_vm) {
    a_vm->RegisterFunction("sendMessage", "AIAgentFunctions", sendMessage, false);
    a_vm->RegisterFunction("sendMessageToActor", "AIAgentFunctions", sendMessageToActor, false);
    a_vm->RegisterFunction("commandEnded", "AIAgentFunctions", commandEnded, false);
    a_vm->RegisterFunction("commandEndedForActor", "AIAgentFunctions", commandEndedForActor, false);
    a_vm->RegisterFunction("getHerikaFormId", "AIAgentFunctions", getHerikaFormId, false);
    a_vm->RegisterFunction("recordSoundEx", "AIAgentFunctions", recordSoundEx, false);
    a_vm->RegisterFunction("logMessage", "AIAgentFunctions", logMessage, false);
    a_vm->RegisterFunction("setAnimationBusy", "AIAgentFunctions", setAnimationBusy, false);
    a_vm->RegisterFunction("setLocked", "AIAgentFunctions", setLocked, false);
    a_vm->RegisterFunction("isActorTalking", "AIAgentFunctions", isActorTalking, false);
    a_vm->RegisterFunction("getPlayerBountyForGuard", "AIAgentFunctions", getPlayerBountyForGuard, false);
    a_vm->RegisterFunction("requestMoveInventoryItemConfirmation", "AIAgentFunctions",
                           requestMoveInventoryItemConfirmation, false);
    a_vm->RegisterFunction("requestArrestConfirmation", "AIAgentFunctions", requestArrestConfirmation, false);
    a_vm->RegisterFunction("sendRequest", "AIAgentFunctions", sendRequest, false);
    a_vm->RegisterFunction("stopRecording", "AIAgentFunctions", stopRecording, false);
    a_vm->RegisterFunction("startOpenMicMonitoring", "AIAgentFunctions", startOpenMicMonitoring, false);
    a_vm->RegisterFunction("stopOpenMicMonitoring", "AIAgentFunctions", stopOpenMicMonitoring, false);
    a_vm->RegisterFunction("setOpenMicMuted", "AIAgentFunctions", setOpenMicMuted, false);
    a_vm->RegisterFunction("getCurrentRecordingDeviceName", "AIAgentFunctions", getCurrentRecordingDeviceName, false);
    a_vm->RegisterFunction("beginChimMcmSnapshot", "AIAgentFunctions", beginChimMcmSnapshot, false);
    a_vm->RegisterFunction("publishChimMcmEntry", "AIAgentFunctions", publishChimMcmEntry, false);
    a_vm->RegisterFunction("commitChimMcmSnapshot", "AIAgentFunctions", commitChimMcmSnapshot, false);
    a_vm->RegisterFunction("beginChimMcmAgents", "AIAgentFunctions", beginChimMcmAgents, false);
    a_vm->RegisterFunction("publishChimMcmAgent", "AIAgentFunctions", publishChimMcmAgent, false);
    a_vm->RegisterFunction("commitChimMcmAgents", "AIAgentFunctions", commitChimMcmAgents, false);
    a_vm->RegisterFunction(
        "publishChimMcmCommandResult", "AIAgentFunctions", publishChimMcmCommandResult, false);
    a_vm->RegisterFunction("requestMessage", "AIAgentFunctions", requestMessage, false);
    a_vm->RegisterFunction("requestMessageForActor", "AIAgentFunctions", requestMessageForActor, false);
    a_vm->RegisterFunction("logMessageForActor", "AIAgentFunctions", logMessageForActor, false);
    a_vm->RegisterFunction("hardResetExpression", "AIAgentFunctions", hardResetExpression, false);
    a_vm->RegisterFunction("shotAndUpload", "AIAgentFunctions", shotAndUpload, false);
    a_vm->RegisterFunction("isGameVR", "AIAgentFunctions", isGameVR, false);
    a_vm->RegisterFunction("setConf", "AIAgentFunctions", setConf, false);
    a_vm->RegisterFunction("get_conf_i", "AIAgentFunctions", get_conf_i, false);
    a_vm->RegisterFunction("setDrivenByAI", "AIAgentFunctions", setDrivenByAI, false);
    a_vm->RegisterFunction("setDrivenByAIA", "AIAgentFunctions", setDrivenByAIA, false);
    a_vm->RegisterFunction("addBasicProfile", "AIAgentFunctions", addBasicProfile, false);
    a_vm->RegisterFunction("setNewActionMode", "AIAgentFunctions", setNewActionMode, false);
    a_vm->RegisterFunction("getClosestAgent", "AIAgentFunctions", getClosestAgent, false);
    a_vm->RegisterFunction("getAgentByName", "AIAgentFunctions", getAgentByName, false);
    a_vm->RegisterFunction("getLocationMarkerFor", "AIAgentFunctions", getLocationMarkerFor, false);
    a_vm->RegisterFunction("getWorldLocationMarkerFor", "AIAgentFunctions", getWorldLocationMarkerFor, false);
    a_vm->RegisterFunction("getLocationCenterMarker", "AIAgentFunctions", getLocationCenterMarker, false);
    a_vm->RegisterFunction("sendAllVoices", "AIAgentFunctions", sendAllVoices, false);
    a_vm->RegisterFunction("findAllNearbyAgents", "AIAgentFunctions", findAllNearbyAgents, false);
    a_vm->RegisterFunction("findAllAgents", "AIAgentFunctions", findAllAgents, false);
    a_vm->RegisterFunction("findAllNearbyNonAgents", "AIAgentFunctions", findAllNearbyNonAgents, false);
    a_vm->RegisterFunction("setAIKeyWord", "AIAgentFunctions", setAIKeyWord, false);
    a_vm->RegisterFunction("removeAgentByName", "AIAgentFunctions", removeAgentByName, false);
    a_vm->RegisterFunction("testAddAllNPCAround", "AIAgentFunctions", testAddAllNPCAround, false);
    a_vm->RegisterFunction("testRemoveAll", "AIAgentFunctions", testRemoveAll, false);
    a_vm->RegisterFunction("getNearestDoor", "AIAgentFunctions", getNearestDoor, false);
    a_vm->RegisterFunction("findLocationsToSafeSpawn", "AIAgentFunctions", findLocationsToSafeSpawn, false);
    a_vm->RegisterFunction("isUsingFurniture", "AIAgentFunctions", isUsingFurniture, false);
    a_vm->RegisterFunction("findAllNearbyActors", "AIAgentFunctions", findAllNearbyActors, false);
    a_vm->RegisterFunction("findAllAgentsFormId", "AIAgentFunctions", findAllAgentsFormId, false);

    a_vm->RegisterFunction("jsonGetInt", "AIAgentFunctions", getInt, false);
    a_vm->RegisterFunction("jsonGetFormId", "AIAgentFunctions", getForm, false);
    a_vm->RegisterFunction("jsonGetString", "AIAgentFunctions", getString, false);
    a_vm->RegisterFunction("jsonGetFloat", "AIAgentFunctions", getFloat, false);
    a_vm->RegisterFunction("jsonGetActor", "AIAgentFunctions", getActor, false);
    a_vm->RegisterFunction("jsonGetReference", "AIAgentFunctions", getReference, false);
    a_vm->RegisterFunction("jsonGetFormList", "AIAgentFunctions", getFormList, false);
    a_vm->RegisterFunction("jsonGetEffectShader", "AIAgentFunctions", getEffectShader, false);

    a_vm->RegisterFunction("SayTo", "AIAgentFunctions", SayTo, false);
    a_vm->RegisterFunction("isInContainer", "AIAgentFunctions", isInContainer, false);
    a_vm->RegisterFunction("GetDoorActivationText", "AIAgentFunctions", GetDoorActivationText, false);
    a_vm->RegisterFunction("GetLocationSpecialRefsString", "AIAgentFunctions", GetLocationSpecialRefsString, false);
    a_vm->RegisterFunction("loadReference", "AIAgentFunctions", loadReference, false);
    a_vm->RegisterFunction("PostGameData", "AIAgentFunctions", PostGameData, false);
    
    // Prisma UI History Panel functions
    a_vm->RegisterFunction("toggleHistoryPanel", "AIAgentFunctions", toggleHistoryPanel, false);
    a_vm->RegisterFunction("showHistoryPanel", "AIAgentFunctions", showHistoryPanel, false);
    a_vm->RegisterFunction("hideHistoryPanel", "AIAgentFunctions", hideHistoryPanel, false);
    a_vm->RegisterFunction("focusHistoryPanel", "AIAgentFunctions", focusHistoryPanel, false);
    a_vm->RegisterFunction("unfocusHistoryPanel", "AIAgentFunctions", unfocusHistoryPanel, false);
    a_vm->RegisterFunction("toggleHistoryPanelFocus", "AIAgentFunctions", toggleHistoryPanelFocus, false);
    
    a_vm->RegisterFunction("toggleOverlayPanel", "AIAgentFunctions", toggleOverlayPanel, false);
    a_vm->RegisterFunction("cycleOverlayStatusPanels", "AIAgentFunctions", cycleOverlayStatusPanels, false);
    a_vm->RegisterFunction("cycleHistoryDiariesPanels", "AIAgentFunctions", cycleHistoryDiariesPanels, false);
    a_vm->RegisterFunction("showOverlayPanel", "AIAgentFunctions", showOverlayPanel, false);
    a_vm->RegisterFunction("hideOverlayPanel", "AIAgentFunctions", hideOverlayPanel, false);
    
    a_vm->RegisterFunction("toggleDiariesPanel", "AIAgentFunctions", toggleDiariesPanel, false);
    a_vm->RegisterFunction("showDiariesPanel", "AIAgentFunctions", showDiariesPanel, false);
    a_vm->RegisterFunction("hideDiariesPanel", "AIAgentFunctions", hideDiariesPanel, false);
    
    a_vm->RegisterFunction("toggleBrowserPanel", "AIAgentFunctions", toggleBrowserPanel, false);
    a_vm->RegisterFunction("showBrowserPanel", "AIAgentFunctions", showBrowserPanel, false);
    a_vm->RegisterFunction("hideBrowserPanel", "AIAgentFunctions", hideBrowserPanel, false);
    
    a_vm->RegisterFunction("toggleAIViewPanel", "AIAgentFunctions", toggleAIViewPanel, false);
    a_vm->RegisterFunction("showAIViewPanel", "AIAgentFunctions", showAIViewPanel, false);
    a_vm->RegisterFunction("hideAIViewPanel", "AIAgentFunctions", hideAIViewPanel, false);
    
    a_vm->RegisterFunction("toggleDebuggerPanel", "AIAgentFunctions", toggleDebuggerPanel, false);
    a_vm->RegisterFunction("showDebuggerPanel", "AIAgentFunctions", showDebuggerPanel, false);
    a_vm->RegisterFunction("hideDebuggerPanel", "AIAgentFunctions", hideDebuggerPanel, false);
    
    a_vm->RegisterFunction("toggleStatusHUDPanel", "AIAgentFunctions", toggleStatusHUDPanel, false);
    a_vm->RegisterFunction("showStatusHUDPanel", "AIAgentFunctions", showStatusHUDPanel, false);
    a_vm->RegisterFunction("hideStatusHUDPanel", "AIAgentFunctions", hideStatusHUDPanel, false);
    
    a_vm->RegisterFunction("toggleChatboxPanel", "AIAgentFunctions", toggleChatboxPanel, false);
    a_vm->RegisterFunction("showChatboxPanel", "AIAgentFunctions", showChatboxPanel, false);
    a_vm->RegisterFunction("hideChatboxPanel", "AIAgentFunctions", hideChatboxPanel, false);
    a_vm->RegisterFunction("focusChatboxPanel", "AIAgentFunctions", focusChatboxPanel, false);
    a_vm->RegisterFunction("unfocusChatboxPanel", "AIAgentFunctions", unfocusChatboxPanel, false);
    a_vm->RegisterFunction("isChatboxPanelVisible", "AIAgentFunctions", isChatboxPanelVisible, false);
    a_vm->RegisterFunction("isChatboxPanelFocused", "AIAgentFunctions", isChatboxPanelFocused, false);
    a_vm->RegisterFunction("isAnyPrismaHotkeyPanelFocused", "AIAgentFunctions", isAnyPrismaHotkeyPanelFocused, false);
    
    a_vm->RegisterFunction("toggleSettingsMenu", "AIAgentFunctions", toggleSettingsMenu, false);
    a_vm->RegisterFunction("getSettingsMenuPendingAction", "AIAgentFunctions", getSettingsMenuPendingAction, false);
    a_vm->RegisterFunction("clearSettingsMenuPendingAction", "AIAgentFunctions", clearSettingsMenuPendingAction, false);
    
    a_vm->RegisterFunction("toggleMasterMenu", "AIAgentFunctions", toggleMasterMenu, false);
    a_vm->RegisterFunction("startPlayerMenuDialogueTTS", "AIAgentFunctions", startPlayerMenuDialogueTTS, false);

    a_vm->RegisterFunction("startMusicScene", "AIAgentFunctions", startMusicScene, false);
    a_vm->RegisterFunction("stopMusicScene", "AIAgentFunctions", stopMusicScene, false);

    a_vm->RegisterFunction("scanActorsAroundOffline", "AIAgentFunctions", scanActorsAroundOffline, false);
    a_vm->RegisterFunction("updateRemoteInventory", "AIAgentFunctions", updateRemoteInventory, false);

    a_vm->RegisterFunction("sendLocationFast", "AIAgentFunctions", sendLocationFast, false);
    a_vm->RegisterFunction("sendFactionFast", "AIAgentFunctions", sendFactionFast, false);
    a_vm->RegisterFunction("sendNPCFast", "AIAgentFunctions", sendNPCFast, false);
    a_vm->RegisterFunction("removeFromRenamedNPCList", "AIAgentFunctions", removeFromRenamedNPCList, false);
    return true;
}
