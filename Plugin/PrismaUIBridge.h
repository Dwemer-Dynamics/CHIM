#pragma once

#include "Globals.h"
#include "PrismaUI_API.h"
#include "json.hpp"
#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

struct PlayerConversationRoutingContext;

namespace PrismaUIBridge {

    enum class ChatboxTargetMode : std::uint8_t {
        Auto = 0,
        Everyone = 1,
        NPC = 2
    };

    // Initialize the Prisma UI API - call during PostLoad message
    bool Initialize();

    // Check if Prisma UI is available
    bool IsAvailable();

    // Create the conversation history panel
    void CreateHistoryPanel();

    // Toggle the history panel visibility
    void ToggleHistoryPanel();

    // Show the history panel
    void ShowHistoryPanel();

    // Hide the history panel
    void HideHistoryPanel();

    // Check if the history panel is visible
    bool IsHistoryPanelVisible();

    // Focus the history panel (allows clicking, scrolling, shows cursor)
    // pauseGame: if true, pauses the game while focused
    // Returns true if focus was successful
    bool FocusHistoryPanel(bool pauseGame = false);

    // Unfocus the history panel (returns control to game)
    void UnfocusHistoryPanel();

    // Check if the history panel has focus
    bool IsHistoryPanelFocused();

    // Toggle focus on the history panel
    void ToggleHistoryPanelFocus(bool pauseGame = false);

    // Fetch conversation history from HerikaServer and update the UI
    void FetchAndUpdateHistory();

    // Push a single new dialogue entry to the UI (for real-time updates)
    void PushDialogueEntry(const std::string& speaker, const std::string& text, 
                           const std::string& timestamp, const std::string& eventType,
                           const std::string& source = "llm",
                           const std::string& speakerType = "");

    // ===== CHIM Overlay Functions =====

    // Create the CHIM overlay panel
    void CreateOverlayPanel();

    // Toggle the overlay panel visibility
    void ToggleOverlayPanel();
    
    // Cycle through: Nothing -> Overlay -> Status -> Nothing
    void CycleOverlayStatusPanels();
    
    // Cycle through: Nothing -> History -> Diaries -> Nothing
    void CycleHistoryDiariesPanels();

    // Show the overlay panel
    void ShowOverlayPanel();

    // Hide the overlay panel
    void HideOverlayPanel();

    // Check if the overlay panel is visible
    bool IsOverlayPanelVisible();

    // Fetch CHIM status from HerikaServer and update the overlay
    void FetchAndUpdateOverlay();

    // Check crosshair target and update overlay in real-time
    void CheckAndUpdateCrosshairTarget();
    
    // Continuously check and update AI View with current target
    void CheckAndUpdateAIView();

    // ===== CHIM Diaries Functions =====

    // Create the CHIM diaries panel
    void CreateDiariesPanel();

    // Toggle the diaries panel visibility
    void ToggleDiariesPanel();

    // Show the diaries panel
    void ShowDiariesPanel();

    // Hide the diaries panel
    void HideDiariesPanel();

    // Check if the diaries panel is visible
    bool IsDiariesPanelVisible();

    // Fetch diaries data from HerikaServer and update the panel
    void FetchDiariesData(const std::string& mode, const std::string& param);

    // ===== Background Life Functions =====

    void CreateBackgroundLifePanel();
    void ToggleBackgroundLifePanel();
    void ShowBackgroundLifePanel();
    void HideBackgroundLifePanel();
    bool IsBackgroundLifePanelVisible();
    void FetchBackgroundLifeData(const std::string& queryString);

    // ===== CHIM NPC Manager Functions =====

    void CreateNpcManagerPanel();
    void ToggleNpcManagerPanel();
    void ShowNpcManagerPanel();
    void HideNpcManagerPanel();
    bool IsNpcManagerPanelVisible();

    // ===== CHIM Settings Hub Functions =====

    void CreateConfigManagerPanel();
    void ToggleConfigManagerPanel();
    void ShowConfigManagerPanel(const std::string& tab = "globals");
    void HideConfigManagerPanel();
    bool IsConfigManagerPanelVisible();

    // Stage one metadata-rich CHIM MCM setting before publishing the snapshot to Prisma.
    void BeginChimMcmSnapshot();
    void PublishChimMcmEntry(
        const std::string& page,
        const std::string& section,
        const std::string& key,
        const std::string& label,
        const std::string& description,
        const std::string& type,
        const std::string& value,
        float minValue,
        float maxValue,
        float step,
        const std::string& unit,
        bool readOnly,
        bool deprecated);
    void CommitChimMcmSnapshot(int revision);
    void BeginChimMcmAgents();
    void PublishChimMcmAgent(const std::string& bucket, int formId, const std::string& name);
    void CommitChimMcmAgents();
    void PublishChimMcmCommandResult(const std::string& request, bool ok, const std::string& message);

    // ===== CHIM Browser Functions =====

    // Create the CHIM browser panel
    void CreateBrowserPanel();

    // Toggle the browser panel visibility
    void ToggleBrowserPanel();

    // Show the browser panel
    void ShowBrowserPanel();

    // Hide the browser panel
    void HideBrowserPanel();

    // Check if the browser panel is visible
    bool IsBrowserPanelVisible();

    // ===== CHIM Quest Manager Functions =====

    // Create the CHIM quest manager panel
    void CreateQuestManagerPanel();

    // Toggle the quest manager panel visibility
    void ToggleQuestManagerPanel();

    // Show the quest manager panel
    void ShowQuestManagerPanel();

    // Hide the quest manager panel
    void HideQuestManagerPanel();

    // Check if the quest manager panel is visible
    bool IsQuestManagerPanelVisible();

    // ===== CHIM AI View Functions =====

    // Create the CHIM AI View panel
    void CreateAIViewPanel();

    // Toggle the AI View panel visibility
    void ToggleAIViewPanel();

    // Show the AI View panel
    void ShowAIViewPanel();

    // Hide the AI View panel
    void HideAIViewPanel();

    // Check if the AI View panel is visible
    bool IsAIViewPanelVisible();

    // Detect target NPC and fetch their profile data
    void DetectAndFetchTargetNPC();

    // Fetch and update AI View with specific NPC data
    void FetchAndUpdateAIView(const std::string& npcName, const std::string& refid);

    // ===== CHIM Debugger Functions =====

    // Create the CHIM debugger panel
    void CreateDebuggerPanel();

    // Toggle the debugger panel visibility
    void ToggleDebuggerPanel();

    // Show the debugger panel
    void ShowDebuggerPanel();

    // Hide the debugger panel
    void HideDebuggerPanel();

    // Check if the debugger panel is visible
    bool IsDebuggerPanelVisible();

    // Check and periodically update debugger data
    void CheckAndUpdateDebugger();

    // Fetch and update debugger with timing data
    void FetchAndUpdateDebugger();

    // ===== CHIM Status HUD Functions =====

    // Create the CHIM status HUD panel
    void CreateStatusHUDPanel();

    // Toggle the status HUD panel visibility
    void ToggleStatusHUDPanel();

    // Show the status HUD panel
    void ShowStatusHUDPanel();

    // Hide the status HUD panel
    void HideStatusHUDPanel();

    // Check if the status HUD panel is visible
    bool IsStatusHUDPanelVisible();

    // Check crosshair/nearest target and update Status HUD
    void CheckAndUpdateStatusHUDTarget();

    // ===== CHIM Chatbox Functions =====

    // Create the CHIM chatbox panel
    void CreateChatboxPanel();

    // Toggle the chatbox panel visibility
    void ToggleChatboxPanel();

    // Show the chatbox panel
    void ShowChatboxPanel();

    // Hide the chatbox panel
    void HideChatboxPanel();

    // Check if the chatbox panel is visible
    bool IsChatboxPanelVisible();

    // Focus the chatbox panel (enables input, blocks game controls)
    bool FocusChatboxPanel();

    // Unfocus the chatbox panel (returns control to game)
    void UnfocusChatboxPanel();

    // Check if the chatbox panel has focus
    bool IsChatboxPanelFocused();

    // Check whether a Prisma panel opened by a CHIM hotkey currently has focus.
    bool IsAnyHotkeyPanelFocused();

    // Check and update chatbox control strip state (target/mode/focus/nearby).
    // Set force=true to bypass the short refresh throttle after explicit UI actions.
    void CheckAndUpdateChatboxControls(bool force = false);

    // Read the current CHIM mode tracked by the Prisma bridge
    std::string GetCurrentChatboxMode();

    // Apply the mood saved in Prisma Chat to speech-to-text routing.
    void ApplySavedPlayerMood(PlayerConversationRoutingContext& routingContext);

    // Synchronize the native mode state after a Prisma, Papyrus, or server selection.
    // Pass persistToServer=true only when the caller has not already written the
    // matching chim_mode setconf entry itself. Server hydration is startup-only.
    bool SetCurrentChatboxMode(const std::string& mode, const char* sourceTag,
                               bool showNotification = false, bool persistToServer = false,
                               bool serverHydration = false);

    // Multiplier applied to player-spoken spatial reach for the active CHIM mode
    float GetPlayerSpeechDistanceMultiplier();

    // Multiplier applied to local player TTS playback volume for the active CHIM mode
    float GetPlayerSpeechPlaybackVolumeMultiplier();

    // True when CHIM mode should force narrator-only routing
    bool IsNarratorChatModeEnabled();

    // Retrieve the currently selected chatbox target override, if any
    bool GetChatboxTargetOverride(uint32_t& formId, std::string& name);

    // Read the current chatbox target mode override
    ChatboxTargetMode GetChatboxTargetMode();

    // True when the chatbox is explicitly forcing group broadcast chat
    bool IsChatboxEveryoneTargetOverrideActive();

    // Clear any active chatbox target override
    void ClearChatboxTargetOverride();

    // ===== CHIM Confirmation Modal Functions =====

    using ConfirmationCallback = std::function<void(bool accepted)>;

    bool ShowConfirmation(const std::string& title, const std::string& message,
                          const std::string& cancelLabel, const std::string& acceptLabel,
                          ConfirmationCallback callback);

    // Monotonic token used to invalidate late dialogue responses after a hard stop
    std::uint64_t GetDialogueStopGeneration();
    void BumpDialogueStopGeneration();

    // Shared by the chatbox and voice hotkey; does not require a Prisma view.
    void StopAllDialogueNow(const char* sourceTag);

    // Push a new chat message to the chatbox (real-time)
    void PushChatboxMessage(const std::string& speaker, const std::string& text, 
                            const std::string& timestamp, const std::string& type,
                            const std::string& source = "llm");

    // Push a system log entry to the chatbox system tab
    void PushSystemLogEntry(const std::string& level, const std::string& message, 
                            const std::string& timestamp);

    // Send a message typed in the chatbox
    void SendChatboxMessage(const std::string& message, const std::string& playerMood,
                            const std::string& customPlayerMood);

    // ===== CHIM Settings Menu Functions =====

    // Create the CHIM settings menu panel
    void CreateSettingsMenu();

    // Toggle the settings menu visibility
    void ToggleSettingsMenu();

    // Show the settings menu
    void ShowSettingsMenu();

    // Hide the settings menu
    void HideSettingsMenu();

    // Check if the settings menu is visible
    bool IsSettingsMenuVisible();

    // Get pending action from settings menu (for Papyrus-only actions)
    std::string GetPendingSettingsAction();

    // Clear pending action after Papyrus has processed it
    void ClearPendingSettingsAction();

    // ===== CHIM Master Menu Functions =====

    // Create the CHIM master menu panel
    void CreateMasterMenu();

    // Toggle the master menu visibility
    void ToggleMasterMenu();

    // Show the master menu
    void ShowMasterMenu();

    // Hide the master menu
    void HideMasterMenu();

    // Check if the master menu is visible
    bool IsMasterMenuVisible();

    // Set the MCM toggle state
    void SetEnabled(bool enabled);

    // Get the enabled state
    bool IsEnabled();

    // Clean up resources
    void Shutdown();

    // Get the last error message (for debugging)
    std::string GetLastError();

}
