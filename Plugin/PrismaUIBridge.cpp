#include "PrismaUIBridge.h"
#include "Conf.h"
#include "Misc.h"
#include "ThreadPool.h"
#include "Papyrus.h"
#include "HTTPManager.h"
#include "Globals.h"
#include "SpeakManager.h"
#include "SPGResponse.h"
#include "SpatialSnapshotManager.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <list>
#include <sstream>
#include <vector>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "ws2_32.lib")

namespace logger = SKSE::log;

namespace PrismaUIBridge {

    // Static state
    static PRISMA_UI_API::IVPrismaUI1* g_prismaUI = nullptr;
    static PRISMA_UI_API::IVPrismaUI2* g_prismaUI2 = nullptr;
    static PrismaView g_historyView = 0;
    static PrismaView g_overlayView = 0;
    static PrismaView g_diariesView = 0;
    static std::atomic<bool> g_enabled{false};
    static std::atomic<bool> g_panelCreated{false};
    static std::atomic<bool> g_overlayCreated{false};
    static std::atomic<bool> g_diariesCreated{false};
    static std::atomic<bool> g_domReady{false};
    static std::atomic<bool> g_overlayDomReady{false};
    static std::atomic<bool> g_diariesDomReady{false};
    static std::atomic<int> g_lastRowId{0};
    static std::string g_lastError;
    static std::mutex g_mutex;
    static std::mutex g_overlayFetchMutex;
    static std::chrono::steady_clock::time_point g_lastOverlayFetchAt{};
    static bool g_overlayFetchInFlight = false;
    constexpr auto kOverlayFetchMinInterval = std::chrono::seconds(8);
    constexpr auto kChatboxControlsMinInterval = std::chrono::milliseconds(250);
    
    // Crosshair target state (for overlay)
    static std::string g_lastCrosshairTarget = "";
    static std::string g_lastCrosshairTargetStatus = "";
    static uint32_t g_lastCrosshairFormId = 0;
    static std::chrono::steady_clock::time_point g_lastCrosshairCheck;
    static std::string g_lastOverlayAgentsPayload = "";
    static uint32_t g_stickyPrismaTargetFormId = 0;
    static std::chrono::steady_clock::time_point g_stickyPrismaTargetAt;
    constexpr auto kPrismaTargetStickyTtl = std::chrono::seconds(3);
    constexpr float kPrismaTargetSwitchMarginMeters = 1.5f;
    constexpr float kChatboxWhisperTargetMaxMeters = 1.0f;
    struct PrismaDisplayStatusEntry {
        std::string status;
        std::chrono::steady_clock::time_point updatedAt;
    };
    static std::unordered_map<uint32_t, PrismaDisplayStatusEntry> g_prismaDisplayStatusCache;
    constexpr auto kPrismaDisplayStatusHoldTtl = std::chrono::milliseconds(1200);
    constexpr size_t kPrismaDisplayStatusMaxEntries = 128;

    // CHIM chatbox control state
    static std::string g_chatboxCurrentMode = "STANDARD";
    static std::string g_lastChatboxTarget = "";
    static uint32_t g_lastChatboxTargetFormId = 0;
    static ChatboxTargetMode g_chatboxTargetMode = ChatboxTargetMode::Auto;
    static std::string g_chatboxTargetOverrideName = "";
    static uint32_t g_chatboxTargetOverrideFormId = 0;
    static std::string g_lastChatboxTargetsPayload = "";
    static std::chrono::steady_clock::time_point g_lastChatboxControlsCheck;
    static std::chrono::steady_clock::time_point g_lastChatboxStatusSync;
    static std::atomic<bool> g_chatboxStatusFetchInProgress{false};
    static std::atomic<bool> g_chatboxFocusChatEnabled{false};
    static std::atomic<bool> g_chatboxFocusChatInitialized{false};
    static bool g_chatboxFocusChatSentInitialized = false;
    static bool g_lastChatboxFocusChatSent = false;
    static bool g_chatboxModeInitialized = false;
    static std::string g_lastChatboxMode = "";
    static std::string g_chatboxCurrentModelLabel = "Standard";
    static bool g_chatboxModelInitialized = false;
    static std::string g_lastChatboxModelLabel = "";
    static std::string g_chatboxCurrentRechatMode = "random";
    static std::atomic<bool> g_chatboxRechatModeLoaded{false};
    static bool g_chatboxRechatModeSentInitialized = false;
    static std::string g_lastChatboxRechatMode = "";
    static std::atomic<std::uint64_t> g_dialogueStopGeneration{0};
    static std::atomic<bool> g_chatboxGameplayInputSuppressed{false};

    static void SetChatboxGameplayInputSuppressed(bool suppressed) {
        const bool previous = g_chatboxGameplayInputSuppressed.exchange(suppressed);
        if (previous != suppressed) {
            logger::info(
                "[PrismaUIBridge] Chatbox gameplay input suppression {}",
                suppressed ? "enabled" : "disabled");
        }
    }

    class ChatboxInputSink final : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static ChatboxInputSink& GetSingleton() {
            static ChatboxInputSink singleton;
            return singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(
            RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>*) override {
            if (!g_chatboxGameplayInputSuppressed.load() || !events) {
                return RE::BSEventNotifyControl::kContinue;
            }

            for (auto* event = *events; event; event = event->next) {
                const auto device = event->GetDevice();
                if (device == RE::INPUT_DEVICE::kKeyboard ||
                    device == RE::INPUT_DEVICE::kVirtualKeyboard) {
                    return RE::BSEventNotifyControl::kStop;
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }

        static bool Install() {
            auto* inputManager = RE::BSInputDeviceManager::GetSingleton();
            if (!inputManager) {
                logger::warn("[PrismaUIBridge] Cannot install chatbox input sink - input manager unavailable");
                return false;
            }

            auto* eventSource = static_cast<RE::BSTEventSource<RE::InputEvent*>*>(inputManager);
            auto* sink = std::addressof(GetSingleton());
            eventSource->AddEventSink(sink);

            // Input sinks run in registration order. Put CHIM first so kStop prevents
            // gameplay, SKSE, and Papyrus hotkeys before any downstream sink sees them.
            RE::BSSpinLockGuard locker(eventSource->lock);
            if (eventSource->notifying) {
                logger::warn("[PrismaUIBridge] Chatbox input sink registration deferred during input dispatch");
                return false;
            }

            const auto sinkIt = std::find(eventSource->sinks.begin(), eventSource->sinks.end(), sink);
            if (sinkIt == eventSource->sinks.end()) {
                logger::warn("[PrismaUIBridge] Chatbox input sink was not registered");
                return false;
            }

            std::rotate(eventSource->sinks.begin(), sinkIt, sinkIt + 1);
            logger::info("[PrismaUIBridge] Chatbox input sink installed at highest priority");
            return true;
        }
    };

    // Confirmation modal state
    static PrismaView g_confirmationView = 0;
    static std::atomic<bool> g_confirmationCreated{false};
    static std::atomic<bool> g_confirmationDomReady{false};
    static std::atomic<bool> g_confirmationVisible{false};
    static std::mutex g_confirmationMutex;
    static ConfirmationCallback g_confirmationCallback;
    static std::string g_confirmationPayload;
    static std::uint64_t g_confirmationRequestId = 0;
    
    // AI View target state
    static std::string g_lastAIViewTarget = "";
    static uint32_t g_lastAIViewFormId = 0;
    static std::chrono::steady_clock::time_point g_lastAIViewCheck;

    static std::string EscapePrismaJSArg(const std::string& raw)
    {
        std::string escaped = raw;
        size_t pos = 0;
        while ((pos = escaped.find('\\', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\\\");
            pos += 2;
        }
        pos = 0;
        while ((pos = escaped.find('\'', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\'");
            pos += 2;
        }
        pos = 0;
        while ((pos = escaped.find('\n', pos)) != std::string::npos) {
            escaped.replace(pos, 1, " ");
        }
        pos = 0;
        while ((pos = escaped.find('\r', pos)) != std::string::npos) {
            escaped.replace(pos, 1, " ");
        }
        return escaped;
    }

    // Settings Menu state
    static PrismaView g_settingsMenuView = 0;
    static std::atomic<bool> g_settingsMenuCreated{false};
    static std::atomic<bool> g_settingsMenuDomReady{false};
    static std::atomic<bool> g_settingsMenuVisible{false};  // Track visibility state
    static std::string g_pendingSettingsAction = "";
    static std::mutex g_settingsMenuMutex;
    
    // Master Menu state
    static PrismaView g_masterMenuView = 0;
    static std::atomic<bool> g_masterMenuCreated{false};
    static std::atomic<bool> g_masterMenuDomReady{false};
    static std::atomic<bool> g_masterMenuVisible{false};  // Track visibility state
    static std::mutex g_masterMenuMutex;

    // Quest Manager state
    static PrismaView g_questManagerView = 0;
    static std::atomic<bool> g_questManagerCreated{false};
    static std::atomic<bool> g_questManagerDomReady{false};
    static std::atomic<bool> g_questManagerVisible{false};

    // Browser state
    static PrismaView g_browserView = 0;
    static std::atomic<bool> g_browserCreated{false};
    static std::atomic<bool> g_browserDomReady{false};
    static std::atomic<bool> g_browserVisible{false};
    static std::string g_browserCurrentLabel = "browser";
    static std::string g_browserCurrentUrl;
    
    // Overlay/Status cycle state: 0 = both hidden, 1 = overlay visible, 2 = status visible
    static std::atomic<int> g_overlayStatusCycleState{0};
    
    // History/Diaries cycle state: 0 = both hidden, 1 = history visible, 2 = diaries visible
    static std::atomic<int> g_historyDiariesCycleState{0};

    // Forward declarations
    static void OnHistoryDomReady(PrismaView view);
    static void OnHistoryCommand(const char* argument);
    static void OnOverlayDomReady(PrismaView view);
    static void OnOverlayCommand(const char* argument);
    static void OnDiariesDomReady(PrismaView view);
    static void OnDiariesCommand(const char* argument);
    static void OnSettingsMenuDomReady(PrismaView view);
    static void OnSettingsMenuCommand(const char* argument);
    static void OnMasterMenuDomReady(PrismaView view);
    static void OnMasterMenuCommand(const char* argument);
    static void OnQuestManagerDomReady(PrismaView view);
    static void OnQuestManagerCommand(const char* argument);
    static void OnConfirmationDomReady(PrismaView view);
    static void OnConfirmationCommand(const char* argument);
    static void CreateConfirmationPanel();
    static bool PresentConfirmationPayload();
    static void ResolveConfirmation(bool accepted);
    static void OnBrowserCommand(const char* argument);
    void HideSettingsMenu();
    void HideMasterMenu();
    void HideQuestManagerPanel();
    void HideDebuggerPanel();
    static std::string FetchEventlogFromServer(int limit, int sinceRowId);
    static std::string FetchOverlayFromServer();
    static std::string FetchDiariesFromServer(const std::string& url);
    static void UpdateCrosshairTargetUI(const std::string& name, float distance, const std::string& status = "",
                                        bool targetable = true);
    static void UpdateOverlayAgentsUI(const std::vector<PlayerSpatialCandidate>& candidates, uint32_t activeFormId);
    static std::string EscapeForJS(const std::string& raw);
    static bool ApplyModeSelection(const std::string& actionId, const char* sourceTag, bool showNotification);
    static bool ApplyLLMProfileSelection(const std::string& actionId, const char* sourceTag, bool showNotification);
    static void UpdateChatboxTargetUI(const std::string& name, float distance);
    static void UpdateChatboxTargetsUI(const std::string& payload);
    static void UpdateChatboxModeUI(const std::string& mode);
    static void StopAllDialogueNow(const char* sourceTag);
    static void UpdateChatboxModelUI(const std::string& modelLabel);
    static void UpdateChatboxFocusUI(bool focused);
    static void UpdateChatboxRechatModeUI(const std::string& mode);
    static void SyncChatboxStatusFromServerAsync();
    static const char* PrismaConsoleLevelName(PRISMA_UI_API::ConsoleMessageLevel level);
    static void OnBrowserConsoleMessage(PrismaView view, PRISMA_UI_API::ConsoleMessageLevel level, const char* message);
    static void RegisterBrowserConsoleDiagnostics();
    static void InstallBrowserDebugHooks();
    static void LogBrowserDebugState(const char* reason);

    bool Initialize() {
        logger::info("[PrismaUIBridge] Initializing...");

        void* api = PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V1);
        if (!api) {
            g_lastError = "Failed to get Prisma UI API - PrismaUI.dll not loaded";
            logger::warn("[PrismaUIBridge] {}", g_lastError);
            return false;
        }

        g_prismaUI = static_cast<PRISMA_UI_API::IVPrismaUI1*>(api);
        logger::info("[PrismaUIBridge] Prisma UI API initialized successfully");

        ChatboxInputSink::Install();

        g_prismaUI2 = static_cast<PRISMA_UI_API::IVPrismaUI2*>(PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V2));
        if (g_prismaUI2) {
            logger::info("[PrismaUIBridge] Prisma UI V2 API available - browser console diagnostics enabled");
        } else {
            logger::warn("[PrismaUIBridge] Prisma UI V2 API unavailable - browser console diagnostics disabled");
        }
        
        // Create the history panel immediately so real-time updates work from the start
        // Panel will be hidden by default until user toggles it
        CreateHistoryPanel();
        CreateChatboxPanel();
        
        return true;
    }

    bool IsAvailable() {
        return g_prismaUI != nullptr;
    }

    static const char* PrismaConsoleLevelName(PRISMA_UI_API::ConsoleMessageLevel level) {
        using ConsoleMessageLevel = PRISMA_UI_API::ConsoleMessageLevel;

        switch (level) {
        case ConsoleMessageLevel::Log:
            return "log";
        case ConsoleMessageLevel::Warning:
            return "warn";
        case ConsoleMessageLevel::Error:
            return "error";
        case ConsoleMessageLevel::Debug:
            return "debug";
        case ConsoleMessageLevel::Info:
            return "info";
        default:
            return "unknown";
        }
    }

    static void OnBrowserConsoleMessage(PrismaView view, PRISMA_UI_API::ConsoleMessageLevel level, const char* message) {
        const char* text = message ? message : "<null>";
        const char* levelName = PrismaConsoleLevelName(level);

        switch (level) {
        case PRISMA_UI_API::ConsoleMessageLevel::Warning:
            logger::warn("[PrismaUIBridge][BrowserJS:{}][view={}][url={}] {}", levelName, view, g_browserCurrentUrl, text);
            break;
        case PRISMA_UI_API::ConsoleMessageLevel::Error:
            logger::error("[PrismaUIBridge][BrowserJS:{}][view={}][url={}] {}", levelName, view, g_browserCurrentUrl, text);
            break;
        case PRISMA_UI_API::ConsoleMessageLevel::Debug:
            logger::debug("[PrismaUIBridge][BrowserJS:{}][view={}][url={}] {}", levelName, view, g_browserCurrentUrl, text);
            break;
        case PRISMA_UI_API::ConsoleMessageLevel::Info:
        case PRISMA_UI_API::ConsoleMessageLevel::Log:
        default:
            logger::info("[PrismaUIBridge][BrowserJS:{}][view={}][url={}] {}", levelName, view, g_browserCurrentUrl, text);
            break;
        }
    }

    static void RegisterBrowserConsoleDiagnostics() {
        if (!g_prismaUI2 || !g_browserCreated.load() || !g_prismaUI || !g_prismaUI->IsValid(g_browserView)) {
            return;
        }

        g_prismaUI2->RegisterConsoleCallback(g_browserView, OnBrowserConsoleMessage);
        logger::info("[PrismaUIBridge] Registered browser console diagnostics for URL {}", g_browserCurrentUrl);
    }

    static void InstallBrowserDebugHooks() {
        if (!g_prismaUI || !g_browserCreated.load() || !g_prismaUI->IsValid(g_browserView)) {
            return;
        }

        constexpr const char* kBrowserDebugJs = R"CHIM(
(() => {
  const prefix = '[CHIM-BROWSER-DEBUG]';
  const editableSelector = 'input:not([type="hidden"]):not([disabled]), textarea:not([disabled]), select:not([disabled]), [contenteditable=""], [contenteditable="true"], [contenteditable="plaintext-only"]';
  const installedDocs = window.__chimBrowserDebugDocs || (window.__chimBrowserDebugDocs = new WeakSet());
  const wiredFrames = window.__chimBrowserDebugFrames || (window.__chimBrowserDebugFrames = new WeakSet());
  const describe = (node) => {
    if (!node) return 'null';
    const tag = node.tagName ? node.tagName.toLowerCase() : String(node);
    const id = node.id ? `#${node.id}` : '';
    const className = typeof node.className === 'string' && node.className.trim()
      ? `.${node.className.trim().split(/\s+/).filter(Boolean).join('.')}`
      : '';
    const name = node.getAttribute && node.getAttribute('name')
      ? `[name="${node.getAttribute('name')}"]`
      : '';
    return `${tag}${id}${className}${name}`;
  };
  const valueLength = (node) => {
    if (!node || !('value' in node) || typeof node.value !== 'string') return -1;
    return node.value.length;
  };
  const isVisible = (node) => {
    if (!node || node.hidden) return false;
    if (!node.getBoundingClientRect) return true;
    const rect = node.getBoundingClientRect();
    return rect.width > 0 && rect.height > 0;
  };
  const isTextEditable = (node) => {
    if (!node || !node.tagName || node.disabled || node.readOnly) {
      return false;
    }
    const tag = node.tagName.toUpperCase();
    if (tag === 'TEXTAREA') {
      return true;
    }
    if (tag === 'INPUT') {
      const type = String(node.type || '').toLowerCase();
      return !['button', 'submit', 'reset', 'checkbox', 'radio', 'range', 'color', 'file', 'hidden', 'image'].includes(type);
    }
    return !!node.isContentEditable;
  };
  const normalizeTextKey = (event) => {
    if (!event) {
      return null;
    }
    const legacyKey = event.which || event.keyCode || 0;
    if (event.code === 'Space' || event.key === ' ' || event.key === 'Space' || event.key === 'Spacebar' || legacyKey === 32) {
      return ' ';
    }
    return typeof event.key === 'string' && event.key.length === 1 ? event.key : null;
  };
  const shouldLogKeyEvent = (event) => {
    if (!event) {
      return false;
    }
    if (normalizeTextKey(event) !== null) {
      return true;
    }
    return event.key === 'Backspace' || event.key === 'Delete' || event.key === 'Enter' || event.key === 'Tab';
  };
  const dispatchBeforeInput = (doc, target, inputType, data) => {
    try {
      if (doc && doc.defaultView && typeof doc.defaultView.InputEvent === 'function') {
        const beforeEvt = new doc.defaultView.InputEvent('beforeinput', {
          bubbles: true,
          cancelable: true,
          data,
          inputType
        });
        return target.dispatchEvent(beforeEvt);
      }
    } catch (error) {
      console.warn(`${prefix} synthetic beforeinput error=${error && error.message ? error.message : error}`);
    }

    try {
      const fallbackEvt = doc.createEvent('Event');
      fallbackEvt.initEvent('beforeinput', true, true);
      fallbackEvt.data = data;
      fallbackEvt.inputType = inputType;
      return target.dispatchEvent(fallbackEvt);
    } catch (error) {
      console.warn(`${prefix} fallback beforeinput error=${error && error.message ? error.message : error}`);
    }
    return true;
  };
  const dispatchInput = (doc, target, inputType, data) => {
    try {
      if (doc && doc.defaultView && typeof doc.defaultView.InputEvent === 'function') {
        const inputEvt = new doc.defaultView.InputEvent('input', {
          bubbles: true,
          data,
          inputType
        });
        target.dispatchEvent(inputEvt);
        return;
      }
    } catch (error) {
      console.warn(`${prefix} synthetic input error=${error && error.message ? error.message : error}`);
    }

    try {
      const fallbackEvt = doc.createEvent('Event');
      fallbackEvt.initEvent('input', true, false);
      fallbackEvt.data = data;
      fallbackEvt.inputType = inputType;
      target.dispatchEvent(fallbackEvt);
    } catch (error) {
      console.warn(`${prefix} fallback input error=${error && error.message ? error.message : error}`);
    }
  };
  const applyTextControlEdit = (target, event) => {
    const value = typeof target.value === 'string' ? target.value : '';
    let start = typeof target.selectionStart === 'number' ? target.selectionStart : value.length;
    let end = typeof target.selectionEnd === 'number' ? target.selectionEnd : value.length;
    let nextValue = value;
    let nextCaret = start;
    let inputType = '';
    let data = null;
    let handled = false;
    const textKey = normalizeTextKey(event);

    if (event.key === 'Backspace') {
      if (start === end && start > 0) {
        start -= 1;
      }
      nextValue = value.slice(0, start) + value.slice(end);
      nextCaret = start;
      inputType = 'deleteContentBackward';
      handled = start !== end || nextValue !== value;
    } else if (event.key === 'Delete') {
      if (start === end && end < value.length) {
        end += 1;
      }
      nextValue = value.slice(0, start) + value.slice(end);
      nextCaret = start;
      inputType = 'deleteContentForward';
      handled = start !== end || nextValue !== value;
    } else if (textKey !== null) {
      data = textKey;
      const maxLength = typeof target.maxLength === 'number' ? target.maxLength : -1;
      if (maxLength >= 0) {
        const room = maxLength - (value.length - (end - start));
        if (room <= 0) {
          return false;
        }
        data = data.slice(0, room);
      }
      if (!data) {
        return false;
      }
      nextValue = value.slice(0, start) + data + value.slice(end);
      nextCaret = start + data.length;
      inputType = 'insertText';
      handled = true;
    } else if (event.key === 'Enter' && target.tagName && target.tagName.toUpperCase() === 'TEXTAREA') {
      data = '\n';
      nextValue = value.slice(0, start) + data + value.slice(end);
      nextCaret = start + data.length;
      inputType = 'insertLineBreak';
      handled = true;
    }

    if (!handled) {
      return false;
    }

    if (!dispatchBeforeInput(target.ownerDocument, target, inputType, data)) {
      return false;
    }

    target.value = nextValue;
    if (typeof target.setSelectionRange === 'function') {
      target.setSelectionRange(nextCaret, nextCaret);
    }
    dispatchInput(target.ownerDocument, target, inputType, data);
    return true;
  };
  const applyContentEditableEdit = (target, event) => {
    if (!target || !target.ownerDocument || !target.ownerDocument.defaultView) {
      return false;
    }
    const selection = target.ownerDocument.defaultView.getSelection();
    if (!selection || !selection.rangeCount) {
      return false;
    }
    const range = selection.getRangeAt(0);
    if (!target.contains(range.commonAncestorContainer)) {
      return false;
    }

    let text = null;
    let inputType = '';
    const textKey = normalizeTextKey(event);
    if (textKey !== null) {
      text = textKey;
      inputType = 'insertText';
    } else if (event.key === 'Enter') {
      text = '\n';
      inputType = 'insertLineBreak';
    } else {
      return false;
    }

    if (!dispatchBeforeInput(target.ownerDocument, target, inputType, text)) {
      return false;
    }

    range.deleteContents();
    const textNode = target.ownerDocument.createTextNode(text);
    range.insertNode(textNode);
    range.setStartAfter(textNode);
    range.collapse(true);
    selection.removeAllRanges();
    selection.addRange(range);
    dispatchInput(target.ownerDocument, target, inputType, text);
    return true;
  };
  const resolveEditableTarget = (target, depth = 0) => {
    if (!target || depth > 5) {
      return null;
    }
    if (isTextEditable(target)) {
      return target;
    }
    if (target.tagName === 'IFRAME') {
      try {
        if (target.contentDocument) {
          const frameTarget = resolveEditableTarget(target.contentDocument, depth + 1);
          if (frameTarget) {
            return frameTarget;
          }
        }
      } catch (error) {
        console.warn(`${prefix} resolve iframe error=${error && error.message ? error.message : error}`);
      }
    }
    const doc = target.nodeType === 9 ? target : (target.ownerDocument || document);
    if (!doc || !doc.activeElement || doc.activeElement === target) {
      return null;
    }
    return resolveEditableTarget(doc.activeElement, depth + 1);
  };
  const shouldShimKey = (event, target) => {
    if (!isTextEditable(target) || event.defaultPrevented || event.isComposing || event.altKey || event.ctrlKey || event.metaKey) {
      return false;
    }
    if (event.key === 'Backspace' || event.key === 'Delete') {
      return true;
    }
    if (event.key === 'Enter') {
      return !!target.isContentEditable || (target.tagName && target.tagName.toUpperCase() === 'TEXTAREA');
    }
    return normalizeTextKey(event) !== null;
  };
  const applyTextInputFallback = (target, event) => {
    const editableTarget = resolveEditableTarget(target);
    if (!editableTarget || !shouldShimKey(event, editableTarget)) {
      return false;
    }
    if (editableTarget.tagName && (editableTarget.tagName.toUpperCase() === 'INPUT' || editableTarget.tagName.toUpperCase() === 'TEXTAREA')) {
      return applyTextControlEdit(editableTarget, event);
    }
    if (editableTarget.isContentEditable) {
      return applyContentEditableEdit(editableTarget, event);
    }
    return false;
  };
  const installTopWindowShell = () => {
    if (window.top !== window || !document.documentElement || !document.body || document.body.dataset.chimBrowserWindowShellInstalled === 'true') {
      return;
    }

    const root = document.documentElement;
    const body = document.body;
    const initialNodes = Array.from(body.childNodes);
    const computedBodyStyle = window.getComputedStyle(body);
    const panelBackground = computedBodyStyle.backgroundColor && computedBodyStyle.backgroundColor !== 'rgba(0, 0, 0, 0)'
      ? computedBodyStyle.backgroundColor
      : 'rgba(20, 20, 24, 0.96)';
    const shell = document.createElement('div');
    const scaleRoot = document.createElement('div');
    const closeButton = document.createElement('button');
    const moveNodeIntoShell = (node) => {
      if (!node || node === shell || node.parentNode !== body) {
        return;
      }
      scaleRoot.appendChild(node);
    };
    body.dataset.chimBrowserWindowShellInstalled = 'true';

    root.style.setProperty('width', '100%', 'important');
    root.style.setProperty('height', '100%', 'important');
    root.style.setProperty('overflow', 'hidden', 'important');
    root.style.setProperty('background', 'transparent', 'important');

    body.style.setProperty('margin', '0', 'important');
    body.style.setProperty('width', '100vw', 'important');
    body.style.setProperty('height', '100vh', 'important');
    body.style.setProperty('overflow', 'hidden', 'important');
    body.style.setProperty('background', 'transparent', 'important');
    body.style.setProperty('pointer-events', 'none', 'important');

    shell.id = 'chim-browser-window-shell';
    shell.style.cssText = [
      'position:fixed',
      'left:10vw',
      'top:10vh',
      'width:80vw',
      'height:80vh',
      'overflow:hidden',
      'border-radius:14px',
      'border:1px solid rgba(242, 124, 17, 0.45)',
      'box-shadow:0 24px 64px rgba(0, 0, 0, 0.55)',
      'background:rgba(15, 15, 18, 0.18)',
      'pointer-events:auto',
      'z-index:2147483000'
    ].join(';');

    scaleRoot.id = 'chim-browser-window-scale';
    scaleRoot.style.cssText = [
      'position:absolute',
      'left:0',
      'top:0',
      'width:125%',
      'height:125%',
      'overflow:auto',
      'transform:scale(0.8)',
      'transform-origin:top left',
      `background:${panelBackground}`,
      'pointer-events:auto'
    ].join(';');

    closeButton.type = 'button';
    closeButton.textContent = 'Close';
    closeButton.style.cssText = [
      'position:absolute',
      'top:12px',
      'right:12px',
      'padding:10px 18px',
      'border:2px solid rgba(242, 124, 17, 1)',
      'border-radius:6px',
      'background:rgba(242, 124, 17, 0.95)',
      'color:#1c1c20',
      'font-size:16px',
      'font-weight:700',
      'cursor:pointer',
      'z-index:2147483647',
      'pointer-events:auto',
      'box-shadow:0 4px 12px rgba(0, 0, 0, 0.5)'
    ].join(';');
    closeButton.addEventListener('click', () => {
      if (window.chimBrowserCommand) {
        window.chimBrowserCommand('close');
      }
    });

    shell.appendChild(scaleRoot);
    shell.appendChild(closeButton);
    body.appendChild(shell);
    initialNodes.forEach(moveNodeIntoShell);

    const observer = new MutationObserver((entries) => {
      entries.forEach((entry) => {
        entry.addedNodes.forEach((node) => moveNodeIntoShell(node));
      });
    });
    observer.observe(body, { childList: true });

    console.info(`${prefix} top window_shell frame=80%x80% overlay=transparent close=enabled`);
  };
)CHIM"
R"CHIM(
  const dumpState = (doc, path, reason) => {
    if (!doc) return;
    const fields = doc.querySelectorAll(editableSelector);
    let href = 'unknown';
    try {
      href = doc.location && doc.location.href ? doc.location.href : 'unknown';
    } catch (error) {
      href = 'inaccessible';
    }
    console.info(`${prefix} ${path} state:${reason} href=${href} ready=${doc.readyState} active=${describe(doc.activeElement)} fields=${fields.length}`);
  };
  const isEditable = (node) => {
    if (!node || !node.tagName) return false;
    const tag = node.tagName.toUpperCase();
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return true;
    return !!node.isContentEditable;
  };
  const findFirstEditable = (doc) => {
    if (!doc) return null;
    const fields = doc.querySelectorAll(editableSelector);
    for (const field of fields) {
      if (isVisible(field)) {
        return field;
      }
    }
    return null;
  };
  const focusDeep = (doc, path, reason, depth = 0) => {
    if (!doc || depth > 4) {
      return false;
    }

    let active = null;
    try {
      active = doc.activeElement;
    } catch (error) {
      active = null;
    }

    if (isEditable(active)) {
      try {
        active.focus({ preventScroll: true });
        if (typeof active.select === 'function' && (active.tagName === 'INPUT' || active.tagName === 'TEXTAREA')) {
          active.select();
        }
        console.info(`${prefix} ${path} focusdeep reason=${reason} target=${describe(active)}`);
        return true;
      } catch (error) {
        console.warn(`${prefix} ${path} focusdeep editable_error=${error && error.message ? error.message : error}`);
      }
    }

    if (active && active.tagName === 'IFRAME') {
      try {
        if (active.contentWindow) {
          active.contentWindow.focus();
        }
        if (active.contentDocument && focusDeep(active.contentDocument, `${path}>active-iframe`, reason, depth + 1)) {
          return true;
        }
      } catch (error) {
        console.warn(`${prefix} ${path} focusdeep active_iframe_error=${error && error.message ? error.message : error}`);
      }
    }

    const frames = doc.querySelectorAll('iframe');
    for (const frame of frames) {
      if (!isVisible(frame)) {
        continue;
      }
      try {
        if (frame.contentWindow) {
          frame.contentWindow.focus();
        }
        if (frame.contentDocument && focusDeep(frame.contentDocument, `${path}>${describe(frame)}`, reason, depth + 1)) {
          return true;
        }
      } catch (error) {
        console.warn(`${prefix} ${path} focusdeep frame_error=${error && error.message ? error.message : error}`);
      }
    }

    const field = findFirstEditable(doc);
    if (field) {
      try {
        field.focus({ preventScroll: true });
        if (typeof field.select === 'function' && (field.tagName === 'INPUT' || field.tagName === 'TEXTAREA')) {
          field.select();
        }
        console.info(`${prefix} ${path} focusdeep reason=${reason} target=${describe(field)}`);
        return true;
      } catch (error) {
        console.warn(`${prefix} ${path} focusdeep field_error=${error && error.message ? error.message : error}`);
      }
    }

    console.info(`${prefix} ${path} focusdeep reason=${reason} result=none active=${describe(active)}`);
    return false;
  };
  const attach = (doc, path = 'top') => {
    if (!doc) {
      return;
    }
    if (doc === document) {
      installTopWindowShell();
    }

    if (!installedDocs.has(doc)) {
      installedDocs.add(doc);
      dumpState(doc, path, 'install');

      doc.addEventListener('focusin', (event) => {
        console.info(`${prefix} ${path} focusin target=${describe(event.target)} active=${describe(doc.activeElement)}`);
        wireFrames(doc, path);
      }, true);

      doc.addEventListener('focusout', (event) => {
        console.info(`${prefix} ${path} focusout target=${describe(event.target)} active=${describe(doc.activeElement)}`);
      }, true);

      doc.addEventListener('mousedown', (event) => {
        console.info(`${prefix} ${path} mousedown target=${describe(event.target)} active=${describe(doc.activeElement)}`);
        setTimeout(() => focusDeep(doc, path, 'post_mousedown'), 0);
      }, true);

      doc.addEventListener('keydown', (event) => {
        if (applyTextInputFallback(event.target, event)) {
          console.info(`${prefix} ${path} shim key=${JSON.stringify(event.key)} target=${describe(event.target)} active=${describe(doc.activeElement)}`);
          event.preventDefault();
        }
        if (shouldLogKeyEvent(event)) {
          console.info(`${prefix} ${path} keydown key=${event.key} code=${event.code} target=${describe(event.target)} active=${describe(doc.activeElement)} prevented=${event.defaultPrevented}`);
        }
      }, true);

      doc.addEventListener('keyup', (event) => {
        if (shouldLogKeyEvent(event)) {
          console.info(`${prefix} ${path} keyup key=${event.key} code=${event.code} target=${describe(event.target)} active=${describe(doc.activeElement)} prevented=${event.defaultPrevented}`);
        }
      }, true);

      doc.addEventListener('beforeinput', (event) => {
        console.info(`${prefix} ${path} beforeinput type=${event.inputType || 'unknown'} data=${JSON.stringify(event.data ?? '')} target=${describe(event.target)} active=${describe(doc.activeElement)}`);
      }, true);

      doc.addEventListener('input', (event) => {
        console.info(`${prefix} ${path} input target=${describe(event.target)} active=${describe(doc.activeElement)} valueLength=${valueLength(event.target)}`);
      }, true);

      if (doc.defaultView) {
        doc.defaultView.addEventListener('error', (event) => {
          console.error(`${prefix} ${path} window.error message=${event.message}`);
        });
      }

      if (doc.readyState === 'loading') {
        doc.addEventListener('DOMContentLoaded', () => {
          if (doc === document) {
            installTopWindowShell();
          }
          dumpState(doc, path, 'dom_ready');
          wireFrames(doc, path);
          setTimeout(() => focusDeep(doc, path, 'dom_ready'), 0);
        }, { once: true });
      } else {
        dumpState(doc, path, 'dom_ready');
      }

      setTimeout(() => {
        wireFrames(doc, path);
        dumpState(doc, path, 'post_install_250ms');
        focusDeep(doc, path, 'post_install_250ms');
      }, 250);

      setTimeout(() => {
        wireFrames(doc, path);
        dumpState(doc, path, 'post_install_1000ms');
        focusDeep(doc, path, 'post_install_1000ms');
      }, 1000);
    } else {
      dumpState(doc, path, 'reinstall');
    }

    wireFrames(doc, path);
  };
)CHIM"
R"CHIM(
  const wireFrames = (doc, path) => {
    if (!doc) {
      return;
    }

    const frames = doc.querySelectorAll('iframe');
    frames.forEach((frame, index) => {
      const framePath = `${path}>iframe[${index}]`;
      if (!wiredFrames.has(frame)) {
        wiredFrames.add(frame);
        frame.addEventListener('load', () => {
          console.info(`${prefix} ${framePath} load src=${frame.getAttribute('src') || frame.src || ''}`);
          try {
            attach(frame.contentDocument, framePath);
          } catch (error) {
            console.warn(`${prefix} ${framePath} attach_error=${error && error.message ? error.message : error}`);
          }
          setTimeout(() => focusDeep(doc, path, `frame_load:${framePath}`), 50);
        }, true);
      }

      try {
        if (frame.contentDocument) {
          attach(frame.contentDocument, framePath);
        }
      } catch (error) {
        console.warn(`${prefix} ${framePath} access_error=${error && error.message ? error.message : error}`);
      }
    });
  };

  window.__chimBrowserDebugInstalled = true;
  window.__chimBrowserDebugDumpState = (reason) => {
    dumpState(document, 'top', reason);
    wireFrames(document, 'top');
  };
  window.__chimBrowserFocusDeep = (reason) => focusDeep(document, 'top', reason || 'manual');

  attach(document, 'top');
})();
)CHIM";

        g_prismaUI->Invoke(g_browserView, kBrowserDebugJs, nullptr);
    }

    static void LogBrowserDebugState(const char* reason) {
        if (!g_prismaUI || !g_browserCreated.load() || !g_prismaUI->IsValid(g_browserView)) {
            return;
        }

        std::string jsCall = std::string("window.__chimBrowserDebugDumpState && window.__chimBrowserDebugDumpState('") + reason + "')";
        g_prismaUI->Invoke(g_browserView, jsCall.c_str(), nullptr);
    }

    void CreateHistoryPanel() {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create panel - Prisma UI not initialized. Make sure PrismaUI.dll is installed.");
            return;
        }

        if (g_panelCreated.load()) {
            logger::debug("[PrismaUIBridge] History panel already created");
            return;
        }

        logger::info("[PrismaUIBridge] Creating conversation history panel from CHIM/history.html...");

        // Create the view - path is relative to Data/PrismaUI/views/
        g_historyView = g_prismaUI->CreateView("CHIM/history.html", OnHistoryDomReady);

        if (g_historyView == 0) {
            g_lastError = "Failed to create history view - check that Data/PrismaUI/views/CHIM/history.html exists";
            logger::error("[PrismaUIBridge] {}", g_lastError);
            return;
        }

        logger::info("[PrismaUIBridge] View created with ID: {}", g_historyView);

        // Set view order (high number = on top of other UI)
        g_prismaUI->SetOrder(g_historyView, 100);

        // Set faster scroll speed (default is ~40px, increase to 120px for faster scrolling)
        g_prismaUI->SetScrollingPixelSize(g_historyView, 120);

        // Register listener for commands from JS
        g_prismaUI->RegisterJSListener(g_historyView, "chimHistoryCommand", OnHistoryCommand);

        // Hide initially - panel is created in background to receive real-time updates
        // User can toggle visibility with hotkey
        g_prismaUI->Hide(g_historyView);

        g_panelCreated.store(true);
        logger::info("[PrismaUIBridge] History panel created successfully, IsValid: {}, IsHidden: {}", 
            g_prismaUI->IsValid(g_historyView), g_prismaUI->IsHidden(g_historyView));
    }

    static void OnHistoryDomReady(PrismaView view) {
        logger::info("[PrismaUIBridge] History panel DOM ready - view can now be shown/hidden");
        g_domReady.store(true);

        // Initial fetch of conversation history
        FetchAndUpdateHistory();
    }

    static void OnHistoryCommand(const char* argument) {
        if (!argument) return;

        std::string cmd(argument);
        logger::debug("[PrismaUIBridge] Received command from UI: {}", cmd);

        if (cmd == "close") {
            // Unfocus first (if focused), then hide
            if (g_prismaUI && g_prismaUI->HasFocus(g_historyView)) {
                g_prismaUI->Unfocus(g_historyView);
            }
            HideHistoryPanel();
        } else if (cmd == "refresh") {
            FetchAndUpdateHistory();
        } else if (cmd == "unfocus") {
            // Allow JS to request unfocus (e.g., after completing an action)
            UnfocusHistoryPanel();
        }
    }

    void ToggleHistoryPanel() {
        logger::info("[PrismaUIBridge] Toggle requested. g_prismaUI={}, g_panelCreated={}, g_domReady={}", 
            (g_prismaUI ? "valid" : "null"), g_panelCreated.load(), g_domReady.load());
        
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot toggle - Prisma UI not initialized. Install PrismaUI mod from Nexus.");
            return;
        }

        // If panel not created yet, create it
        if (!g_panelCreated.load()) {
            logger::info("[PrismaUIBridge] Panel not created yet, creating now...");
            CreateHistoryPanel();
            if (!g_panelCreated.load()) {
                logger::error("[PrismaUIBridge] Failed to create panel - check logs above for details");
                return;
            }
        }

        // Check if DOM is ready
        if (!g_domReady.load()) {
            logger::warn("[PrismaUIBridge] DOM not ready yet - HTML may still be loading. Try again in a moment.");
            // Force a show attempt anyway
            g_prismaUI->Show(g_historyView);
            g_prismaUI->Focus(g_historyView, false, true);
            return;
        }

        // Check current state - if focused or visible, hide; otherwise show
        bool isHidden = g_prismaUI->IsHidden(g_historyView);
        bool hasFocus = g_prismaUI->HasFocus(g_historyView);
        logger::info("[PrismaUIBridge] Current state - IsHidden: {}, HasFocus: {}", isHidden, hasFocus);

        if (isHidden) {
            logger::info("[PrismaUIBridge] Showing and focusing panel");
            ShowHistoryPanel();  // This now auto-focuses
        } else {
            logger::info("[PrismaUIBridge] Hiding panel (will auto-unfocus)");
            HideHistoryPanel();  // This auto-unfocuses
        }
    }

    void ShowHistoryPanel() {
        if (!g_prismaUI || !g_panelCreated.load()) {
            // Try to create it if not yet created
            if (!g_panelCreated.load()) {
                CreateHistoryPanel();
            }
            if (!g_panelCreated.load()) {
                return;
            }
        }

        // Check if view is still valid
        if (!g_prismaUI->IsValid(g_historyView)) {
            logger::error("[PrismaUIBridge] View is not valid! HTML file may not exist at Data/PrismaUI/views/CHIM/history.html");
            return;
        }

        logger::info("[PrismaUIBridge] Showing history panel (view ID: {})", g_historyView);
        g_prismaUI->Show(g_historyView);
        
        // Double-check visibility
        bool isHidden = g_prismaUI->IsHidden(g_historyView);
        logger::info("[PrismaUIBridge] After Show() - IsHidden: {}", isHidden);

        // Focus with game PAUSED and cursor enabled (like settings menu)
        // pauseGame=true to pause game, disableFocusMenu=false to show cursor
        bool focused = g_prismaUI->Focus(g_historyView, true, false);
        logger::info("[PrismaUIBridge] Focused panel (paused game, cursor enabled): {}", focused);

        // DON'T fetch on show - we rely on real-time updates via PushDialogueEntry
        // Fetching from database would overwrite any recent dialogue that hasn't been logged yet
        // Only fetch on initial DOM ready (line 150) to populate the panel
    }

    void HideHistoryPanel() {
        if (!g_prismaUI || !g_panelCreated.load()) {
            return;
        }

        // Unfocus first if focused (this also resumes game if paused)
        if (g_prismaUI->HasFocus(g_historyView)) {
            g_prismaUI->Unfocus(g_historyView);
        }

        logger::info("[PrismaUIBridge] Hiding history panel");
        g_prismaUI->Hide(g_historyView);
    }

    bool IsHistoryPanelVisible() {
        if (!g_prismaUI || !g_panelCreated.load()) {
            return false;
        }
        return !g_prismaUI->IsHidden(g_historyView);
    }

    bool FocusHistoryPanel(bool pauseGame) {
        if (!g_prismaUI || !g_panelCreated.load()) {
            logger::warn("[PrismaUIBridge] Cannot focus - panel not ready");
            return false;
        }

        if (!g_domReady.load()) {
            logger::warn("[PrismaUIBridge] Cannot focus - DOM not ready");
            return false;
        }

        // Make sure panel is visible first
        if (g_prismaUI->IsHidden(g_historyView)) {
            g_prismaUI->Show(g_historyView);
        }

        // Focus with optional game pause, disable focus menu overlay for cleaner look
        bool success = g_prismaUI->Focus(g_historyView, pauseGame, true);
        
        if (success) {
            logger::info("[PrismaUIBridge] Panel focused (pauseGame: {})", pauseGame);
        } else {
            logger::warn("[PrismaUIBridge] Failed to focus panel");
        }

        return success;
    }

    void UnfocusHistoryPanel() {
        if (!g_prismaUI || !g_panelCreated.load()) {
            return;
        }

        g_prismaUI->Unfocus(g_historyView);
        logger::info("[PrismaUIBridge] Panel unfocused - control returned to game");
    }

    bool IsHistoryPanelFocused() {
        if (!g_prismaUI || !g_panelCreated.load()) {
            return false;
        }
        return g_prismaUI->HasFocus(g_historyView);
    }

    void ToggleHistoryPanelFocus(bool pauseGame) {
        if (!g_prismaUI || !g_panelCreated.load()) {
            logger::warn("[PrismaUIBridge] Cannot toggle focus - panel not ready");
            return;
        }

        if (g_prismaUI->HasFocus(g_historyView)) {
            UnfocusHistoryPanel();
        } else {
            FocusHistoryPanel(pauseGame);
        }
    }

    void FetchAndUpdateHistory() {
        if (!g_prismaUI || !g_panelCreated.load()) {
            return;
        }

        // Queue the fetch on the thread pool to avoid blocking
        ThreadPool::getInstance().enqueue(
            "PrismaUIFetch",
            []() {
                try {
                    // Fetch from server
                    std::string response = FetchEventlogFromServer(50, 0);

                    if (response.empty()) {
                        logger::warn("[PrismaUIBridge] Empty response from server");
                        return;
                    }

                    // Log response preview for debugging
                    std::string preview = response.substr(0, std::min(response.size(), (size_t)200));
                    logger::info("[PrismaUIBridge] Response preview (first 200 chars): {}", preview);

                    // Handle chunked transfer encoding - find where the JSON actually starts
                    size_t jsonStart = response.find('{');
                    if (jsonStart == std::string::npos) {
                        logger::error("[PrismaUIBridge] No JSON object found in response");
                        return;
                    }
                    
                    // Also find where JSON ends (in case of chunked encoding trailers)
                    size_t jsonEnd = response.rfind('}');
                    if (jsonEnd == std::string::npos || jsonEnd < jsonStart) {
                        logger::error("[PrismaUIBridge] Malformed JSON - no closing brace");
                        return;
                    }
                    
                    // Extract just the JSON portion
                    std::string jsonBody = response.substr(jsonStart, jsonEnd - jsonStart + 1);
                    logger::debug("[PrismaUIBridge] Extracted JSON body, length: {}", jsonBody.size());

                    // Parse JSON to validate
                    json parsed = json::parse(jsonBody, nullptr, false);
                    if (parsed.is_discarded()) {
                        logger::error("[PrismaUIBridge] Failed to parse JSON response. JSON length: {}", jsonBody.size());
                        return;
                    }
                    
                    // Use the cleaned JSON body for the response
                    response = jsonBody;

                    // Update the last row ID for incremental updates
                    if (parsed.contains("data") && parsed["data"].is_array() && !parsed["data"].empty()) {
                        auto& firstItem = parsed["data"][0];
                        if (firstItem.contains("ROWID")) {
                            std::string rowidStr = firstItem["ROWID"].get<std::string>();
                            g_lastRowId.store(std::stoi(rowidStr));
                        }
                    }

                    // Escape the JSON string for JavaScript
                    std::string escaped = response;
                    // Replace backslashes first, then other special chars
                    size_t pos = 0;
                    while ((pos = escaped.find('\\', pos)) != std::string::npos) {
                        escaped.replace(pos, 1, "\\\\");
                        pos += 2;
                    }
                    pos = 0;
                    while ((pos = escaped.find('\'', pos)) != std::string::npos) {
                        escaped.replace(pos, 1, "\\'");
                        pos += 2;
                    }
                    pos = 0;
                    while ((pos = escaped.find('\n', pos)) != std::string::npos) {
                        escaped.replace(pos, 1, "\\n");
                        pos += 2;
                    }
                    pos = 0;
                    while ((pos = escaped.find('\r', pos)) != std::string::npos) {
                        escaped.replace(pos, 1, "\\r");
                        pos += 2;
                    }

                    // Invoke the JavaScript update function
                    std::string jsCall = "window.updateHistory('" + escaped + "')";

                    if (g_prismaUI && g_panelCreated.load()) {
                        g_prismaUI->Invoke(g_historyView, jsCall.c_str(), nullptr);
                        logger::debug("[PrismaUIBridge] Sent history update to UI");
                    }

                } catch (const std::exception& e) {
                    logger::error("[PrismaUIBridge] Error fetching history: {}", e.what());
                }
            },
            "HistoryFetch",
            std::chrono::seconds(30)
        );
    }

    static std::list<std::string> g_historyEntryCache;
    static const size_t MAX_HISTORY_ENTRY_CACHE = 150;

    void PushDialogueEntry(const std::string& speaker, const std::string& text,
                           const std::string& timestamp, const std::string& eventType,
                           const std::string& source, const std::string& speakerType) {
        if (!g_prismaUI || !g_panelCreated.load()) {
            return;
        }

        // Push entries even when hidden so they appear when user opens the panel
        // This matches chatbox behavior - don't check IsHidden()
        std::string dedupeKey = speaker + "|" + text;
        auto it = std::find(g_historyEntryCache.begin(), g_historyEntryCache.end(), dedupeKey);
        if (it != g_historyEntryCache.end()) {
            return;
        }
        g_historyEntryCache.push_back(dedupeKey);
        while (g_historyEntryCache.size() > MAX_HISTORY_ENTRY_CACHE) {
            g_historyEntryCache.pop_front();
        }

        try {
            json entry;
            entry["speaker"] = speaker;
            entry["text"] = text;
            entry["timestamp"] = timestamp;
            entry["eventType"] = eventType;
            entry["source"] = source;
            entry["speakerType"] = speakerType;

            std::string jsonStr = entry.dump();

            // Escape for JS
            size_t pos = 0;
            while ((pos = jsonStr.find('\'', pos)) != std::string::npos) {
                jsonStr.replace(pos, 1, "\\'");
                pos += 2;
            }

            std::string jsCall = "window.pushEntry('" + jsonStr + "')";
            g_prismaUI->Invoke(g_historyView, jsCall.c_str(), nullptr);

        } catch (const std::exception& e) {
            logger::error("[PrismaUIBridge] Error pushing entry: {}", e.what());
        }
    }

    void SetEnabled(bool enabled) {
        g_enabled.store(enabled);
        logger::info("[PrismaUIBridge] Enabled state set to: {}", enabled);

        if (enabled && !g_panelCreated.load()) {
            CreateHistoryPanel();
        }
    }

    bool IsEnabled() {
        return g_enabled.load();
    }

    // ===== Crosshair Target Functions =====

    static bool IsSelectablePrismaCandidate(const PlayerSpatialCandidate& candidate)
    {
        return candidate.autoEligible || candidate.targetable || candidate.lookTarget;
    }

    static bool IsStablePrismaSpatialReason(const std::string& reason)
    {
        return reason == "immediate_proximity" ||
               reason == "line_of_sight_clear" ||
               reason == "line_of_sight_blocked" ||
               reason == "closed_door_between" ||
               reason == "open_door_muffled" ||
               reason == "path_fallback_clear" ||
               reason == "path_ratio_blocked" ||
               reason == "path_ratio_los_blocked" ||
               reason == "path_ratio_distance_blocked" ||
               reason == "navmesh_no_path" ||
               reason == "path_unavailable" ||
               reason == "different_area" ||
               reason == "different_interior_cells" ||
               reason == "interior_exterior_boundary" ||
               reason == "too_far" ||
               reason == "too_quiet";
    }

    static bool IsTransientPrismaSpatialReason(const std::string& reason)
    {
        return reason.empty() ||
               reason == "distance_cell_clear" ||
               reason == "pending_spatial" ||
               reason == "vertical_separation" ||
               reason == "unknown";
    }

    static bool IsHiddenPrismaStatus(const std::string& status)
    {
        return status.empty() ||
               status == "Busy" ||
               status == "Hostile" ||
               status == "Restrained" ||
               status == "Unavailable" ||
               status == "In range" ||
               status == "Can't hear you: different area";
    }

    static bool IsGenericPrismaHearingStatus(const std::string& status)
    {
        return status == "Can hear you" || status == "Can't hear you";
    }

    static bool IsVerticalPrismaStatus(const std::string& status)
    {
        return status.find("above you") != std::string::npos ||
               status.find("below you") != std::string::npos;
    }

    static void PrunePrismaDisplayStatusCache(std::chrono::steady_clock::time_point now)
    {
        if (g_prismaDisplayStatusCache.size() <= kPrismaDisplayStatusMaxEntries) {
            return;
        }

        for (auto it = g_prismaDisplayStatusCache.begin(); it != g_prismaDisplayStatusCache.end();) {
            if (now - it->second.updatedAt > kPrismaDisplayStatusHoldTtl) {
                it = g_prismaDisplayStatusCache.erase(it);
            } else {
                ++it;
            }
        }
        while (g_prismaDisplayStatusCache.size() > kPrismaDisplayStatusMaxEntries) {
            g_prismaDisplayStatusCache.erase(g_prismaDisplayStatusCache.begin());
        }
    }

    static void CachePrismaDisplayStatus(uint32_t formId, const std::string& status,
                                         std::chrono::steady_clock::time_point now)
    {
        if (formId == 0 || IsHiddenPrismaStatus(status)) {
            return;
        }

        PrunePrismaDisplayStatusCache(now);
        g_prismaDisplayStatusCache[formId] = { status, now };
    }

    static std::string GetCachedPrismaDisplayStatus(uint32_t formId, std::chrono::steady_clock::time_point now)
    {
        auto it = g_prismaDisplayStatusCache.find(formId);
        if (it == g_prismaDisplayStatusCache.end()) {
            return "";
        }

        if (now - it->second.updatedAt > kPrismaDisplayStatusHoldTtl) {
            g_prismaDisplayStatusCache.erase(it);
            return "";
        }

        return IsHiddenPrismaStatus(it->second.status) ? "" : it->second.status;
    }

    static std::string PreserveVerticalPrismaStatus(uint32_t formId, const std::string& status,
                                                    std::chrono::steady_clock::time_point now)
    {
        if (!IsGenericPrismaHearingStatus(status)) {
            return status;
        }

        const auto cachedStatus = GetCachedPrismaDisplayStatus(formId, now);
        return IsVerticalPrismaStatus(cachedStatus) ? cachedStatus : status;
    }

    static std::string GetPrismaDisplayStatus(const PlayerSpatialCandidate& candidate)
    {
        if (candidate.formId == 0) {
            return IsHiddenPrismaStatus(candidate.status) ? "" : candidate.status;
        }

        const auto now = std::chrono::steady_clock::now();

        if (candidate.status == "In combat") {
            CachePrismaDisplayStatus(candidate.formId, candidate.status, now);
            return candidate.status;
        }

        if (IsStablePrismaSpatialReason(candidate.reason) && !IsHiddenPrismaStatus(candidate.status)) {
            const auto displayStatus = PreserveVerticalPrismaStatus(candidate.formId, candidate.status, now);
            CachePrismaDisplayStatus(candidate.formId, displayStatus, now);
            return displayStatus;
        } else if (IsStablePrismaSpatialReason(candidate.reason)) {
            g_prismaDisplayStatusCache.erase(candidate.formId);
            return "";
        }

        if (IsTransientPrismaSpatialReason(candidate.reason)) {
            if (candidate.reason == "distance_cell_clear" &&
                (candidate.targetable || candidate.autoEligible || candidate.lookTarget)) {
                const auto displayStatus = PreserveVerticalPrismaStatus(candidate.formId, "Can hear you", now);
                CachePrismaDisplayStatus(candidate.formId, displayStatus, now);
                return displayStatus;
            }

            if (candidate.reason == "vertical_separation" && !IsHiddenPrismaStatus(candidate.status)) {
                CachePrismaDisplayStatus(candidate.formId, candidate.status, now);
                return candidate.status;
            }

            const auto cachedStatus = GetCachedPrismaDisplayStatus(candidate.formId, now);
            if (!cachedStatus.empty()) {
                return cachedStatus;
            }
            return "";
        }

        if (IsHiddenPrismaStatus(candidate.status)) {
            g_prismaDisplayStatusCache.erase(candidate.formId);
            return "";
        }

        CachePrismaDisplayStatus(candidate.formId, candidate.status, now);
        return candidate.status;
    }

    static PlayerSpatialTargetStatus ResolvePrimaryPrismaTarget(const std::vector<PlayerSpatialCandidate>& candidates)
    {
        PlayerSpatialTargetStatus resolved{};
        if (candidates.empty()) {
            resolved.status = "No target";
            return resolved;
        }

        // Prisma is feedback first: if the player is looking at an NPC, show that NPC
        // immediately even when spatial says blocked. Listener routing still only uses
        // autoEligible/targetable candidates, so this does not make blocked NPCs receive STT.
        auto selected = std::find_if(candidates.begin(), candidates.end(),
            [](const PlayerSpatialCandidate& candidate) {
                return candidate.lookTarget;
            });
        if (selected == candidates.end()) {
            selected = std::find_if(candidates.begin(), candidates.end(),
            [](const PlayerSpatialCandidate& candidate) {
                return candidate.autoEligible;
            });
        }
        if (selected == candidates.end()) {
            selected = std::find_if(candidates.begin(), candidates.end(),
                [](const PlayerSpatialCandidate& candidate) {
                    return candidate.targetable;
                });
        }
        if (selected == candidates.end()) {
            selected = candidates.begin();
        }

        const auto now = std::chrono::steady_clock::now();
        if (!selected->lookTarget && g_stickyPrismaTargetFormId != 0 &&
            now - g_stickyPrismaTargetAt <= kPrismaTargetStickyTtl) {
            auto sticky = std::find_if(candidates.begin(), candidates.end(),
                [](const PlayerSpatialCandidate& candidate) {
                    return candidate.formId == g_stickyPrismaTargetFormId &&
                        IsSelectablePrismaCandidate(candidate);
                });
            if (sticky != candidates.end() &&
                sticky->distanceMeters <= selected->distanceMeters + kPrismaTargetSwitchMarginMeters) {
                selected = sticky;
            }
        }

        if (IsSelectablePrismaCandidate(*selected)) {
            g_stickyPrismaTargetFormId = selected->formId;
            g_stickyPrismaTargetAt = now;
        } else if (now - g_stickyPrismaTargetAt > kPrismaTargetStickyTtl) {
            g_stickyPrismaTargetFormId = 0;
        }

        resolved.hasTarget = true;
        resolved.name = selected->name;
        resolved.formId = selected->formId;
        resolved.distanceMeters = selected->distanceMeters;
        resolved.source = selected->lookTarget ? selected->source : "nearest_" + selected->source;
        resolved.reason = selected->reason;
        resolved.targetable = selected->targetable;
        const std::string prefix = selected->lookTarget ? "Crosshair" : "Nearest";
        const auto displayStatus = GetPrismaDisplayStatus(*selected);
        resolved.status = displayStatus.empty() ? prefix : prefix + ": " + displayStatus;
        return resolved;
    }

    static PlayerSpatialTargetStatus GetPrimaryPrismaTarget(const std::string& reason)
    {
        const auto candidates = SpatialSnapshotManager::GetPlayerConversationTargets(reason, true);
        return ResolvePrimaryPrismaTarget(candidates);
    }

    void CheckAndUpdateCrosshairTarget() {
        // Poll UI target state quickly; SpatialSnapshotManager owns the heavier cache/TTL.
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastCrosshairCheck).count() < 250) {
            return;
        }
        g_lastCrosshairCheck = now;
        
        // Only update if overlay is visible
        if (!g_overlayCreated.load() || !g_prismaUI || g_prismaUI->IsHidden(g_overlayView)) {
            return;
        }
        
        const auto candidates = SpatialSnapshotManager::GetPlayerConversationTargets("prismaui_overlay", true);
        const auto target = ResolvePrimaryPrismaTarget(candidates);
        UpdateOverlayAgentsUI(candidates, target.formId);

        if (target.hasTarget) {
            if (target.name != g_lastCrosshairTarget || target.formId != g_lastCrosshairFormId ||
                target.status != g_lastCrosshairTargetStatus) {
                UpdateCrosshairTargetUI(target.name, target.distanceMeters, target.status, target.targetable);
                g_lastCrosshairTarget = target.name;
                g_lastCrosshairFormId = target.formId;
                g_lastCrosshairTargetStatus = target.status;
            }
        } else {
            if (!g_lastCrosshairTarget.empty() || target.status != g_lastCrosshairTargetStatus) {
                UpdateCrosshairTargetUI("", 0.0f, target.status, false);
                g_lastCrosshairTarget.clear();
                g_lastCrosshairFormId = 0;
                g_lastCrosshairTargetStatus = target.status;
            }
        }
    }

    static void UpdateCrosshairTargetUI(const std::string& name, float distance, const std::string& status,
                                        bool targetable) {
        if (!g_prismaUI || !g_overlayCreated.load()) {
            return;
        }
        
        std::string jsCall;
        if (name.empty()) {
            jsCall = "window.updateCrosshairTarget('', 0, '" + EscapePrismaJSArg(status) + "', false)";
        } else {
            jsCall = "window.updateCrosshairTarget('" + EscapePrismaJSArg(name) + "', " +
                std::to_string(distance) + ", '" + EscapePrismaJSArg(status) + "', " +
                (targetable ? "true" : "false") + ")";
        }
        
        g_prismaUI->Invoke(g_overlayView, jsCall.c_str(), nullptr);
    }

    static void UpdateOverlayAgentsUI(const std::vector<PlayerSpatialCandidate>& candidates, uint32_t activeFormId) {
        if (!g_prismaUI || !g_overlayCreated.load()) {
            return;
        }

        json agents = json::array();
        for (const auto& candidate : candidates) {
            json item;
            item["name"] = candidate.name.empty() ? "Unknown Target" : candidate.name;
            item["form_id"] = candidate.formId;
            item["distance"] = candidate.distanceMeters;
            item["status"] = GetPrismaDisplayStatus(candidate);
            item["source"] = candidate.source;
            item["reason"] = candidate.reason;
            item["targetable"] = candidate.targetable || candidate.autoEligible;
            item["active"] = candidate.formId != 0 && candidate.formId == activeFormId;
            item["look_target"] = candidate.lookTarget;
            agents.push_back(item);
        }

        const std::string payload = agents.dump();
        if (payload == g_lastOverlayAgentsPayload) {
            return;
        }

        g_lastOverlayAgentsPayload = payload;
        const std::string jsCall = "window.updateSpatialAgents('" + EscapePrismaJSArg(payload) + "')";
        g_prismaUI->Invoke(g_overlayView, jsCall.c_str(), nullptr);
    }

    static std::string EscapeForJS(const std::string& raw) {
        std::string escaped = raw;
        size_t pos = 0;
        while ((pos = escaped.find('\\', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\\\");
            pos += 2;
        }
        pos = 0;
        while ((pos = escaped.find('\'', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\'");
            pos += 2;
        }
        pos = 0;
        while ((pos = escaped.find('\n', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\n");
            pos += 2;
        }
        pos = 0;
        while ((pos = escaped.find('\r', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\r");
            pos += 2;
        }
        return escaped;
    }

    static bool ApplyModeSelection(const std::string& actionId, const char* sourceTag, bool showNotification) {
        std::string modeStr;
        if (actionId == "mode_standard") modeStr = "STANDARD";
        else if (actionId == "mode_shout") modeStr = "SHOUT";
        else if (actionId == "mode_whisper") modeStr = "WHISPER";
        else if (actionId == "mode_narrator") modeStr = "NARRATOR";
        else if (actionId == "mode_director") modeStr = "DIRECTOR";
        else if (actionId == "mode_spawn") modeStr = "SPAWN";
        else if (actionId == "mode_cheat") modeStr = "CHEATMODE";
        else if (actionId == "mode_autochat") modeStr = "AUTOCHAT";
        else if (actionId == "mode_inject_log") modeStr = "INJECTION_LOG";
        else if (actionId == "mode_inject_chat") modeStr = "INJECTION_CHAT";

        if (modeStr.empty()) {
            return false;
        }

        HTTPManager::log(std::format("setconf|{}|{}|chim_mode@{}",
            getCurrentTimeMillis(), GetGameTimeStamp(), modeStr));

        const std::string previousMode = g_chatboxCurrentMode;
        g_chatboxCurrentMode = modeStr;
        logger::info("[{}] Set mode to: {}", sourceTag, modeStr);

        if (showNotification) {
            RE::DebugNotification(("[CHIM] Chat mode: " + modeStr).c_str());
        }

        if (previousMode != modeStr) {
            // Voice mode changes should affect player speech reach without rewriting
            // the user's MCM auto-activation distances. Clear dynamic spatial state so
            // listener routing and Prisma UI immediately use the new runtime multiplier
            // without treating the mode change as a fresh cell-entry settle window.
            SpatialSnapshotManager::InvalidateDynamicSpatialState();
            g_lastOverlayAgentsPayload.clear();
            g_lastChatboxTargetsPayload.clear();
            g_prismaDisplayStatusCache.clear();
            logger::info("[{}] Player speech spatial multiplier now {:.2f}", sourceTag,
                         GetPlayerSpeechDistanceMultiplier());
        }

        return true;
    }

    // ===== CHIM Overlay Functions =====

    void CreateOverlayPanel() {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create overlay - Prisma UI not initialized");
            return;
        }

        if (g_overlayCreated.load()) {
            logger::debug("[PrismaUIBridge] Overlay panel already created");
            return;
        }

        logger::info("[PrismaUIBridge] Creating CHIM overlay panel from CHIM/overlay.html...");

        // Create the view - path is relative to Data/PrismaUI/views/
        g_overlayView = g_prismaUI->CreateView("CHIM/overlay.html", OnOverlayDomReady);

        if (g_overlayView == 0) {
            g_lastError = "Failed to create overlay view - check that Data/PrismaUI/views/CHIM/overlay.html exists";
            logger::error("[PrismaUIBridge] {}", g_lastError);
            return;
        }

        logger::info("[PrismaUIBridge] Overlay view created with ID: {}", g_overlayView);

        // Set view order (slightly lower than history panel)
        g_prismaUI->SetOrder(g_overlayView, 90);

        // Register listener for commands from JS
        g_prismaUI->RegisterJSListener(g_overlayView, "chimOverlayCommand", OnOverlayCommand);

        g_overlayCreated.store(true);
        logger::info("[PrismaUIBridge] Overlay panel created successfully");
    }

    static void OnOverlayDomReady(PrismaView view) {
        logger::info("[PrismaUIBridge] Overlay panel DOM ready");
        g_overlayDomReady.store(true);

        // Initial fetch of overlay data
        FetchAndUpdateOverlay();
        if (g_prismaUI && g_overlayCreated.load() && !g_prismaUI->IsHidden(g_overlayView)) {
            g_prismaUI->Invoke(g_overlayView, "window.onOverlayShown && window.onOverlayShown()", nullptr);
        }
    }

    static void OnOverlayCommand(const char* argument) {
        if (!argument) return;

        std::string cmd(argument);
        logger::debug("[PrismaUIBridge] Received overlay command: {}", cmd);

        if (cmd == "close") {
            HideOverlayPanel();
        } else if (cmd == "refresh") {
            if (!g_prismaUI || !g_overlayCreated.load() || g_prismaUI->IsHidden(g_overlayView)) {
                return;
            }
            FetchAndUpdateOverlay();
        }
    }

    void ToggleOverlayPanel() {
        logger::info("[PrismaUIBridge] Overlay toggle requested");
        
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot toggle overlay - Prisma UI not initialized");
            return;
        }

        // If panel not created yet, create it
        if (!g_overlayCreated.load()) {
            logger::info("[PrismaUIBridge] Overlay not created yet, creating now...");
            CreateOverlayPanel();
            if (!g_overlayCreated.load()) {
                logger::error("[PrismaUIBridge] Failed to create overlay");
                return;
            }
        }

        // Check if DOM is ready
        if (!g_overlayDomReady.load()) {
            logger::warn("[PrismaUIBridge] Overlay DOM not ready yet");
            g_prismaUI->Show(g_overlayView);
            return;
        }

        if (g_prismaUI->IsHidden(g_overlayView)) {
            logger::info("[PrismaUIBridge] Showing overlay");
            ShowOverlayPanel();
        } else {
            logger::info("[PrismaUIBridge] Hiding overlay");
            HideOverlayPanel();
        }
    }

    void ShowOverlayPanel() {
        if (!g_prismaUI || !g_overlayCreated.load()) {
            if (!g_overlayCreated.load()) {
                CreateOverlayPanel();
            }
            if (!g_overlayCreated.load()) {
                return;
            }
        }

        if (!g_prismaUI->IsValid(g_overlayView)) {
            logger::error("[PrismaUIBridge] Overlay view is not valid!");
            return;
        }

        logger::info("[PrismaUIBridge] Showing overlay panel");
        g_prismaUI->Show(g_overlayView);
        if (g_overlayDomReady.load()) {
            g_prismaUI->Invoke(g_overlayView, "window.onOverlayShown && window.onOverlayShown()", nullptr);
        }

        // Fetch latest data when shown
        FetchAndUpdateOverlay();
    }

    void HideOverlayPanel() {
        if (!g_prismaUI || !g_overlayCreated.load()) {
            return;
        }

        logger::info("[PrismaUIBridge] Hiding overlay panel");
        if (g_overlayDomReady.load()) {
            g_prismaUI->Invoke(g_overlayView, "window.onOverlayHidden && window.onOverlayHidden()", nullptr);
        }
        g_prismaUI->Hide(g_overlayView);
    }
    
    void CycleOverlayStatusPanels() {
        logger::info("[PrismaUIBridge] Cycling overlay/status/aiview panels (current state: {})", g_overlayStatusCycleState.load());
        
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot cycle panels - Prisma UI not initialized");
            return;
        }
        
        int currentState = g_overlayStatusCycleState.load();
        
        switch (currentState) {
            case 0: // Nothing visible -> Show Overlay
                logger::info("[PrismaUIBridge] Cycle: Showing Overlay");
                ShowOverlayPanel();
                g_overlayStatusCycleState.store(1);
                break;
                
            case 1: // Overlay visible -> Hide Overlay, Show Status
                logger::info("[PrismaUIBridge] Cycle: Hiding Overlay, Showing Status");
                HideOverlayPanel();
                ShowStatusHUDPanel();
                g_overlayStatusCycleState.store(2);
                break;
                
            case 2: // Status visible -> Hide Status, Show AI View
                logger::info("[PrismaUIBridge] Cycle: Hiding Status, Showing AI View");
                HideStatusHUDPanel();
                ShowAIViewPanel();
                g_overlayStatusCycleState.store(3);
                break;
                
            case 3: // AI View visible -> Hide AI View, back to Nothing
                logger::info("[PrismaUIBridge] Cycle: Hiding AI View");
                HideAIViewPanel();
                g_overlayStatusCycleState.store(0);
                break;
                
            default:
                // Invalid state, reset
                logger::warn("[PrismaUIBridge] Invalid cycle state {}, resetting", currentState);
                HideOverlayPanel();
                HideStatusHUDPanel();
                HideAIViewPanel();
                g_overlayStatusCycleState.store(0);
                break;
        }
    }
    
    void CycleHistoryDiariesPanels() {
        logger::info("[PrismaUIBridge] Cycling history/diaries panels (current state: {})", g_historyDiariesCycleState.load());
        
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot cycle panels - Prisma UI not initialized");
            return;
        }
        
        int currentState = g_historyDiariesCycleState.load();
        
        switch (currentState) {
            case 0: // Nothing visible -> Show History
                logger::info("[PrismaUIBridge] Cycle: Showing History");
                ShowHistoryPanel();
                g_historyDiariesCycleState.store(1);
                break;
                
            case 1: // History visible -> Hide History, Show Diaries
                logger::info("[PrismaUIBridge] Cycle: Hiding History, Showing Diaries");
                HideHistoryPanel();
                ShowDiariesPanel();
                g_historyDiariesCycleState.store(2);
                break;
                
            case 2: // Diaries visible -> Hide Diaries, back to Nothing
                logger::info("[PrismaUIBridge] Cycle: Hiding Diaries");
                HideDiariesPanel();
                g_historyDiariesCycleState.store(0);
                break;
                
            default:
                // Invalid state, reset
                logger::warn("[PrismaUIBridge] Invalid cycle state {}, resetting", currentState);
                HideHistoryPanel();
                HideDiariesPanel();
                g_historyDiariesCycleState.store(0);
                break;
        }
    }

    bool IsOverlayPanelVisible() {
        if (!g_prismaUI || !g_overlayCreated.load()) {
            return false;
        }
        return !g_prismaUI->IsHidden(g_overlayView);
    }

    void FetchAndUpdateOverlay() {
        if (!g_prismaUI || !g_overlayCreated.load()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_overlayFetchMutex);
            const auto now = std::chrono::steady_clock::now();
            if (g_overlayFetchInFlight ||
                (g_lastOverlayFetchAt.time_since_epoch().count() != 0 &&
                 now - g_lastOverlayFetchAt < kOverlayFetchMinInterval)) {
                return;
            }
            g_overlayFetchInFlight = true;
            g_lastOverlayFetchAt = now;
        }

        // Queue the fetch on the thread pool
        ThreadPool::getInstance().enqueue(
            "PrismaUIOverlayFetch",
            []() {
                struct OverlayFetchGuard {
                    ~OverlayFetchGuard()
                    {
                        std::lock_guard<std::mutex> lock(g_overlayFetchMutex);
                        g_overlayFetchInFlight = false;
                    }
                } guard;

                try {
                    // Fetch from server
                    std::string response = FetchOverlayFromServer();

                    if (response.empty()) {
                        logger::warn("[PrismaUIBridge] Empty overlay response from server");
                        return;
                    }

                    // Handle chunked transfer encoding
                    size_t jsonStart = response.find('{');
                    if (jsonStart == std::string::npos) {
                        logger::error("[PrismaUIBridge] No JSON object found in overlay response");
                        return;
                    }
                    
                    size_t jsonEnd = response.rfind('}');
                    if (jsonEnd == std::string::npos || jsonEnd < jsonStart) {
                        logger::error("[PrismaUIBridge] Malformed overlay JSON");
                        return;
                    }
                    
                    std::string jsonBody = response.substr(jsonStart, jsonEnd - jsonStart + 1);

                    // Parse JSON to validate
                    json parsed = json::parse(jsonBody, nullptr, false);
                    if (parsed.is_discarded()) {
                        logger::error("[PrismaUIBridge] Failed to parse overlay JSON");
                        return;
                    }

                    // Escape the JSON string for JavaScript
                    std::string escaped = jsonBody;
                    size_t pos = 0;
                    while ((pos = escaped.find('\\', pos)) != std::string::npos) {
                        escaped.replace(pos, 1, "\\\\");
                        pos += 2;
                    }
                    pos = 0;
                    while ((pos = escaped.find('\'', pos)) != std::string::npos) {
                        escaped.replace(pos, 1, "\\'");
                        pos += 2;
                    }
                    pos = 0;
                    while ((pos = escaped.find('\n', pos)) != std::string::npos) {
                        escaped.replace(pos, 1, "\\n");
                        pos += 2;
                    }
                    pos = 0;
                    while ((pos = escaped.find('\r', pos)) != std::string::npos) {
                        escaped.replace(pos, 1, "\\r");
                        pos += 2;
                    }

                    // Invoke the JavaScript update function
                    std::string jsCall = "window.updateOverlay('" + escaped + "')";

                    if (g_prismaUI && g_overlayCreated.load()) {
                        g_prismaUI->Invoke(g_overlayView, jsCall.c_str(), nullptr);
                        logger::debug("[PrismaUIBridge] Sent overlay update to UI");
                    }

                } catch (const std::exception& e) {
                    logger::error("[PrismaUIBridge] Error fetching overlay: {}", e.what());
                }
            },
            "OverlayFetch",
            std::chrono::seconds(30)
        );
    }

    static std::string FetchOverlayFromServer() {
        constexpr size_t BUFFER_SIZE = 4096;
        constexpr int TIMEOUT_SECONDS = 10;

        WSADATA wsaData;
        int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (iResult != 0) {
            logger::error("[PrismaUIBridge] WSAStartup failed: {}", iResult);
            return "";
        }

        SOCKET rawSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (rawSocket == INVALID_SOCKET) {
            logger::error("[PrismaUIBridge] Failed to create socket: {}", WSAGetLastError());
            WSACleanup();
            return "";
        }

        DWORD timeout = TIMEOUT_SECONDS * 1000;
        setsockopt(rawSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(rawSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        struct addrinfo hints;
        struct addrinfo* result = nullptr;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        std::string server = Conf::getInstance().getServer();
        std::string port = Conf::getInstance().getPort();

        int adHres = getaddrinfo(server.c_str(), port.c_str(), &hints, &result);
        if (adHres != 0) {
            logger::error("[PrismaUIBridge] getaddrinfo failed: {}", adHres);
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        bool connected = false;
        for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
            iResult = connect(rawSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
            if (iResult != SOCKET_ERROR) {
                connected = true;
                break;
            }
        }

        freeaddrinfo(result);

        if (!connected) {
            logger::error("[PrismaUIBridge] Could not connect to server");
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        std::string configPath = Conf::getInstance().getPath();
        std::string basePath;
        size_t lastSlash = configPath.find_last_of("/");
        if (lastSlash != std::string::npos) {
            basePath = configPath.substr(0, lastSlash + 1);
        }

        std::string requestPath = basePath + "ui/api/chim_overlay.php";
        if (!requestPath.empty() && requestPath[0] == '/') {
            requestPath = requestPath.substr(1);
        }

        logger::info("[PrismaUIBridge] Fetching overlay from: http://{}:{}/{}", server, port, requestPath);

        std::string httpRequest = "GET /" + requestPath + " HTTP/1.1\r\n";
        httpRequest += "Host: " + server + "\r\n";
        httpRequest += "Connection: close\r\n";
        httpRequest += "Accept: application/json\r\n";
        httpRequest += "\r\n";

        iResult = send(rawSocket, httpRequest.c_str(), (int)httpRequest.size(), 0);
        if (iResult == SOCKET_ERROR) {
            logger::error("[PrismaUIBridge] Failed to send request: {}", WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        std::string response;
        char buffer[BUFFER_SIZE];

        while (true) {
            iResult = recv(rawSocket, buffer, BUFFER_SIZE - 1, 0);
            if (iResult > 0) {
                buffer[iResult] = '\0';
                response += buffer;
            } else if (iResult == 0) {
                break;
            } else {
                int error = WSAGetLastError();
                if (error != WSAETIMEDOUT) {
                    logger::error("[PrismaUIBridge] recv failed: {}", error);
                }
                break;
            }
        }

        closesocket(rawSocket);
        WSACleanup();

        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            response = response.substr(headerEnd + 4);
        }

        return response;
    }

    // ===== CHIM Diaries Functions =====

    void CreateDiariesPanel() {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create diaries - Prisma UI not initialized");
            return;
        }

        if (g_diariesCreated.load()) {
            logger::debug("[PrismaUIBridge] Diaries panel already created");
            return;
        }

        logger::info("[PrismaUIBridge] Creating CHIM diaries panel from CHIM/diaries.html...");

        // Create the view - path is relative to Data/PrismaUI/views/
        g_diariesView = g_prismaUI->CreateView("CHIM/diaries.html", OnDiariesDomReady);

        if (g_diariesView == 0) {
            g_lastError = "Failed to create diaries view - check that Data/PrismaUI/views/CHIM/diaries.html exists";
            logger::error("[PrismaUIBridge] {}", g_lastError);
            return;
        }

        logger::info("[PrismaUIBridge] Diaries view created with ID: {}", g_diariesView);

        // Set view order (on top)
        g_prismaUI->SetOrder(g_diariesView, 110);

        // Register listener for commands from JS
        g_prismaUI->RegisterJSListener(g_diariesView, "chimDiariesCommand", OnDiariesCommand);

        g_diariesCreated.store(true);
        logger::info("[PrismaUIBridge] Diaries panel created successfully");
    }

    static void OnDiariesDomReady(PrismaView view) {
        logger::info("[PrismaUIBridge] Diaries panel DOM ready, triggering initial fetch");
        g_diariesDomReady.store(true);
        
        // Trigger initial people list fetch
        FetchDiariesData("people", "");
    }

    static void OnDiariesCommand(const char* argument) {
        if (!argument) {
            logger::warn("[PrismaUIBridge] Received diaries command with null argument");
            return;
        }

        std::string cmd(argument);
        logger::info("[PrismaUIBridge] Received diaries command: {}", cmd);

        if (cmd == "close") {
            HideDiariesPanel();
        } else if (cmd == "dom_ready") {
            logger::info("[PrismaUIBridge] Diaries DOM ready signal received");
            g_diariesDomReady.store(true);
        } else if (cmd.substr(0, 9) == "js_debug|") {
            // Debug messages from JavaScript
            std::string debugMsg = cmd.substr(9);
            logger::info("[PrismaUIBridge] JS DEBUG: {}", debugMsg);
        } else if (cmd.substr(0, 13) == "fetch_people") {
            // Fetch people list
            FetchDiariesData("people", "");
        } else if (cmd.substr(0, 14) == "fetch_entries|") {
            // Extract person name
            std::string person = cmd.substr(14);
            FetchDiariesData("entries", person);
        } else if (cmd.substr(0, 12) == "fetch_entry|") {
            // Extract entry ID
            std::string entryId = cmd.substr(12);
            FetchDiariesData("entry", entryId);
        } else if (cmd == "navigate_back") {
            logger::debug("[PrismaUIBridge] Navigation back in diaries");
        }
    }

    void ToggleDiariesPanel() {
        logger::info("[PrismaUIBridge] Diaries toggle requested (created={}, view={}, domReady={})", 
            g_diariesCreated.load(), g_diariesView, g_diariesDomReady.load());
        
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot toggle diaries - Prisma UI not initialized");
            return;
        }

        // If panel not created yet OR view is invalid, create it
        if (!g_diariesCreated.load() || g_diariesView == 0 || !g_prismaUI->IsValid(g_diariesView)) {
            logger::info("[PrismaUIBridge] Diaries not created or invalid, creating now...");
            CreateDiariesPanel();
            if (!g_diariesCreated.load() || g_diariesView == 0) {
                logger::error("[PrismaUIBridge] Failed to create diaries panel");
                return;
            }
            // Give it a moment for DOM to load
            logger::info("[PrismaUIBridge] Diaries created, showing immediately");
            ShowDiariesPanel();
            return;
        }

        // Check if DOM is ready
        if (!g_diariesDomReady.load()) {
            logger::warn("[PrismaUIBridge] Diaries DOM not ready yet, showing anyway");
            g_prismaUI->Show(g_diariesView);
            g_prismaUI->Focus(g_diariesView, false, true);
            return;
        }

        if (g_prismaUI->IsHidden(g_diariesView)) {
            logger::info("[PrismaUIBridge] Showing diaries");
            ShowDiariesPanel();
        } else {
            logger::info("[PrismaUIBridge] Hiding diaries");
            HideDiariesPanel();
        }
    }

    void ShowDiariesPanel() {
        if (!g_prismaUI || !g_diariesCreated.load()) {
            if (!g_diariesCreated.load()) {
                CreateDiariesPanel();
            }
            if (!g_diariesCreated.load()) {
                return;
            }
        }

        if (!g_prismaUI->IsValid(g_diariesView)) {
            logger::error("[PrismaUIBridge] Diaries view is not valid!");
            return;
        }

        logger::info("[PrismaUIBridge] Showing diaries panel");
        
        // Unfocus other views first to avoid focus conflicts
        if (g_panelCreated.load() && g_prismaUI->HasFocus(g_historyView)) {
            g_prismaUI->Unfocus(g_historyView);
            logger::debug("[PrismaUIBridge] Unfocused history view");
        }
        if (g_overlayCreated.load() && g_prismaUI->HasFocus(g_overlayView)) {
            g_prismaUI->Unfocus(g_overlayView);
            logger::debug("[PrismaUIBridge] Unfocused overlay view");
        }
        
        g_prismaUI->Show(g_diariesView);
        
        // Focus with game PAUSED and cursor enabled (pauseGame=true, disableFocusMenu=false)
        bool focusSuccess = g_prismaUI->Focus(g_diariesView, true, false);
        logger::info("[PrismaUIBridge] Focus on diaries (paused game, cursor enabled) result: {}", focusSuccess ? "SUCCESS" : "FAILED");
        
        // Verify focus was set
        if (g_prismaUI->HasFocus(g_diariesView)) {
            logger::info("[PrismaUIBridge] Diaries view confirmed to have focus");
        } else {
            logger::warn("[PrismaUIBridge] Diaries view does NOT have focus after Focus() call!");
        }
        
        // Only fetch if DOM is already ready (otherwise OnDiariesDomReady will fetch)
        if (g_diariesDomReady.load()) {
            logger::debug("[PrismaUIBridge] DOM ready, triggering data fetch for diaries panel");
            FetchDiariesData("people", "");
        } else {
            logger::debug("[PrismaUIBridge] DOM not ready yet, fetch will happen on DOM ready");
        }
    }

    void HideDiariesPanel() {
        if (!g_prismaUI || !g_diariesCreated.load()) {
            return;
        }

        logger::info("[PrismaUIBridge] Hiding diaries panel");
        
        // Remove focus first
        g_prismaUI->Unfocus(g_diariesView);
        
        g_prismaUI->Hide(g_diariesView);
    }

    bool IsDiariesPanelVisible() {
        if (!g_prismaUI || !g_diariesCreated.load()) {
            return false;
        }
        return !g_prismaUI->IsHidden(g_diariesView);
    }

    void FetchDiariesData(const std::string& mode, const std::string& param) {
        if (!g_prismaUI || !g_diariesCreated.load()) {
            return;
        }

        // Queue the fetch on the thread pool
        ThreadPool::getInstance().enqueue(
            "PrismaUIDiariesFetch",
            [mode, param]() {
                try {
                    // Construct URL based on mode
                    std::string url;
                    if (mode == "people") {
                        url = "/HerikaServer/ui/api/chim_diaries.php?list=people";
                    } else if (mode == "entries") {
                        // URL encode the person name
                        std::string encodedPerson = param;
                        // Simple URL encoding for spaces and special chars
                        size_t pos = 0;
                        while ((pos = encodedPerson.find(' ', pos)) != std::string::npos) {
                            encodedPerson.replace(pos, 1, "%20");
                            pos += 3;
                        }
                        url = "/HerikaServer/ui/api/chim_diaries.php?person=" + encodedPerson;
                    } else if (mode == "entry") {
                        url = "/HerikaServer/ui/api/chim_diaries.php?entry=" + param;
                    }

                    logger::info("[PrismaUIBridge] Fetching diaries data: mode={}, url={}", mode, url);

                    // Fetch from server using HTTP helper
                    std::string response = FetchDiariesFromServer(url);

                    if (response.empty()) {
                        logger::warn("[PrismaUIBridge] Empty diaries response from server for mode: {}", mode);
                        return;
                    }

                    logger::debug("[PrismaUIBridge] Diaries response (first 200 chars): {}", response.substr(0, 200));

                    // Parse JSON to validate
                    try {
                        json j = json::parse(response);
                        
                        if (!j.contains("success") || !j["success"].get<bool>()) {
                            logger::error("[PrismaUIBridge] Server returned error for diaries");
                            return;
                        }

                        logger::debug("[PrismaUIBridge] JSON parsed successfully, preparing JS call");

                        // Escape the JSON response for JavaScript string (same as history panel)
                        std::string escapedResponse = response;
                        size_t pos = 0;
                        // Escape backslashes first
                        while ((pos = escapedResponse.find('\\', pos)) != std::string::npos) {
                            escapedResponse.replace(pos, 1, "\\\\");
                            pos += 2;
                        }
                        // Then escape single quotes
                        pos = 0;
                        while ((pos = escapedResponse.find('\'', pos)) != std::string::npos) {
                            escapedResponse.replace(pos, 1, "\\'");
                            pos += 2;
                        }
                        // Escape newlines
                        pos = 0;
                        while ((pos = escapedResponse.find('\n', pos)) != std::string::npos) {
                            escapedResponse.replace(pos, 1, "\\n");
                            pos += 2;
                        }
                        // Escape carriage returns
                        pos = 0;
                        while ((pos = escapedResponse.find('\r', pos)) != std::string::npos) {
                            escapedResponse.replace(pos, 1, "\\r");
                            pos += 2;
                        }

                        logger::debug("[PrismaUIBridge] Response escaped, length: {}", escapedResponse.length());

                        // Invoke appropriate JS function based on mode
                        std::string jsCall;
                        if (mode == "people") {
                            jsCall = "window.updatePeopleList('" + escapedResponse + "')";
                        } else if (mode == "entries") {
                            jsCall = "window.updateEntriesList('" + escapedResponse + "')";
                        } else if (mode == "entry") {
                            jsCall = "window.updateDiaryContent('" + escapedResponse + "')";
                        }

                        logger::debug("[PrismaUIBridge] JS call prepared, length: {}", jsCall.length());

                        // Call Invoke directly like the history panel does
                        if (g_prismaUI && g_diariesCreated.load()) {
                            g_prismaUI->Invoke(g_diariesView, jsCall.c_str(), nullptr);
                            logger::debug("[PrismaUIBridge] Invoked diaries JS callback");
                        } else {
                            logger::warn("[PrismaUIBridge] Cannot invoke callback - view not available");
                        }

                    } catch (const json::exception& e) {
                        logger::error("[PrismaUIBridge] Failed to parse diaries JSON: {}", e.what());
                        logger::error("[PrismaUIBridge] JSON content: {}", response);
                    }

                } catch (const std::exception& e) {
                    logger::error("[PrismaUIBridge] Error fetching diaries data: {}", e.what());
                }
            }
        );
    }

    static std::string FetchDiariesFromServer(const std::string& url) {
        constexpr size_t BUFFER_SIZE = 8192;  // Larger buffer for diary content
        constexpr int TIMEOUT_SECONDS = 10;

        WSADATA wsaData;
        int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (iResult != 0) {
            logger::error("[PrismaUIBridge] WSAStartup failed: {}", iResult);
            return "";
        }

        SOCKET rawSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (rawSocket == INVALID_SOCKET) {
            logger::error("[PrismaUIBridge] Failed to create socket: {}", WSAGetLastError());
            WSACleanup();
            return "";
        }

        // Get server config
        std::string host = Conf::getInstance().getServer();
        std::string portStr = Conf::getInstance().getPort();
        int port = std::stoi(portStr);

        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);

        // Set timeout
        DWORD timeout = TIMEOUT_SECONDS * 1000;
        setsockopt(rawSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(rawSocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

        // Connect
        iResult = connect(rawSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
        if (iResult == SOCKET_ERROR) {
            logger::error("[PrismaUIBridge] Failed to connect to {}:{} - error: {}", host, port, WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        // Build HTTP request
        std::string request = "GET " + url + " HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Connection: close\r\n";
        request += "\r\n";

        // Send request
        iResult = send(rawSocket, request.c_str(), static_cast<int>(request.length()), 0);
        if (iResult == SOCKET_ERROR) {
            logger::error("[PrismaUIBridge] Failed to send request: {}", WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        // Receive response
        std::string response;
        char buffer[BUFFER_SIZE];

        while (true) {
            iResult = recv(rawSocket, buffer, BUFFER_SIZE - 1, 0);
            if (iResult > 0) {
                buffer[iResult] = '\0';
                response += buffer;
            } else if (iResult == 0) {
                break; // Connection closed
            } else {
                int error = WSAGetLastError();
                if (error != WSAETIMEDOUT) {
                    logger::error("[PrismaUIBridge] recv failed: {}", error);
                }
                break;
            }
        }

        closesocket(rawSocket);
        WSACleanup();

        // Extract body from HTTP response
        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            response = response.substr(headerEnd + 4);
        }

        return response;
    }

    // ===== CHIM Browser Functions =====

    static void OnBrowserDomReady(PrismaView view);
    static std::string BuildBrowserUrl(const char* pageName);
    static void DestroyBrowserPanelView();
    static bool EnsureBrowserPanelForUrl(const std::string& browserUrl, const char* sourceLabel);
    static bool FocusBrowserPanelView(const char* sourceLabel);
    static void HideOtherPanelsForBrowser();
    static void ShowBrowserPanelForUrl(const std::string& browserUrl, const char* sourceLabel);

    void CreateBrowserPanel() {
        ShowBrowserPanelForUrl(BuildBrowserUrl("home.php"), "browser");
    }

    static void OnBrowserCommand(const char* argument) {
        if (!argument) {
            return;
        }

        std::string cmd(argument);
        logger::debug("[PrismaUIBridge] Received browser command: {}", cmd);

        if (cmd == "close") {
            HideBrowserPanel();
            return;
        }

        if (cmd == "dom_ready") {
            return;
        }

        logger::warn("[PrismaUIBridge] Unknown browser command: {}", cmd);
    }

    static void OnBrowserDomReady(PrismaView view) {
        (void)view;
        logger::info("[PrismaUIBridge] Browser panel DOM ready");
        g_browserDomReady.store(true);
        RegisterBrowserConsoleDiagnostics();
        InstallBrowserDebugHooks();
        LogBrowserDebugState("dom_ready");
    }

    static std::string BuildBrowserUrl(const char* pageName) {
        std::string server = Conf::getInstance().getServer();
        std::string port = Conf::getInstance().getPort();
        return "http://" + server + ":" + port + "/HerikaServer/ui/" + pageName;
    }

    static void DestroyBrowserPanelView() {
        if (!g_prismaUI || !g_browserCreated.load()) {
            g_browserView = 0;
            g_browserCreated.store(false);
            g_browserDomReady.store(false);
            g_browserVisible.store(false);
            g_browserCurrentLabel = "browser";
            g_browserCurrentUrl.clear();
            return;
        }

        if (g_prismaUI->IsValid(g_browserView)) {
            if (g_prismaUI2) {
                g_prismaUI2->RegisterConsoleCallback(g_browserView, nullptr);
            }
            if (g_prismaUI->HasFocus(g_browserView)) {
                g_prismaUI->Unfocus(g_browserView);
            }
            g_prismaUI->Destroy(g_browserView);
        }

        g_browserView = 0;
        g_browserCreated.store(false);
        g_browserDomReady.store(false);
        g_browserVisible.store(false);
        g_browserCurrentLabel = "browser";
        g_browserCurrentUrl.clear();
    }

    static bool EnsureBrowserPanelForUrl(const std::string& browserUrl, const char* sourceLabel) {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create {} view - Prisma UI not initialized", sourceLabel);
            return false;
        }

        if (g_browserCreated.load() && g_prismaUI->IsValid(g_browserView) && g_browserCurrentUrl == browserUrl) {
            return true;
        }

        if (g_browserCreated.load()) {
            logger::info("[PrismaUIBridge] Recreating browser view for {} URL: {}", sourceLabel, browserUrl);
            DestroyBrowserPanelView();
        } else {
            logger::info("[PrismaUIBridge] Creating browser view for {} URL: {}", sourceLabel, browserUrl);
        }

        g_browserDomReady.store(false);
        g_browserView = g_prismaUI->CreateView(browserUrl.c_str(), OnBrowserDomReady);

        if (g_browserView == 0) {
            g_lastError = std::format("Failed to create browser view for URL {}", browserUrl);
            logger::error("[PrismaUIBridge] {}", g_lastError);
            return false;
        }

        g_prismaUI->SetOrder(g_browserView, 120);
        g_prismaUI->SetScrollingPixelSize(g_browserView, 100);
        g_prismaUI->RegisterJSListener(g_browserView, "chimBrowserCommand", OnBrowserCommand);

        g_browserCurrentLabel = sourceLabel;
        g_browserCurrentUrl = browserUrl;
        g_browserCreated.store(true);
        RegisterBrowserConsoleDiagnostics();
        logger::info("[PrismaUIBridge] Browser view created with ID: {}", g_browserView);
        return true;
    }

    static bool FocusBrowserPanelView(const char* sourceLabel) {
        constexpr auto kDomReadyPollInterval = std::chrono::milliseconds(20);
        constexpr auto kDomReadyTimeout = std::chrono::milliseconds(3000);

        if (!g_prismaUI || !g_browserCreated.load() || !g_prismaUI->IsValid(g_browserView)) {
            logger::warn("[PrismaUIBridge] Cannot focus {} view - browser is not ready", sourceLabel);
            return false;
        }

        if (!g_browserDomReady.load()) {
            logger::info("[PrismaUIBridge] Waiting for {} browser DOM ready...", sourceLabel);
            const auto waitStart = std::chrono::steady_clock::now();
            while (!g_browserDomReady.load()) {
                const auto waited = std::chrono::steady_clock::now() - waitStart;
                if (waited >= kDomReadyTimeout) {
                    const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(waited).count();
                    logger::warn("[PrismaUIBridge] {} browser DOM ready timeout after {} ms", sourceLabel, waitedMs);
                    break;
                }
                std::this_thread::sleep_for(kDomReadyPollInterval);
            }
        }

        bool success = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            success = g_prismaUI->Focus(g_browserView, true, false);
            if (success) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        logger::info("[PrismaUIBridge] {} browser focus result: {} (hasFocus={}, anyFocus={})",
            sourceLabel,
            success ? "SUCCESS" : "FAILED",
            g_prismaUI->HasFocus(g_browserView),
            g_prismaUI->HasAnyActiveFocus());

        LogBrowserDebugState(success ? "after_focus_success" : "after_focus_failed");
        if (success) {
            g_prismaUI->Invoke(g_browserView, "window.__chimBrowserFocusDeep && window.__chimBrowserFocusDeep('native_after_focus')", nullptr);
        }

        return success;
    }

    static void HideOtherPanelsForBrowser() {
        if (g_panelCreated.load()) {
            if (g_prismaUI->HasFocus(g_historyView)) {
                g_prismaUI->Unfocus(g_historyView);
                logger::debug("[PrismaUIBridge] Unfocused history view");
            }
            if (!g_prismaUI->IsHidden(g_historyView)) {
                g_prismaUI->Hide(g_historyView);
            }
        }
        if (g_overlayCreated.load()) {
            if (g_prismaUI->HasFocus(g_overlayView)) {
                g_prismaUI->Unfocus(g_overlayView);
                logger::debug("[PrismaUIBridge] Unfocused overlay view");
            }
            if (!g_prismaUI->IsHidden(g_overlayView)) {
                g_prismaUI->Hide(g_overlayView);
            }
        }
        if (g_diariesCreated.load()) {
            if (g_prismaUI->HasFocus(g_diariesView)) {
                g_prismaUI->Unfocus(g_diariesView);
                logger::debug("[PrismaUIBridge] Unfocused diaries view");
            }
            if (!g_prismaUI->IsHidden(g_diariesView)) {
                g_prismaUI->Hide(g_diariesView);
            }
        }
        if (g_questManagerCreated.load() && !g_prismaUI->IsHidden(g_questManagerView)) {
            HideQuestManagerPanel();
        }
        if (g_settingsMenuCreated.load() && !g_prismaUI->IsHidden(g_settingsMenuView)) {
            HideSettingsMenu();
        }
        if (g_masterMenuCreated.load() && !g_prismaUI->IsHidden(g_masterMenuView)) {
            HideMasterMenu();
        }
        HideDebuggerPanel();
    }

    static void ShowBrowserPanelForUrl(const std::string& browserUrl, const char* sourceLabel) {
        if (!EnsureBrowserPanelForUrl(browserUrl, sourceLabel)) {
            return;
        }

        logger::info("[PrismaUIBridge] Showing {} browser panel at {}", sourceLabel, browserUrl);

        HideOtherPanelsForBrowser();

        if (g_prismaUI->IsHidden(g_browserView)) {
            g_prismaUI->Show(g_browserView);
        }

        if (FocusBrowserPanelView(sourceLabel)) {
            g_browserVisible.store(true);
            InstallBrowserDebugHooks();
            logger::info("[PrismaUIBridge] {} browser panel ready - press the toggle hotkey again to close", sourceLabel);
            return;
        }

        logger::warn("[PrismaUIBridge] Failed to focus {} browser panel", sourceLabel);
        g_browserVisible.store(false);
    }

    void ToggleBrowserPanel() {
        const std::string browserUrl = BuildBrowserUrl("home.php");
        logger::info("[PrismaUIBridge] Browser toggle requested (created={}, view={}, domReady={}, visible={}, url={})",
            g_browserCreated.load(), g_browserView, g_browserDomReady.load(), g_browserVisible.load(), g_browserCurrentUrl);

        if (g_browserVisible.load() && g_browserCurrentUrl == browserUrl) {
            logger::info("[PrismaUIBridge] Hiding browser (was visible)");
            HideBrowserPanel();
            return;
        }

        ShowBrowserPanelForUrl(browserUrl, "browser");
    }

    void ShowBrowserPanel() {
        ShowBrowserPanelForUrl(BuildBrowserUrl("home.php"), "browser");
    }

    void HideBrowserPanel() {
        if (!g_prismaUI || !g_browserCreated.load()) {
            return;
        }

        logger::info("[PrismaUIBridge] Hiding browser panel");
        
        // Remove focus first
        if (g_prismaUI->HasFocus(g_browserView)) {
            g_prismaUI->Unfocus(g_browserView);
        }

        g_prismaUI->Hide(g_browserView);

        // Mark as hidden
        g_browserVisible.store(false);
    }

    bool IsBrowserPanelVisible() {
        if (!g_prismaUI || !g_browserCreated.load()) {
            return false;
        }
        return !g_prismaUI->IsHidden(g_browserView);
    }

    // ===== CHIM Quest Manager Functions =====

    void CreateQuestManagerPanel() {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create quest manager - Prisma UI not initialized");
            return;
        }

        if (g_questManagerCreated.load()) {
            logger::debug("[PrismaUIBridge] Quest manager panel already created");
            return;
        }

        logger::info("[PrismaUIBridge] Creating CHIM quest manager panel from CHIM/quest_manager.html...");
        g_questManagerView = g_prismaUI->CreateView("CHIM/quest_manager.html", OnQuestManagerDomReady);

        if (g_questManagerView == 0) {
            g_lastError = "Failed to create quest manager view - check that Data/PrismaUI/views/CHIM/quest_manager.html exists";
            logger::error("[PrismaUIBridge] {}", g_lastError);
            return;
        }

        g_prismaUI->SetOrder(g_questManagerView, 130);
        g_prismaUI->SetScrollingPixelSize(g_questManagerView, 90);
        g_prismaUI->RegisterJSListener(g_questManagerView, "chimQuestManagerCommand", OnQuestManagerCommand);
        g_prismaUI->Hide(g_questManagerView);

        g_questManagerCreated.store(true);
        logger::info("[PrismaUIBridge] Quest manager panel created successfully");
    }

    static void OnQuestManagerDomReady(PrismaView view) {
        logger::info("[PrismaUIBridge] Quest manager panel DOM ready");
        g_questManagerDomReady.store(true);

        if (!g_prismaUI || !g_questManagerCreated.load()) {
            return;
        }

        std::string server = Conf::getInstance().getServer();
        std::string port = Conf::getInstance().getPort();
        std::string serverUrl = "http://" + server + ":" + port + "/HerikaServer";
        std::string jsCall = "window.initQuestManager('" + EscapeForJS(serverUrl) + "')";
        g_prismaUI->Invoke(g_questManagerView, jsCall.c_str(), nullptr);
        if (g_prismaUI->HasFocus(g_questManagerView)) {
            g_prismaUI->Invoke(g_questManagerView,
                               "window.onQuestManagerFocused && window.onQuestManagerFocused()",
                               nullptr);
        }
        g_prismaUI->Invoke(g_questManagerView,
                           "window.requestQuestManagerFocus && window.requestQuestManagerFocus('dom_ready')",
                           nullptr);
    }

    static void OnQuestManagerCommand(const char* argument) {
        if (!argument) {
            return;
        }

        std::string cmd(argument);
        logger::debug("[PrismaUIBridge] Received quest manager command: {}", cmd);

        if (cmd == "close") {
            HideQuestManagerPanel();
            return;
        }

        if (cmd == "dom_ready") {
            g_questManagerDomReady.store(true);
            return;
        }

        logger::warn("[PrismaUIBridge] Unknown quest manager command: {}", cmd);
    }

    void ToggleQuestManagerPanel() {
        logger::info("[PrismaUIBridge] Quest manager toggle requested");

        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot toggle quest manager - Prisma UI not initialized");
            return;
        }

        if (!g_questManagerCreated.load()) {
            CreateQuestManagerPanel();
            if (!g_questManagerCreated.load()) {
                logger::error("[PrismaUIBridge] Failed to create quest manager panel");
                return;
            }
        }

        if (!g_questManagerVisible.load()) {
            ShowQuestManagerPanel();
        } else {
            HideQuestManagerPanel();
        }
    }

    void ShowQuestManagerPanel() {
        if (!g_prismaUI || !g_questManagerCreated.load()) {
            if (!g_questManagerCreated.load()) {
                CreateQuestManagerPanel();
            }
            if (!g_questManagerCreated.load()) {
                return;
            }
        }

        if (!g_prismaUI->IsValid(g_questManagerView)) {
            logger::error("[PrismaUIBridge] Quest manager view is not valid");
            return;
        }

        if (g_panelCreated.load() && !g_prismaUI->IsHidden(g_historyView)) {
            HideHistoryPanel();
        }
        if (g_overlayCreated.load() && !g_prismaUI->IsHidden(g_overlayView)) {
            HideOverlayPanel();
        }
        if (g_diariesCreated.load() && !g_prismaUI->IsHidden(g_diariesView)) {
            HideDiariesPanel();
        }
        if (g_browserCreated.load() && !g_prismaUI->IsHidden(g_browserView)) {
            HideBrowserPanel();
        }
        if (g_settingsMenuCreated.load() && !g_prismaUI->IsHidden(g_settingsMenuView)) {
            HideSettingsMenu();
        }
        if (g_masterMenuCreated.load() && !g_prismaUI->IsHidden(g_masterMenuView)) {
            HideMasterMenu();
        }

        g_prismaUI->Show(g_questManagerView);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_prismaUI->SetScrollingPixelSize(g_questManagerView, 90);

        bool focusSuccess = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            focusSuccess = g_prismaUI->Focus(g_questManagerView, true, false);
            if (focusSuccess) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        logger::info("[PrismaUIBridge] Quest manager focus result: {} (hasFocus={})",
                     focusSuccess ? "SUCCESS" : "FAILED",
                     g_prismaUI->HasFocus(g_questManagerView) ? "true" : "false");

        if (g_questManagerDomReady.load()) {
            std::string server = Conf::getInstance().getServer();
            std::string port = Conf::getInstance().getPort();
            std::string serverUrl = "http://" + server + ":" + port + "/HerikaServer";
            std::string jsCall = "window.initQuestManager('" + EscapeForJS(serverUrl) + "')";
            g_prismaUI->Invoke(g_questManagerView, jsCall.c_str(), nullptr);
            g_prismaUI->Invoke(g_questManagerView,
                               "window.onQuestManagerFocused && window.onQuestManagerFocused()",
                               nullptr);
            g_prismaUI->Invoke(g_questManagerView,
                               "window.requestQuestManagerFocus && window.requestQuestManagerFocus('native_after_focus')",
                               nullptr);
        }

        g_questManagerVisible.store(true);
        logger::info("[PrismaUIBridge] Quest manager panel shown");
    }

    void HideQuestManagerPanel() {
        if (!g_prismaUI || !g_questManagerCreated.load()) {
            return;
        }

        if (g_prismaUI->HasFocus(g_questManagerView)) {
            if (g_questManagerDomReady.load()) {
                g_prismaUI->Invoke(g_questManagerView,
                                   "window.onQuestManagerUnfocused && window.onQuestManagerUnfocused()",
                                   nullptr);
            }
            g_prismaUI->Unfocus(g_questManagerView);
        }

        g_prismaUI->Hide(g_questManagerView);
        g_questManagerVisible.store(false);
        logger::info("[PrismaUIBridge] Quest manager panel hidden");
    }

    bool IsQuestManagerPanelVisible() {
        if (!g_prismaUI || !g_questManagerCreated.load()) {
            return false;
        }
        return !g_prismaUI->IsHidden(g_questManagerView);
    }

    // ===== CHIM AI View Functions =====

    static PrismaView g_aiviewView = 0;
    static std::atomic<bool> g_aiviewCreated{false};
    static std::atomic<bool> g_aiviewDomReady{false};
    static std::string g_lastTargetNpcName = "";
    static std::string g_lastTargetRefId = "";

    static void OnAIViewDomReady(PrismaView view);
    static void OnAIViewCommand(const char* argument);
    static std::string FetchAIViewFromServer(const std::string& npcName, const std::string& refid);

    // ===== AI View Functions =====
    
    void CheckAndUpdateAIView() {
        // Poll UI target state quickly; SpatialSnapshotManager owns the heavier cache/TTL.
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastAIViewCheck).count() < 250) {
            return;
        }
        g_lastAIViewCheck = now;
        
        // Only update if AI View is visible
        if (!g_aiviewCreated.load() || !g_prismaUI || g_prismaUI->IsHidden(g_aiviewView)) {
            return;
        }
        
        const auto target = GetPrimaryPrismaTarget("prismaui_ai_view");

        if (target.hasTarget && target.targetable) {
            if (target.name != g_lastAIViewTarget || target.formId != g_lastAIViewFormId) {
                // Get RefID as hex string
                std::stringstream ss;
                ss << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << target.formId;
                std::string targetRefId = ss.str();
                
                logger::debug("[PrismaUIBridge] AI View target: {} ({}, reason={}) - RefID: {}",
                              target.name, target.source, target.reason, targetRefId);
                
                FetchAndUpdateAIView(target.name, targetRefId);
                g_lastAIViewTarget = target.name;
                g_lastAIViewFormId = target.formId;
            }
        } else {
            // No valid target found - clear if we had one before
            if (!g_lastAIViewTarget.empty()) {
                logger::debug("[PrismaUIBridge] AI View spatial target cleared");
                // Clear the UI
                if (g_prismaUI && g_aiviewDomReady.load()) {
                    g_prismaUI->Invoke(g_aiviewView, "window.clearTarget()", nullptr);
                }
                g_lastAIViewTarget.clear();
                g_lastAIViewFormId = 0;
            }
        }
    }

    void CreateAIViewPanel() {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create AI View - Prisma UI not initialized");
            return;
        }

        if (g_aiviewCreated.load()) {
            logger::debug("[PrismaUIBridge] AI View panel already created");
            return;
        }

        logger::info("[PrismaUIBridge] Creating CHIM AI View panel from CHIM/aiview.html...");

        // Create the view - path is relative to Data/PrismaUI/views/
        g_aiviewView = g_prismaUI->CreateView("CHIM/aiview.html", OnAIViewDomReady);

        if (g_aiviewView == 0) {
            g_lastError = "Failed to create AI View - check that Data/PrismaUI/views/CHIM/aiview.html exists";
            logger::error("[PrismaUIBridge] {}", g_lastError);
            return;
        }

        logger::info("[PrismaUIBridge] AI View created with ID: {}", g_aiviewView);

        // Set view order (on top, but below browser)
        g_prismaUI->SetOrder(g_aiviewView, 115);

        // Set scroll speed
        g_prismaUI->SetScrollingPixelSize(g_aiviewView, 120);

        // Register listener for commands from JS
        g_prismaUI->RegisterJSListener(g_aiviewView, "chimAIViewCommand", OnAIViewCommand);

        g_aiviewCreated.store(true);
        logger::info("[PrismaUIBridge] AI View panel created successfully");
    }

    static void OnAIViewDomReady(PrismaView view) {
        logger::info("[PrismaUIBridge] AI View panel DOM ready");
        g_aiviewDomReady.store(true);
    }

    static void OnAIViewCommand(const char* argument) {
        if (!argument) {
            logger::warn("[PrismaUIBridge] Received AI View command with null argument");
            return;
        }

        std::string cmd(argument);
        logger::info("[PrismaUIBridge] Received AI View command: {}", cmd);

        if (cmd == "close") {
            HideAIViewPanel();
        } else if (cmd == "refresh") {
            // Refresh with the last known target
            if (!g_lastTargetNpcName.empty() || !g_lastTargetRefId.empty()) {
                FetchAndUpdateAIView(g_lastTargetNpcName, g_lastTargetRefId);
            }
        }
    }

    void ToggleAIViewPanel() {
        logger::info("[PrismaUIBridge] AI View toggle requested (created={}, view={}, domReady={})", 
            g_aiviewCreated.load(), g_aiviewView, g_aiviewDomReady.load());
        
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot toggle AI View - Prisma UI not initialized");
            return;
        }

        // If panel not created yet, create it
        if (!g_aiviewCreated.load() || g_aiviewView == 0 || !g_prismaUI->IsValid(g_aiviewView)) {
            logger::info("[PrismaUIBridge] AI View not created or invalid, creating now...");
            CreateAIViewPanel();
            if (!g_aiviewCreated.load() || g_aiviewView == 0) {
                logger::error("[PrismaUIBridge] Failed to create AI View panel");
                return;
            }
        }

        // Check if DOM is ready
        if (!g_aiviewDomReady.load()) {
            logger::warn("[PrismaUIBridge] AI View DOM not ready yet, showing anyway");
            g_prismaUI->Show(g_aiviewView);
            return;
        }

        if (g_prismaUI->IsHidden(g_aiviewView)) {
            logger::info("[PrismaUIBridge] Showing AI View");
            ShowAIViewPanel();
        } else {
            logger::info("[PrismaUIBridge] Hiding AI View");
            HideAIViewPanel();
        }
    }

    void ShowAIViewPanel() {
        if (!g_prismaUI || !g_aiviewCreated.load()) {
            if (!g_aiviewCreated.load()) {
                CreateAIViewPanel();
            }
            if (!g_aiviewCreated.load()) {
                return;
            }
        }

        if (!g_prismaUI->IsValid(g_aiviewView)) {
            logger::error("[PrismaUIBridge] AI View is not valid!");
            return;
        }

        logger::info("[PrismaUIBridge] Showing AI View panel");
        
        g_prismaUI->Show(g_aiviewView);
        
        // Do NOT focus - this is a passive HUD like the overlay
        // Player should maintain control and it updates based on crosshair target
        
        // Try to detect and fetch data for the current crosshair target
        DetectAndFetchTargetNPC();
    }

    void HideAIViewPanel() {
        if (!g_prismaUI || !g_aiviewCreated.load()) {
            return;
        }

        logger::info("[PrismaUIBridge] Hiding AI View panel");
        
        // No need to unfocus since we never focus it
        g_prismaUI->Hide(g_aiviewView);
    }

    bool IsAIViewPanelVisible() {
        if (!g_prismaUI || !g_aiviewCreated.load()) {
            return false;
        }
        return !g_prismaUI->IsHidden(g_aiviewView);
    }

    void DetectAndFetchTargetNPC() {
        // Try to get the crosshair target first
        RE::Actor* targetActor = nullptr;
        std::string targetName = "";
        std::string targetRefId = "";
        
        auto crosshairTarget = RE::CrosshairPickData::GetSingleton()->target;
        
        if (crosshairTarget && crosshairTarget.get()->GetFormType() == RE::FormType::ActorCharacter) {
            auto potentialTarget = crosshairTarget.get()->As<RE::Actor>();
            
            // Check if this actor is an AI agent
            AIAgentManager& aiam = AIAgentManager::getInstance();
            for (const auto& agent : aiam.getAgents()) {
                if (agent->getActor() && agent->getActor()->GetFormID() == potentialTarget->GetFormID()) {
                    targetActor = potentialTarget;
                    break;
                }
            }
        }
        
        // If no crosshair target, find closest AI agent
        if (!targetActor) {
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto player = RE::PlayerCharacter::GetSingleton();
            auto playerPos = player->GetPosition();
            
            float closestDistance = 500.0f;
            
            for (const auto& agent : aiam.getAgents()) {
                auto agentActor = agent->getActor();
                if (!agentActor) continue;
                if (agentActor->IsDead()) continue;
                if (agent->isNarrator()) continue;
                if (agentActor->GetParentCell() != player->GetParentCell()) continue;
                
                float distance = playerPos.GetDistance(agentActor->GetPosition());
                
                if (distance < closestDistance) {
                    closestDistance = distance;
                    targetActor = agentActor;
                }
            }
        }
        
        if (targetActor) {
            targetName = targetActor->GetDisplayFullName();
            
            // Get RefID as hex string
            uint32_t formId = targetActor->GetFormID();
            std::stringstream ss;
            ss << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << formId;
            targetRefId = ss.str();
            
            logger::info("[PrismaUIBridge] Detected target NPC: {} (RefID: {})", targetName, targetRefId);
            
            // Notify JS and fetch data
            FetchAndUpdateAIView(targetName, targetRefId);
        } else {
            logger::info("[PrismaUIBridge] No AI agent target found");
        }
    }

    void FetchAndUpdateAIView(const std::string& npcName, const std::string& refid) {
        if (!g_prismaUI || !g_aiviewCreated.load()) {
            logger::warn("[PrismaUIBridge] Cannot fetch AI View - UI not created (prismaUI={}, created={})", 
                (g_prismaUI != nullptr), g_aiviewCreated.load());
            return;
        }
        
        if (!g_aiviewDomReady.load()) {
            logger::warn("[PrismaUIBridge] Cannot fetch AI View - DOM not ready yet");
            return;
        }

        logger::info("[PrismaUIBridge] Fetching AI View for: {} (RefID: {})", npcName, refid);

        // Store last target
        g_lastTargetNpcName = npcName;
        g_lastTargetRefId = refid;

        // Notify JS that we're setting the target (triggers loading state)
        std::string escapedName = npcName;
        size_t pos = 0;
        while ((pos = escapedName.find('\'', pos)) != std::string::npos) {
            escapedName.replace(pos, 1, "\\'");
            pos += 2;
        }
        
        std::string jsCall = "window.setTargetNPC('" + escapedName + "', '" + refid + "')";
        logger::debug("[PrismaUIBridge] Invoking JS: {}", jsCall);
        g_prismaUI->Invoke(g_aiviewView, jsCall.c_str(), nullptr);

        // Queue the fetch on the thread pool
        ThreadPool::getInstance().enqueue(
            "PrismaUIAIViewFetch",
            [npcName, refid]() {
                try {
                    logger::info("[PrismaUIBridge] Starting server fetch for AI View data...");
                    
                    // Fetch from server
                    std::string response = FetchAIViewFromServer(npcName, refid);

                    if (response.empty()) {
                        logger::error("[PrismaUIBridge] Empty AI View response from server");
                        
                        // Send error to UI
                        if (g_prismaUI && g_aiviewCreated.load()) {
                            g_prismaUI->Invoke(g_aiviewView, "window.showError('No data received from server')", nullptr);
                        }
                        return;
                    }

                    logger::info("[PrismaUIBridge] Received {} bytes from server", response.length());

                    // Handle chunked transfer encoding
                    size_t jsonStart = response.find('{');
                    if (jsonStart == std::string::npos) {
                        logger::error("[PrismaUIBridge] No JSON object found in AI View response");
                        logger::debug("[PrismaUIBridge] Response preview: {}", 
                            response.substr(0, std::min(response.length(), size_t(200))));
                        
                        if (g_prismaUI && g_aiviewCreated.load()) {
                            g_prismaUI->Invoke(g_aiviewView, "window.showError('Invalid server response')", nullptr);
                        }
                        return;
                    }
                    
                    size_t jsonEnd = response.rfind('}');
                    if (jsonEnd == std::string::npos || jsonEnd < jsonStart) {
                        logger::error("[PrismaUIBridge] Malformed AI View JSON");
                        
                        if (g_prismaUI && g_aiviewCreated.load()) {
                            g_prismaUI->Invoke(g_aiviewView, "window.showError('Malformed JSON response')", nullptr);
                        }
                        return;
                    }
                    
                    std::string jsonBody = response.substr(jsonStart, jsonEnd - jsonStart + 1);
                    logger::debug("[PrismaUIBridge] Extracted JSON ({} bytes)", jsonBody.length());

                    // Parse JSON to validate
                    json parsed = json::parse(jsonBody, nullptr, false);
                    if (parsed.is_discarded()) {
                        logger::error("[PrismaUIBridge] Failed to parse AI View JSON");
                        
                        if (g_prismaUI && g_aiviewCreated.load()) {
                            g_prismaUI->Invoke(g_aiviewView, "window.showError('Failed to parse JSON')", nullptr);
                        }
                        return;
                    }

                    // Check if it's an error response
                    if (parsed.contains("success") && parsed["success"] == false) {
                        std::string errorMsg = parsed.value("error", "Unknown error");
                        logger::warn("[PrismaUIBridge] Server returned error: {}", errorMsg);
                        
                        // Still send to UI so it can display the error properly
                        // Don't return here - let the JS handle the error display
                    } else {
                        logger::info("[PrismaUIBridge] Successfully parsed AI View JSON");
                    }

                    // Escape the JSON string for JavaScript
                    std::string escaped = jsonBody;
                    size_t escapePos = 0;
                    while ((escapePos = escaped.find('\\', escapePos)) != std::string::npos) {
                        escaped.replace(escapePos, 1, "\\\\");
                        escapePos += 2;
                    }
                    escapePos = 0;
                    while ((escapePos = escaped.find('\'', escapePos)) != std::string::npos) {
                        escaped.replace(escapePos, 1, "\\'");
                        escapePos += 2;
                    }
                    escapePos = 0;
                    while ((escapePos = escaped.find('\n', escapePos)) != std::string::npos) {
                        escaped.replace(escapePos, 1, "\\n");
                        escapePos += 2;
                    }
                    escapePos = 0;
                    while ((escapePos = escaped.find('\r', escapePos)) != std::string::npos) {
                        escaped.replace(escapePos, 1, "\\r");
                        escapePos += 2;
                    }

                    // Invoke the JavaScript update function
                    std::string jsCall = "window.updateAIView('" + escaped + "')";

                    if (g_prismaUI && g_aiviewCreated.load()) {
                        g_prismaUI->Invoke(g_aiviewView, jsCall.c_str(), nullptr);
                        logger::info("[PrismaUIBridge] Successfully sent AI View update to UI");
                    } else {
                        logger::error("[PrismaUIBridge] UI no longer available when trying to send update");
                    }

                } catch (const std::exception& e) {
                    logger::error("[PrismaUIBridge] Error fetching AI View: {}", e.what());
                    
                    if (g_prismaUI && g_aiviewCreated.load()) {
                        std::string errorMsg = std::string("window.showError('Error: ") + e.what() + "')";
                        g_prismaUI->Invoke(g_aiviewView, errorMsg.c_str(), nullptr);
                    }
                }
            },
            "AIViewFetch",
            std::chrono::seconds(30)
        );
    }

    static std::string FetchAIViewFromServer(const std::string& npcName, const std::string& refid) {
        constexpr size_t BUFFER_SIZE = 8192;
        constexpr int TIMEOUT_SECONDS = 10;

        WSADATA wsaData;
        int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (iResult != 0) {
            logger::error("[PrismaUIBridge] WSAStartup failed: {}", iResult);
            return "";
        }

        SOCKET rawSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (rawSocket == INVALID_SOCKET) {
            logger::error("[PrismaUIBridge] Failed to create socket: {}", WSAGetLastError());
            WSACleanup();
            return "";
        }

        DWORD timeout = TIMEOUT_SECONDS * 1000;
        setsockopt(rawSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(rawSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        struct addrinfo hints;
        struct addrinfo* result = nullptr;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        std::string server = Conf::getInstance().getServer();
        std::string port = Conf::getInstance().getPort();

        int adHres = getaddrinfo(server.c_str(), port.c_str(), &hints, &result);
        if (adHres != 0) {
            logger::error("[PrismaUIBridge] getaddrinfo failed: {}", adHres);
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        bool connected = false;
        for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
            iResult = connect(rawSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
            if (iResult != SOCKET_ERROR) {
                connected = true;
                break;
            }
        }

        freeaddrinfo(result);

        if (!connected) {
            logger::error("[PrismaUIBridge] Could not connect to server");
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        std::string configPath = Conf::getInstance().getPath();
        std::string basePath;
        size_t lastSlash = configPath.find_last_of("/");
        if (lastSlash != std::string::npos) {
            basePath = configPath.substr(0, lastSlash + 1);
        }

        // URL encode the NPC name
        std::string encodedName = npcName;
        size_t pos = 0;
        while ((pos = encodedName.find(' ', pos)) != std::string::npos) {
            encodedName.replace(pos, 1, "%20");
            pos += 3;
        }

        std::string requestPath = basePath + "ui/api/chim_aiview.php?npc_name=" + encodedName;
        if (!refid.empty()) {
            requestPath += "&refid=" + refid;
        }
        
        if (!requestPath.empty() && requestPath[0] == '/') {
            requestPath = requestPath.substr(1);
        }

        logger::info("[PrismaUIBridge] Fetching AI View from: http://{}:{}/{}", server, port, requestPath);

        std::string httpRequest = "GET /" + requestPath + " HTTP/1.1\r\n";
        httpRequest += "Host: " + server + "\r\n";
        httpRequest += "Connection: close\r\n";
        httpRequest += "Accept: application/json\r\n";
        httpRequest += "\r\n";

        iResult = send(rawSocket, httpRequest.c_str(), (int)httpRequest.size(), 0);
        if (iResult == SOCKET_ERROR) {
            logger::error("[PrismaUIBridge] Failed to send request: {}", WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        std::string response;
        char buffer[BUFFER_SIZE];

        while (true) {
            iResult = recv(rawSocket, buffer, BUFFER_SIZE - 1, 0);
            if (iResult > 0) {
                buffer[iResult] = '\0';
                response += buffer;
            } else if (iResult == 0) {
                break;
            } else {
                int error = WSAGetLastError();
                if (error != WSAETIMEDOUT) {
                    logger::error("[PrismaUIBridge] recv failed: {}", error);
                }
                break;
            }
        }

        closesocket(rawSocket);
        WSACleanup();

        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            response = response.substr(headerEnd + 4);
        }

        return response;
    }

    // ===== CHIM Debugger Panel Functions =====

    static PrismaView g_debuggerView = 0;
    static std::atomic<bool> g_debuggerCreated{false};
    static std::atomic<bool> g_debuggerDomReady{false};
    static std::chrono::steady_clock::time_point g_lastDebuggerRefresh;

    static void OnDebuggerDomReady(PrismaView view);
    static void OnDebuggerCommand(const char* argument);
    static std::string FetchDebuggerFromServer();
    static std::string FetchLogFromServer(const std::string& logType, int numLines);
    static void FetchAndUpdateChimLog();
    static void FetchAndUpdateApacheLog();
    static void FetchAndUpdateLLMLogs();

    // Helper function to properly escape JSON strings for embedding in JavaScript (using single quotes)
    // This matches the pattern used by the overlay system
    static std::string EscapeJSONForJS(const std::string& json) {
        std::string escaped = json;
        size_t pos = 0;
        
        // Escape backslashes first (must be done before other escapes)
        while ((pos = escaped.find('\\', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\\\");
            pos += 2;
        }
        
        // Escape single quotes (we'll wrap in single quotes, so this is critical)
        pos = 0;
        while ((pos = escaped.find('\'', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\'");
            pos += 2;
        }
        
        // Escape newlines (actual newline characters, not \n in JSON)
        pos = 0;
        while ((pos = escaped.find('\n', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\n");
            pos += 2;
        }
        
        // Escape carriage returns
        pos = 0;
        while ((pos = escaped.find('\r', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\r");
            pos += 2;
        }
        
        // Escape tabs
        pos = 0;
        while ((pos = escaped.find('\t', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\t");
            pos += 2;
        }
        
        // Remove any null bytes that might have snuck through
        pos = 0;
        while ((pos = escaped.find('\0', pos)) != std::string::npos) {
            escaped.erase(pos, 1);
        }
        
        // Escape Unicode line separator (U+2028) and paragraph separator (U+2029)
        // These are valid JSON but break JavaScript string literals
        const std::string lineSep = "\xE2\x80\xA8";  // UTF-8 encoding of U+2028
        pos = 0;
        while ((pos = escaped.find(lineSep, pos)) != std::string::npos) {
            escaped.replace(pos, 3, "\\u2028");
            pos += 6;
        }
        
        const std::string paraSep = "\xE2\x80\xA9";  // UTF-8 encoding of U+2029
        pos = 0;
        while ((pos = escaped.find(paraSep, pos)) != std::string::npos) {
            escaped.replace(pos, 3, "\\u2029");
            pos += 6;
        }
        
        return escaped;
    }

    // Base64 encoding table
    static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    // Base64 encode a string - this is the safest way to pass data to JavaScript
    static std::string Base64Encode(const std::string& input) {
        std::string encoded;
        encoded.reserve(((input.size() + 2) / 3) * 4);
        
        unsigned int val = 0;
        int valb = -6;
        
        for (unsigned char c : input) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                encoded.push_back(base64_chars[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        
        if (valb > -6) {
            encoded.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
        }
        
        while (encoded.size() % 4) {
            encoded.push_back('=');
        }
        
        return encoded;
    }

    void CreateDebuggerPanel() {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create debugger - Prisma UI not initialized");
            return;
        }

        if (g_debuggerCreated.load()) {
            logger::debug("[PrismaUIBridge] Debugger panel already created");
            return;
        }

        logger::info("[PrismaUIBridge] Creating CHIM debugger panel from CHIM/debugger.html...");

        g_debuggerView = g_prismaUI->CreateView("CHIM/debugger.html", OnDebuggerDomReady);

        if (g_debuggerView == 0) {
            g_lastError = "Failed to create debugger view - check that Data/PrismaUI/views/CHIM/debugger.html exists";
            logger::error("[PrismaUIBridge] {}", g_lastError);
            return;
        }

        logger::info("[PrismaUIBridge] Debugger view created with ID: {}", g_debuggerView);

        g_prismaUI->SetOrder(g_debuggerView, 120);
        g_prismaUI->SetScrollingPixelSize(g_debuggerView, 120);
        g_prismaUI->RegisterJSListener(g_debuggerView, "chimDebuggerCommand", OnDebuggerCommand);

        g_debuggerCreated.store(true);
        logger::info("[PrismaUIBridge] Debugger panel created successfully");
    }

    static void OnDebuggerDomReady(PrismaView view) {
        logger::info("[PrismaUIBridge] Debugger panel DOM ready");
        g_debuggerDomReady.store(true);
        g_lastDebuggerRefresh = std::chrono::steady_clock::now();

        // Send the server URL to JavaScript - it will handle fetching data directly via fetch()
        std::string server = Conf::getInstance().getServer();
        std::string port = Conf::getInstance().getPort();
        std::string serverUrl = "http://" + server + ":" + port + "/HerikaServer";
        std::string jsCall = "window.initDebugger('" + serverUrl + "')";
        g_prismaUI->Invoke(g_debuggerView, jsCall.c_str(), nullptr);
        
        logger::info("[PrismaUIBridge] Debugger initialized with server: {}", serverUrl);
    }

    static void OnDebuggerCommand(const char* argument) {
        if (!argument) return;

        std::string cmd(argument);
        logger::debug("[PrismaUIBridge] Received debugger command: {}", cmd);

        if (cmd == "close") {
            HideDebuggerPanel();
        }
        // Other commands now handled directly by JavaScript fetching from server
    }

    void ToggleDebuggerPanel() {
        const std::string controlPanelUrl = BuildBrowserUrl("control_panel.php");
        logger::info("[PrismaUIBridge] CHIM Logs View hotkey pressed - opening browser to control panel");

        if (!IsAvailable()) {
            logger::warn("[PrismaUIBridge] Prisma UI not available");
            return;
        }

        if (g_browserVisible.load() && g_browserCurrentUrl == controlPanelUrl) {
            logger::info("[PrismaUIBridge] Control panel browser already visible, hiding it");
            HideBrowserPanel();
            return;
        }

        ShowBrowserPanelForUrl(controlPanelUrl, "control panel");
        if (g_browserVisible.load()) {
            logger::info("[PrismaUIBridge] CHIM Logs View opened - press hotkey again to close");
        }
    }

    void ShowDebuggerPanel() {
        if (!g_prismaUI || !g_debuggerCreated.load()) {
            if (!g_debuggerCreated.load()) {
                CreateDebuggerPanel();
            }
            if (!g_debuggerCreated.load()) {
                return;
            }
        }

        if (!g_prismaUI->IsValid(g_debuggerView)) {
            logger::error("[PrismaUIBridge] Debugger view is invalid");
            return;
        }

        logger::info("[PrismaUIBridge] Showing debugger panel");
        g_prismaUI->Show(g_debuggerView);
        g_prismaUI->Focus(g_debuggerView, false, false);
        
        // Fetch fresh data when showing
        FetchAndUpdateDebugger();
    }

    void HideDebuggerPanel() {
        if (!g_prismaUI || !g_debuggerCreated.load()) {
            return;
        }

        logger::info("[PrismaUIBridge] Hiding debugger panel");
        
        if (g_prismaUI->HasFocus(g_debuggerView)) {
            g_prismaUI->Unfocus(g_debuggerView);
        }
        g_prismaUI->Hide(g_debuggerView);
    }

    bool IsDebuggerPanelVisible() {
        if (!g_prismaUI || !g_debuggerCreated.load()) {
            return false;
        }
        return !g_prismaUI->IsHidden(g_debuggerView);
    }

    void CheckAndUpdateDebugger() {
        // JavaScript handles periodic updates via fetch() - no C++ involvement needed
    }

    void FetchAndUpdateDebugger() {
        if (!g_prismaUI || !g_debuggerCreated.load()) {
            logger::warn("[PrismaUIBridge] Cannot fetch debugger - UI not created");
            return;
        }

        if (!g_debuggerDomReady.load()) {
            logger::warn("[PrismaUIBridge] Debugger DOM not ready yet");
            return;
        }

        logger::debug("[PrismaUIBridge] Fetching debugger data from server");

        std::thread([=]() {
            std::string jsonResponse = FetchDebuggerFromServer();

            if (jsonResponse.empty()) {
                logger::warn("[PrismaUIBridge] Received empty response from debugger API");
                if (g_prismaUI && g_debuggerCreated.load()) {
                    g_prismaUI->Invoke(g_debuggerView, "window.showError('No data received from server')", nullptr);
                }
                return;
            }

            logger::debug("[PrismaUIBridge] Received debugger data: {} bytes", jsonResponse.length());

            std::string escapedJson = EscapeJSONForJS(jsonResponse);
            std::string jsCall = "window.updateDebugger('" + escapedJson + "')";
            
            if (g_prismaUI && g_debuggerCreated.load()) {
                g_prismaUI->Invoke(g_debuggerView, jsCall.c_str(), nullptr);
            }
        }).detach();
    }

    static void FetchAndUpdateChimLog() {
        if (!g_prismaUI || !g_debuggerCreated.load() || !g_debuggerDomReady.load()) return;

        std::thread([=]() {
            std::string jsonResponse = FetchLogFromServer("chim", 200);
            if (!jsonResponse.empty() && g_prismaUI && g_debuggerCreated.load()) {
                logger::debug("[PrismaUIBridge] Sending CHIM log data: {} bytes", jsonResponse.length());
                
                // Use base64 to safely pass data to JavaScript
                std::string base64Data = Base64Encode(jsonResponse);
                std::string jsCall = "window.updateChimLogB64('" + base64Data + "')";
                g_prismaUI->Invoke(g_debuggerView, jsCall.c_str(), nullptr);
            } else {
                logger::warn("[PrismaUIBridge] CHIM log: Empty response or invalid state");
            }
        }).detach();
    }

    static void FetchAndUpdateApacheLog() {
        if (!g_prismaUI || !g_debuggerCreated.load() || !g_debuggerDomReady.load()) return;

        std::thread([=]() {
            std::string jsonResponse = FetchLogFromServer("apache", 100);
            if (!jsonResponse.empty() && g_prismaUI && g_debuggerCreated.load()) {
                logger::debug("[PrismaUIBridge] Sending Apache log data: {} bytes", jsonResponse.length());
                
                std::string base64Data = Base64Encode(jsonResponse);
                std::string jsCall = "window.updateApacheLogB64('" + base64Data + "')";
                g_prismaUI->Invoke(g_debuggerView, jsCall.c_str(), nullptr);
            }
        }).detach();
    }

    static void FetchAndUpdateLLMLogs() {
        if (!g_prismaUI || !g_debuggerCreated.load() || !g_debuggerDomReady.load()) return;

        // Fetch context log - request more lines for LLM logs
        std::thread([=]() {
            std::string jsonResponse = FetchLogFromServer("context", 500);
            if (!jsonResponse.empty() && g_prismaUI && g_debuggerCreated.load()) {
                logger::debug("[PrismaUIBridge] Sending Context log data: {} bytes", jsonResponse.length());
                
                std::string base64Data = Base64Encode(jsonResponse);
                std::string jsCall = "window.updateContextLogB64('" + base64Data + "')";
                g_prismaUI->Invoke(g_debuggerView, jsCall.c_str(), nullptr);
            }
        }).detach();

        // Fetch output log - request more lines for LLM logs
        std::thread([=]() {
            std::string jsonResponse = FetchLogFromServer("output", 500);
            if (!jsonResponse.empty() && g_prismaUI && g_debuggerCreated.load()) {
                logger::debug("[PrismaUIBridge] Sending Output log data: {} bytes", jsonResponse.length());
                
                std::string base64Data = Base64Encode(jsonResponse);
                std::string jsCall = "window.updateOutputLogB64('" + base64Data + "')";
                g_prismaUI->Invoke(g_debuggerView, jsCall.c_str(), nullptr);
            }
        }).detach();
    }

    static std::string FetchDebuggerFromServer() {
        constexpr size_t BUFFER_SIZE = 8192;
        constexpr int TIMEOUT_SECONDS = 10;

        WSADATA wsaData;
        int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (iResult != 0) {
            logger::error("[PrismaUIBridge] WSAStartup failed: {}", iResult);
            return "";
        }

        SOCKET rawSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (rawSocket == INVALID_SOCKET) {
            logger::error("[PrismaUIBridge] Failed to create socket: {}", WSAGetLastError());
            WSACleanup();
            return "";
        }

        DWORD timeout = TIMEOUT_SECONDS * 1000;
        setsockopt(rawSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(rawSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        struct addrinfo hints;
        struct addrinfo* result = nullptr;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        std::string server = Conf::getInstance().getServer();
        std::string port = Conf::getInstance().getPort();

        int adHres = getaddrinfo(server.c_str(), port.c_str(), &hints, &result);
        if (adHres != 0) {
            logger::error("[PrismaUIBridge] getaddrinfo failed: {}", adHres);
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        bool connected = false;
        for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
            iResult = connect(rawSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
            if (iResult != SOCKET_ERROR) {
                connected = true;
                break;
            }
        }

        freeaddrinfo(result);

        if (!connected) {
            logger::error("[PrismaUIBridge] Could not connect to server");
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        std::string configPath = Conf::getInstance().getPath();
        std::string basePath;
        size_t lastSlash = configPath.find_last_of("/");
        if (lastSlash != std::string::npos) {
            basePath = configPath.substr(0, lastSlash + 1);
        }

        std::string requestPath = basePath + "ui/api/chim_debugger.php?limit=20";
        
        if (!requestPath.empty() && requestPath[0] == '/') {
            requestPath = requestPath.substr(1);
        }

        logger::info("[PrismaUIBridge] Fetching debugger from: http://{}:{}/{}", server, port, requestPath);

        std::string httpRequest = "GET /" + requestPath + " HTTP/1.1\r\n";
        httpRequest += "Host: " + server + "\r\n";
        httpRequest += "Connection: close\r\n";
        httpRequest += "Accept: application/json\r\n";
        httpRequest += "\r\n";

        iResult = send(rawSocket, httpRequest.c_str(), (int)httpRequest.size(), 0);
        if (iResult == SOCKET_ERROR) {
            logger::error("[PrismaUIBridge] Failed to send request: {}", WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        std::string response;
        char buffer[BUFFER_SIZE];

        while (true) {
            iResult = recv(rawSocket, buffer, BUFFER_SIZE - 1, 0);
            if (iResult > 0) {
                buffer[iResult] = '\0';
                response += buffer;
            } else if (iResult == 0) {
                break;
            } else {
                int error = WSAGetLastError();
                if (error != WSAETIMEDOUT) {
                    logger::error("[PrismaUIBridge] recv failed: {}", error);
                }
                break;
            }
        }

        closesocket(rawSocket);
        WSACleanup();

        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            response = response.substr(headerEnd + 4);
        }

        return response;
    }

    static std::string FetchLogFromServer(const std::string& logType, int numLines) {
        constexpr size_t BUFFER_SIZE = 16384;
        constexpr int TIMEOUT_SECONDS = 10;

        WSADATA wsaData;
        int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (iResult != 0) {
            return "";
        }

        SOCKET rawSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (rawSocket == INVALID_SOCKET) {
            WSACleanup();
            return "";
        }

        DWORD timeout = TIMEOUT_SECONDS * 1000;
        setsockopt(rawSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(rawSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        struct addrinfo hints;
        struct addrinfo* result = nullptr;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        std::string server = Conf::getInstance().getServer();
        std::string port = Conf::getInstance().getPort();

        if (getaddrinfo(server.c_str(), port.c_str(), &hints, &result) != 0) {
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        bool connected = false;
        for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
            if (connect(rawSocket, ptr->ai_addr, (int)ptr->ai_addrlen) != SOCKET_ERROR) {
                connected = true;
                break;
            }
        }

        freeaddrinfo(result);

        if (!connected) {
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        std::string configPath = Conf::getInstance().getPath();
        std::string basePath;
        size_t lastSlash = configPath.find_last_of("/");
        if (lastSlash != std::string::npos) {
            basePath = configPath.substr(0, lastSlash + 1);
        }

        std::string requestPath = basePath + "ui/api/chim_debugger_logs.php?type=" + logType + "&lines=" + std::to_string(numLines);
        
        if (!requestPath.empty() && requestPath[0] == '/') {
            requestPath = requestPath.substr(1);
        }

        std::string httpRequest = "GET /" + requestPath + " HTTP/1.1\r\n";
        httpRequest += "Host: " + server + "\r\n";
        httpRequest += "Connection: close\r\n";
        httpRequest += "Accept: application/json\r\n";
        httpRequest += "\r\n";

        if (send(rawSocket, httpRequest.c_str(), (int)httpRequest.size(), 0) == SOCKET_ERROR) {
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        std::string response;
        char buffer[BUFFER_SIZE];

        while (true) {
            iResult = recv(rawSocket, buffer, BUFFER_SIZE - 1, 0);
            if (iResult > 0) {
                buffer[iResult] = '\0';
                response += buffer;
            } else if (iResult == 0) {
                break;
            } else {
                break;
            }
        }

        closesocket(rawSocket);
        WSACleanup();

        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            response = response.substr(headerEnd + 4);
        }

        return response;
    }

    // ===== Common Functions =====

    void Shutdown() {
        logger::info("[PrismaUIBridge] Shutting down...");
        SetChatboxGameplayInputSuppressed(false);

        if (g_prismaUI) {
            if (g_panelCreated.load()) {
                g_prismaUI->Destroy(g_historyView);
                g_historyView = 0;
                g_panelCreated.store(false);
            }
            
            if (g_overlayCreated.load()) {
                g_prismaUI->Destroy(g_overlayView);
                g_overlayView = 0;
                g_overlayCreated.store(false);
            }
            
            if (g_diariesCreated.load()) {
                g_prismaUI->Destroy(g_diariesView);
                g_diariesView = 0;
                g_diariesCreated.store(false);
            }
            
            if (g_browserCreated.load()) {
                g_prismaUI->Destroy(g_browserView);
                g_browserView = 0;
                g_browserCreated.store(false);
                g_browserDomReady.store(false);
                g_browserVisible.store(false);
                g_browserCurrentLabel = "browser";
                g_browserCurrentUrl.clear();
            }

            if (g_questManagerCreated.load()) {
                g_prismaUI->Destroy(g_questManagerView);
                g_questManagerView = 0;
                g_questManagerCreated.store(false);
                g_questManagerVisible.store(false);
                g_questManagerDomReady.store(false);
            }
             
            if (g_aiviewCreated.load()) {
                g_prismaUI->Destroy(g_aiviewView);
                g_aiviewView = 0;
                g_aiviewCreated.store(false);
            }
            
            if (g_debuggerCreated.load()) {
                g_prismaUI->Destroy(g_debuggerView);
                g_debuggerView = 0;
                g_debuggerCreated.store(false);
            }

            if (g_confirmationCreated.load()) {
                g_prismaUI->Destroy(g_confirmationView);
                g_confirmationView = 0;
                g_confirmationCreated.store(false);
                g_confirmationDomReady.store(false);
                g_confirmationVisible.store(false);
                std::lock_guard<std::mutex> lock(g_confirmationMutex);
                g_confirmationCallback = nullptr;
                g_confirmationPayload.clear();
            }
        }

        g_prismaUI = nullptr;
        g_prismaUI2 = nullptr;
    }

    std::string GetLastError() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_lastError;
    }

    // HTTP fetch helper - similar to HTTPManager but simpler for GET requests
    static std::string FetchEventlogFromServer(int limit, int sinceRowId) {
        constexpr size_t BUFFER_SIZE = 4096;
        constexpr int TIMEOUT_SECONDS = 10;

        WSADATA wsaData;
        int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (iResult != 0) {
            logger::error("[PrismaUIBridge] WSAStartup failed: {}", iResult);
            return "";
        }

        SOCKET rawSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (rawSocket == INVALID_SOCKET) {
            logger::error("[PrismaUIBridge] Failed to create socket: {}", WSAGetLastError());
            WSACleanup();
            return "";
        }

        // Set socket timeouts
        DWORD timeout = TIMEOUT_SECONDS * 1000;
        setsockopt(rawSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(rawSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        struct addrinfo hints;
        struct addrinfo* result = nullptr;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        std::string server = Conf::getInstance().getServer();
        std::string port = Conf::getInstance().getPort();

        int adHres = getaddrinfo(server.c_str(), port.c_str(), &hints, &result);
        if (adHres != 0) {
            logger::error("[PrismaUIBridge] getaddrinfo failed: {}", adHres);
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        // Connect
        bool connected = false;
        for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
            iResult = connect(rawSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
            if (iResult != SOCKET_ERROR) {
                connected = true;
                break;
            }
        }

        freeaddrinfo(result);

        if (!connected) {
            logger::error("[PrismaUIBridge] Could not connect to server");
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        // Build the request path
        std::string configPath = Conf::getInstance().getPath();
        std::string basePath;
        size_t lastSlash = configPath.find_last_of("/");
        if (lastSlash != std::string::npos) {
            basePath = configPath.substr(0, lastSlash + 1);
        }

        // Ensure basePath starts with / but we don't double it
        // Use format=raw to get clean data without HTML tags (for in-game UI)
        // Don't filter by event_types - show all events like the eventlog page does
        // The default server filter excludes only internal events (addnpc, init, etc)
        std::string requestPath = basePath + "ui/api/eventlog.php?limit=" + std::to_string(limit) + "&format=raw";
        if (sinceRowId > 0) {
            requestPath += "&since_rowid=" + std::to_string(sinceRowId);
        }

        // Remove leading slash if present (we add it in the GET line)
        if (!requestPath.empty() && requestPath[0] == '/') {
            requestPath = requestPath.substr(1);
        }

        logger::info("[PrismaUIBridge] Fetching from: http://{}:{}/{}", server, port, requestPath);

        // Build HTTP request
        std::string httpRequest = "GET /" + requestPath + " HTTP/1.1\r\n";
        httpRequest += "Host: " + server + "\r\n";
        httpRequest += "Connection: close\r\n";
        httpRequest += "Accept: application/json\r\n";
        httpRequest += "\r\n";

        // Send request
        iResult = send(rawSocket, httpRequest.c_str(), (int)httpRequest.size(), 0);
        if (iResult == SOCKET_ERROR) {
            logger::error("[PrismaUIBridge] Failed to send request: {}", WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        // Receive response
        std::string response;
        char buffer[BUFFER_SIZE];

        while (true) {
            iResult = recv(rawSocket, buffer, BUFFER_SIZE - 1, 0);
            if (iResult > 0) {
                buffer[iResult] = '\0';
                response += buffer;
            } else if (iResult == 0) {
                break; // Connection closed
            } else {
                int error = WSAGetLastError();
                if (error != WSAETIMEDOUT) {
                    logger::error("[PrismaUIBridge] recv failed: {}", error);
                }
                break;
            }
        }

        closesocket(rawSocket);
        WSACleanup();

        // Extract body from HTTP response
        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            response = response.substr(headerEnd + 4);
        }

        return response;
    }

    // ===== CHIM Status HUD Panel Functions =====

    static PrismaView g_statusHUDView = 0;
    static std::atomic<bool> g_statusHUDCreated{false};
    static std::atomic<bool> g_statusHUDDomReady{false};
    
    // Status HUD target state
    static std::string g_lastStatusHUDTarget = "";
    static std::string g_lastStatusHUDTargetStatus = "";
    static uint32_t g_lastStatusHUDFormId = 0;
    static std::chrono::steady_clock::time_point g_lastStatusHUDTargetCheck;

    static void OnStatusHUDDomReady(PrismaView view);
    static void UpdateStatusHUDTarget(const std::string& name, float distance, const std::string& status = "",
                                      bool targetable = true);

    void CreateStatusHUDPanel() {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create status HUD - Prisma UI not initialized");
            return;
        }

        if (g_statusHUDCreated.load()) {
            logger::debug("[PrismaUIBridge] Status HUD panel already created");
            return;
        }

        logger::info("[PrismaUIBridge] Creating CHIM status HUD panel from CHIM/status_hud.html...");

        g_statusHUDView = g_prismaUI->CreateView("CHIM/status_hud.html", OnStatusHUDDomReady);

        if (g_statusHUDView == 0) {
            g_lastError = "Failed to create status HUD view - check that Data/PrismaUI/views/CHIM/status_hud.html exists";
            logger::error("[PrismaUIBridge] {}", g_lastError);
            return;
        }

        logger::info("[PrismaUIBridge] Status HUD view created with ID: {}", g_statusHUDView);

        // High z-order so it stays on top, no scrolling needed for a HUD
        g_prismaUI->SetOrder(g_statusHUDView, 200);

        g_statusHUDCreated.store(true);
        logger::info("[PrismaUIBridge] Status HUD panel created successfully");
    }

    static void OnStatusHUDDomReady(PrismaView view) {
        logger::info("[PrismaUIBridge] Status HUD panel DOM ready");
        g_statusHUDDomReady.store(true);

        // Send the server URL to JavaScript - it will handle fetching data directly via fetch()
        std::string server = Conf::getInstance().getServer();
        std::string port = Conf::getInstance().getPort();
        std::string serverUrl = "http://" + server + ":" + port + "/HerikaServer";
        std::string jsCall = "window.initStatusHUD('" + serverUrl + "')";
        g_prismaUI->Invoke(g_statusHUDView, jsCall.c_str(), nullptr);
        if (g_prismaUI && g_statusHUDCreated.load() && !g_prismaUI->IsHidden(g_statusHUDView)) {
            g_prismaUI->Invoke(g_statusHUDView, "window.onStatusHUDShown && window.onStatusHUDShown()", nullptr);
        }
        
        logger::info("[PrismaUIBridge] Status HUD initialized with server: {}", serverUrl);
    }

    void ToggleStatusHUDPanel() {
        logger::info("[PrismaUIBridge] Toggle status HUD panel (created={}, view={}, domReady={})",
            g_statusHUDCreated.load(), g_statusHUDView, g_statusHUDDomReady.load());

        if (!IsAvailable()) {
            logger::warn("[PrismaUIBridge] Prisma UI not available");
            return;
        }

        if (!g_statusHUDCreated.load() || g_statusHUDView == 0 || !g_prismaUI->IsValid(g_statusHUDView)) {
            CreateStatusHUDPanel();
            if (!g_statusHUDCreated.load() || g_statusHUDView == 0) {
                logger::error("[PrismaUIBridge] Failed to create status HUD panel");
                return;
            }
        }

        if (!g_statusHUDDomReady.load()) {
            logger::info("[PrismaUIBridge] Status HUD DOM not ready yet, showing panel");
            g_prismaUI->Show(g_statusHUDView);
            return;
        }

        if (g_prismaUI->IsHidden(g_statusHUDView)) {
            ShowStatusHUDPanel();
        } else {
            HideStatusHUDPanel();
        }
    }

    void ShowStatusHUDPanel() {
        if (!g_prismaUI || !g_statusHUDCreated.load()) {
            if (!g_statusHUDCreated.load()) {
                CreateStatusHUDPanel();
            }
            if (!g_statusHUDCreated.load()) {
                return;
            }
        }

        if (!g_prismaUI->IsValid(g_statusHUDView)) {
            logger::error("[PrismaUIBridge] Status HUD view is invalid");
            return;
        }

        logger::info("[PrismaUIBridge] Showing status HUD panel");
        g_prismaUI->Show(g_statusHUDView);
        if (g_statusHUDDomReady.load()) {
            g_prismaUI->Invoke(g_statusHUDView, "window.onStatusHUDShown && window.onStatusHUDShown()", nullptr);
        }
        // No focus for HUD - it should be non-interactive
    }

    void HideStatusHUDPanel() {
        if (!g_prismaUI || !g_statusHUDCreated.load()) {
            return;
        }

        logger::info("[PrismaUIBridge] Hiding status HUD panel");
        if (g_statusHUDDomReady.load()) {
            g_prismaUI->Invoke(g_statusHUDView, "window.onStatusHUDHidden && window.onStatusHUDHidden()", nullptr);
        }
        g_prismaUI->Hide(g_statusHUDView);
    }

    bool IsStatusHUDPanelVisible() {
        if (!g_prismaUI || !g_statusHUDCreated.load()) {
            return false;
        }
        return !g_prismaUI->IsHidden(g_statusHUDView);
    }

    void CheckAndUpdateStatusHUDTarget() {
        // Poll UI target state quickly; SpatialSnapshotManager owns the heavier cache/TTL.
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastStatusHUDTargetCheck).count() < 250) {
            return;
        }
        g_lastStatusHUDTargetCheck = now;
        
        // Only update if Status HUD is visible
        if (!g_statusHUDCreated.load() || !g_prismaUI || g_prismaUI->IsHidden(g_statusHUDView)) {
            return;
        }
        
        const auto target = GetPrimaryPrismaTarget("prismaui_status_hud");

        if (target.hasTarget) {
            if (target.name != g_lastStatusHUDTarget || target.formId != g_lastStatusHUDFormId ||
                target.status != g_lastStatusHUDTargetStatus) {
                UpdateStatusHUDTarget(target.name, target.distanceMeters, target.status, target.targetable);
                g_lastStatusHUDTarget = target.name;
                g_lastStatusHUDFormId = target.formId;
                g_lastStatusHUDTargetStatus = target.status;
            }
        } else {
            if (!g_lastStatusHUDTarget.empty() || target.status != g_lastStatusHUDTargetStatus) {
                UpdateStatusHUDTarget("", 0.0f, target.status, false);
                g_lastStatusHUDTarget.clear();
                g_lastStatusHUDFormId = 0;
                g_lastStatusHUDTargetStatus = target.status;
            }
        }
    }

    static void UpdateStatusHUDTarget(const std::string& name, float distance, const std::string& status,
                                      bool targetable) {
        if (!g_prismaUI || !g_statusHUDCreated.load() || !g_statusHUDDomReady.load()) {
            return;
        }
        
        std::string jsCall;
        if (name.empty()) {
            jsCall = "window.updateStatusHUDTarget('', 0, '" + EscapePrismaJSArg(status) + "', false)";
        } else {
            jsCall = "window.updateStatusHUDTarget('" + EscapePrismaJSArg(name) + "', " +
                std::to_string(distance) + ", '" + EscapePrismaJSArg(status) + "', " +
                (targetable ? "true" : "false") + ")";
        }
        
        g_prismaUI->Invoke(g_statusHUDView, jsCall.c_str(), nullptr);
    }

    // ===== CHIM Chatbox Panel Functions =====

    static PrismaView g_chatboxView = 0;
    static std::atomic<bool> g_chatboxCreated{false};
    static std::atomic<bool> g_chatboxDomReady{false};
    static std::atomic<int> g_chatboxState{0}; // 0=hidden, 1=visible
    static std::atomic<bool> g_chatboxQuickFocusActive{false};

    static void OnChatboxDomReady(PrismaView view);
    static void OnChatboxCommand(const char* argument);

    static void CreateConfirmationPanel() {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create confirmation modal - Prisma UI not initialized");
            return;
        }

        if (g_confirmationCreated.load()) {
            return;
        }

        logger::info("[PrismaUIBridge] Creating CHIM confirmation modal from CHIM/confirmation.html...");

        g_confirmationView = g_prismaUI->CreateView("CHIM/confirmation.html", OnConfirmationDomReady);
        if (g_confirmationView == 0) {
            g_lastError = "Failed to create confirmation view - check that Data/PrismaUI/views/CHIM/confirmation.html exists";
            logger::error("[PrismaUIBridge] {}", g_lastError);
            return;
        }

        g_prismaUI->SetOrder(g_confirmationView, 260);
        g_prismaUI->RegisterJSListener(g_confirmationView, "chimConfirmationCommand", OnConfirmationCommand);
        g_prismaUI->Hide(g_confirmationView);
        g_confirmationCreated.store(true);

        logger::info("[PrismaUIBridge] Confirmation modal created successfully");
    }

    static void UnfocusPrismaViewIfFocused(PrismaView view, bool created) {
        if (!g_prismaUI || !created || view == 0 || !g_prismaUI->IsValid(view)) {
            return;
        }

        if (g_prismaUI->HasFocus(view)) {
            g_prismaUI->Unfocus(view);
        }
    }

    static bool PresentConfirmationPayload() {
        if (!g_prismaUI || !g_confirmationCreated.load() || !g_confirmationDomReady.load()) {
            return false;
        }

        if (!g_prismaUI->IsValid(g_confirmationView)) {
            logger::warn("[PrismaUIBridge] Confirmation modal view is invalid");
            return false;
        }

        std::string payload;
        {
            std::lock_guard<std::mutex> lock(g_confirmationMutex);
            payload = g_confirmationPayload;
        }

        if (payload.empty()) {
            return false;
        }

        UnfocusPrismaViewIfFocused(g_historyView, g_panelCreated.load());
        UnfocusPrismaViewIfFocused(g_overlayView, g_overlayCreated.load());
        UnfocusPrismaViewIfFocused(g_diariesView, g_diariesCreated.load());
        UnfocusPrismaViewIfFocused(g_browserView, g_browserCreated.load());
        UnfocusPrismaViewIfFocused(g_questManagerView, g_questManagerCreated.load());
        UnfocusPrismaViewIfFocused(g_aiviewView, g_aiviewCreated.load());
        UnfocusPrismaViewIfFocused(g_debuggerView, g_debuggerCreated.load());
        UnfocusPrismaViewIfFocused(g_statusHUDView, g_statusHUDCreated.load());
        UnfocusPrismaViewIfFocused(g_settingsMenuView, g_settingsMenuCreated.load());
        UnfocusPrismaViewIfFocused(g_masterMenuView, g_masterMenuCreated.load());
        if (g_chatboxCreated.load() && g_prismaUI->HasFocus(g_chatboxView)) {
            UnfocusChatboxPanel();
        }

        g_prismaUI->Show(g_confirmationView);
        g_confirmationVisible.store(true);

        std::string jsCall = "window.showChimConfirmation('" + EscapeForJS(payload) + "')";
        g_prismaUI->Invoke(g_confirmationView, jsCall.c_str(), nullptr);

        bool focused = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            focused = g_prismaUI->Focus(g_confirmationView, true, false);
            if (focused) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (!focused) {
            logger::warn("[PrismaUIBridge] Failed to focus confirmation modal; leaving it visible because focus can fail on some Prisma builds");
        }

        logger::info("[PrismaUIBridge] Confirmation modal shown{}", focused ? "" : " without Prisma focus");
        return true;
    }

    static bool HasPendingConfirmationPayload() {
        std::lock_guard<std::mutex> lock(g_confirmationMutex);
        return !g_confirmationPayload.empty();
    }

    static void OnConfirmationDomReady(PrismaView view) {
        logger::info("[PrismaUIBridge] Confirmation modal DOM ready");
        g_confirmationDomReady.store(true);

        if (HasPendingConfirmationPayload() && !g_confirmationVisible.load()) {
            PresentConfirmationPayload();
        }
    }

    static void ResolveConfirmation(bool accepted) {
        ConfirmationCallback callback;
        {
            std::lock_guard<std::mutex> lock(g_confirmationMutex);
            callback = std::move(g_confirmationCallback);
            g_confirmationCallback = nullptr;
            g_confirmationPayload.clear();
        }

        if (g_prismaUI && g_confirmationCreated.load() && g_prismaUI->IsValid(g_confirmationView)) {
            if (g_prismaUI->HasFocus(g_confirmationView)) {
                g_prismaUI->Unfocus(g_confirmationView);
            }
            g_prismaUI->Hide(g_confirmationView);
        }
        g_confirmationVisible.store(false);

        if (callback) {
            callback(accepted);
        }
    }

    static void OnConfirmationCommand(const char* argument) {
        if (!argument) {
            return;
        }

        std::string cmd(argument);
        logger::debug("[PrismaUIBridge] Received confirmation command: {}", cmd);

        if (cmd == "accept") {
            ResolveConfirmation(true);
        } else if (cmd == "cancel" || cmd == "close" || cmd == "escape") {
            ResolveConfirmation(false);
        } else if (cmd == "dom_ready") {
            g_confirmationDomReady.store(true);
            if (HasPendingConfirmationPayload() && !g_confirmationVisible.load()) {
                PresentConfirmationPayload();
            }
        } else {
            logger::warn("[PrismaUIBridge] Unknown confirmation command: {}", cmd);
        }
    }

    bool ShowConfirmation(const std::string& title, const std::string& message,
                          const std::string& cancelLabel, const std::string& acceptLabel,
                          ConfirmationCallback callback) {
        if (!g_prismaUI) {
            logger::warn("[PrismaUIBridge] Cannot show confirmation - Prisma UI not initialized");
            return false;
        }

        if (!callback) {
            logger::warn("[PrismaUIBridge] Cannot show confirmation - callback missing");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_confirmationMutex);
            if (g_confirmationCallback) {
                logger::warn("[PrismaUIBridge] Replacing active confirmation; dropping previous request");
                g_confirmationCallback = nullptr;
                g_confirmationPayload.clear();
            }
        }

        if (!g_confirmationCreated.load()) {
            CreateConfirmationPanel();
        }
        if (!g_confirmationCreated.load()) {
            return false;
        }

        json payload = {
            {"id", ++g_confirmationRequestId},
            {"title", title},
            {"message", message},
            {"cancelLabel", cancelLabel},
            {"acceptLabel", acceptLabel}
        };

        {
            std::lock_guard<std::mutex> lock(g_confirmationMutex);
            g_confirmationCallback = std::move(callback);
            g_confirmationPayload = payload.dump();
        }

        if (!g_confirmationDomReady.load()) {
            if (g_prismaUI->IsValid(g_confirmationView)) {
                g_prismaUI->Show(g_confirmationView);
            }
            logger::info("[PrismaUIBridge] Confirmation modal queued while waiting for DOM ready");
            return true;
        }

        if (g_confirmationVisible.load()) {
            return true;
        }

        if (!PresentConfirmationPayload()) {
            std::lock_guard<std::mutex> lock(g_confirmationMutex);
            g_confirmationCallback = nullptr;
            g_confirmationPayload.clear();
            return false;
        }

        return true;
    }

    struct ChatboxNearbyAgent {
        RE::Actor* actor;
        std::string name;
        float distanceMeters;
        uint32_t formId;
        bool isNarrator;
        std::string status;
        int sortBucket = 0;
        bool targetable = true;
        bool autoEligible = true;
    };

    static bool IsChatboxSpawnMode()
    {
        return g_chatboxCurrentMode == "SPAWN";
    }

    static bool IsChatboxNarratorOnlyMode()
    {
        return g_chatboxCurrentMode == "NARRATOR";
    }

    static bool IsChatboxDirectorMode()
    {
        return g_chatboxCurrentMode == "DIRECTOR";
    }

    static std::shared_ptr<AIAgent> FindChatboxAgentByFormIdOrName(uint32_t formId, const std::string& name)
    {
        AIAgentManager& aiam = AIAgentManager::getInstance();

        if (formId != 0) {
            for (const auto& agent : aiam.getAgents()) {
                auto* actor = agent ? agent->getActor() : nullptr;
                if (actor && actor->GetFormID() == formId) {
                    return std::const_pointer_cast<AIAgent>(agent);
                }
            }
        }

        if (!name.empty()) {
            auto agent = aiam.getAgentByName(name);
            if (agent) {
                return agent;
            }
        }

        return nullptr;
    }

    static void SetChatboxTargetOverride(uint32_t formId, const std::string& name)
    {
        g_chatboxTargetMode = ChatboxTargetMode::NPC;
        g_chatboxTargetOverrideFormId = formId;
        g_chatboxTargetOverrideName = name;
    }

    static void SetChatboxEveryoneTargetOverride()
    {
        g_chatboxTargetMode = ChatboxTargetMode::Everyone;
        g_chatboxTargetOverrideFormId = 0;
        g_chatboxTargetOverrideName.clear();
    }

    void ClearChatboxTargetOverride()
    {
        g_chatboxTargetMode = ChatboxTargetMode::Auto;
        g_chatboxTargetOverrideFormId = 0;
        g_chatboxTargetOverrideName.clear();
    }

    bool GetChatboxTargetOverride(uint32_t& formId, std::string& name)
    {
        if (g_chatboxTargetMode != ChatboxTargetMode::NPC) {
            formId = 0;
            name.clear();
            return false;
        }

        formId = g_chatboxTargetOverrideFormId;
        name = g_chatboxTargetOverrideName;
        return formId != 0 || !name.empty();
    }

    ChatboxTargetMode GetChatboxTargetMode()
    {
        return g_chatboxTargetMode;
    }

    bool IsChatboxEveryoneTargetOverrideActive()
    {
        return g_chatboxTargetMode == ChatboxTargetMode::Everyone;
    }

    static std::vector<ChatboxNearbyAgent> CollectChatboxNearbyAgents() {
        std::vector<ChatboxNearbyAgent> nearbyAgents;
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nearbyAgents;
        }

        AIAgentManager& aiam = AIAgentManager::getInstance();
        auto* playerCell = player->GetParentCell();
        if (!playerCell) {
            return nearbyAgents;
        }

        if (IsChatboxSpawnMode() || IsChatboxDirectorMode()) {
            return nearbyAgents;
        }

        if (IsChatboxNarratorOnlyMode()) {
            auto narrator = aiam.getAgentByName(NARRATOR_NAME);
            if (narrator) {
                ChatboxNearbyAgent narratorTarget{};
                narratorTarget.actor = narrator->getActor();
                narratorTarget.name = narrator->getActorName().empty() ? NARRATOR_NAME : narrator->getActorName();
                narratorTarget.formId = narratorTarget.actor ? narratorTarget.actor->GetFormID() : 0;
                narratorTarget.distanceMeters = 0.0f;
                narratorTarget.isNarrator = true;
                narratorTarget.status = "Narrator";
                narratorTarget.sortBucket = 0;
                narratorTarget.targetable = true;
                narratorTarget.autoEligible = true;
                nearbyAgents.push_back(narratorTarget);
            }
            return nearbyAgents;
        }

        const bool whisperTargetCapActive = g_chatboxCurrentMode == "WHISPER";
        const auto candidates = SpatialSnapshotManager::GetValidPlayerSpeechTargets(
            "chatbox_targets", true, PlayerSpeechTargetMode::Manual);
        for (const auto& candidate : candidates) {
            if (whisperTargetCapActive &&
                (!std::isfinite(candidate.distanceMeters) ||
                 candidate.distanceMeters > kChatboxWhisperTargetMaxMeters)) {
                continue;
            }

            ChatboxNearbyAgent nearby{};
            nearby.actor = candidate.actor;
            nearby.name = candidate.name;
            nearby.formId = candidate.formId;
            nearby.distanceMeters = candidate.distanceMeters;
            nearby.isNarrator = candidate.narrator;
            nearby.status = GetPrismaDisplayStatus(candidate);
            nearby.sortBucket = candidate.sortBucket;
            nearby.targetable = candidate.targetable;
            nearby.autoEligible = candidate.autoEligible;

            nearbyAgents.push_back(nearby);
        }

        std::sort(nearbyAgents.begin(), nearbyAgents.end(),
            [](const ChatboxNearbyAgent& lhs, const ChatboxNearbyAgent& rhs) {
                if (lhs.sortBucket != rhs.sortBucket) {
                    return lhs.sortBucket < rhs.sortBucket;
                }
                if (std::abs(lhs.distanceMeters - rhs.distanceMeters) > 0.001f) {
                    return lhs.distanceMeters < rhs.distanceMeters;
                }
                return lhs.name < rhs.name;
            });

        return nearbyAgents;
    }

    static void UpdateChatboxTargetUI(const std::string& name, float distance) {
        if (!g_prismaUI || !g_chatboxCreated.load() || !g_chatboxDomReady.load()) {
            return;
        }

        std::string jsCall;
        if (name.empty()) {
            jsCall = "window.updateChatboxTarget('', 0)";
        } else {
            jsCall = "window.updateChatboxTarget('" + EscapeForJS(name) + "', " + std::to_string(distance) + ")";
        }

        g_prismaUI->Invoke(g_chatboxView, jsCall.c_str(), nullptr);
    }

    static void UpdateChatboxTargetsUI(const std::string& payload) {
        if (!g_prismaUI || !g_chatboxCreated.load() || !g_chatboxDomReady.load()) {
            return;
        }

        std::string jsCall = "window.updateChatboxTargets('" + EscapeForJS(payload) + "')";
        g_prismaUI->Invoke(g_chatboxView, jsCall.c_str(), nullptr);
    }

    static void UpdateChatboxModeUI(const std::string& mode) {
        if (!g_prismaUI || !g_chatboxCreated.load() || !g_chatboxDomReady.load()) {
            return;
        }

        std::string modeUpper = mode.empty() ? "STANDARD" : mode;
        std::string jsCall = "window.updateChatboxMode('" + EscapeForJS(modeUpper) + "')";
        g_prismaUI->Invoke(g_chatboxView, jsCall.c_str(), nullptr);
    }

    static void UpdateChatboxModelUI(const std::string& modelLabel) {
        if (!g_prismaUI || !g_chatboxCreated.load() || !g_chatboxDomReady.load()) {
            return;
        }

        std::string label = modelLabel.empty() ? "Standard" : modelLabel;
        std::string jsCall = "window.updateChatboxModel('" + EscapeForJS(label) + "')";
        g_prismaUI->Invoke(g_chatboxView, jsCall.c_str(), nullptr);
    }

    static void UpdateChatboxFocusUI(bool focused) {
        if (!g_prismaUI || !g_chatboxCreated.load() || !g_chatboxDomReady.load()) {
            return;
        }

        std::string jsCall = std::string("window.updateChatboxFocus(") + (focused ? "true" : "false") + ")";
        g_prismaUI->Invoke(g_chatboxView, jsCall.c_str(), nullptr);
    }

    static void UpdateChatboxRechatModeUI(const std::string& mode) {
        if (!g_prismaUI || !g_chatboxCreated.load() || !g_chatboxDomReady.load()) {
            return;
        }

        const std::string normalizedMode = mode.empty() ? "random" : mode;
        const std::string jsCall = "window.updateChatboxRechatMode('" + EscapeForJS(normalizedMode) + "')";
        g_prismaUI->Invoke(g_chatboxView, jsCall.c_str(), nullptr);
    }

    static bool ApplyLLMProfileSelection(const std::string& actionId, const char* sourceTag, bool showNotification) {
        std::string profileNum;
        std::string label;

        if (actionId == "llm_standard") {
            profileNum = "1";
            label = "Standard";
        } else if (actionId == "llm_fast") {
            profileNum = "2";
            label = "Fast";
        } else if (actionId == "llm_powerful") {
            profileNum = "3";
            label = "Powerful";
        } else if (actionId == "llm_experimental") {
            profileNum = "4";
            label = "Experimental";
        } else {
            return false;
        }

        HTTPManager::log(std::format("setconf|{}|{}|chim_profile_model@{}",
            getCurrentTimeMillis(), GetGameTimeStamp(), profileNum));
        logger::info("[{}] Switched to LLM profile: {}", sourceTag, profileNum);
        if (showNotification) {
            RE::DebugNotification(("[CHIM] LLM profile: " + label).c_str());
        }

        g_chatboxCurrentModelLabel = label;
        g_lastChatboxModelLabel = label;
        g_chatboxModelInitialized = true;
        UpdateChatboxModelUI(label);
        return true;
    }

    static std::string BuildContinueConversationRequest(const char* eventType)
    {
        return std::format("{}|{}|{}|", eventType, getCurrentTimeMillis(), GetGameTimeStamp());
    }

    static bool TriggerContinueConversationForNpc(const std::string& npcName, const char* sourceTag, bool showMissingTargetNotification) {
        if (npcName.empty()) {
            logger::warn("[{}] continue_chat requires a target", sourceTag);
            if (showMissingTargetNotification) {
                RE::DebugNotification("[CHIM] No target available for Continue Speaking.");
            }
            return false;
        }

        std::string request = BuildContinueConversationRequest("continue");

        AIAgentManager& aiam = AIAgentManager::getInstance();
        auto agentPtr = aiam.getAgentByName(npcName);
        if (agentPtr && agentPtr->getActor()) {
            HTTPManager::stream(request, agentPtr->getActor());
            logger::info("[{}] Triggered continue_chat for {}", sourceTag, npcName);
        } else {
            logger::warn("[{}] continue_chat target '{}' not found, falling back to auto-select", sourceTag, npcName);
            HTTPManager::stream(request);
        }

        return true;
    }

    static bool TriggerContinueConversationForEveryone(const char* sourceTag, bool showNotification)
    {
        std::string request = BuildContinueConversationRequest("continue_group");

        HTTPManager::stream(request);
        logger::info("[{}] Triggered continue_chat for Everyone", sourceTag);
        if (showNotification) {
            RE::DebugNotification("[CHIM] Continuing conversation with everyone.");
        }
        return true;
    }

    static void SyncChatboxStatusFromServerAsync() {
        bool expected = false;
        if (!g_chatboxStatusFetchInProgress.compare_exchange_strong(expected, true)) {
            return;
        }

        ThreadPool::getInstance().enqueue(
            "PrismaUIChatboxStatusFetch",
            []() {
                try {
                    std::string response = FetchOverlayFromServer();
                    if (response.empty()) {
                        g_chatboxStatusFetchInProgress.store(false);
                        return;
                    }

                    size_t jsonStart = response.find('{');
                    size_t jsonEnd = response.rfind('}');
                    if (jsonStart == std::string::npos || jsonEnd == std::string::npos || jsonEnd < jsonStart) {
                        g_chatboxStatusFetchInProgress.store(false);
                        return;
                    }

                    std::string jsonBody = response.substr(jsonStart, jsonEnd - jsonStart + 1);
                    json parsed = json::parse(jsonBody, nullptr, false);
                    if (parsed.is_discarded()) {
                        g_chatboxStatusFetchInProgress.store(false);
                        return;
                    }

                    if (parsed.contains("success") && parsed["success"].is_boolean() && parsed["success"].get<bool>() &&
                        parsed.contains("data") && parsed["data"].is_object()) {
                        const auto& data = parsed["data"];
                        if (data.contains("mode") && data["mode"].is_string()) {
                            g_chatboxCurrentMode = data["mode"].get<std::string>();
                        }
                        if (data.contains("active_model_label") && data["active_model_label"].is_string()) {
                            g_chatboxCurrentModelLabel = data["active_model_label"].get<std::string>();
                        } else if (data.contains("active_model_slot") && data["active_model_slot"].is_number_integer()) {
                            int slot = data["active_model_slot"].get<int>();
                            switch (slot) {
                            case 2:
                                g_chatboxCurrentModelLabel = "Fast";
                                break;
                            case 3:
                                g_chatboxCurrentModelLabel = "Powerful";
                                break;
                            case 4:
                                g_chatboxCurrentModelLabel = "Experimental";
                                break;
                            default:
                                g_chatboxCurrentModelLabel = "Standard";
                                break;
                            }
                        }
                        if (data.contains("focus_chat")) {
                            bool enabled = false;
                            if (data["focus_chat"].is_boolean()) {
                                enabled = data["focus_chat"].get<bool>();
                            } else if (data["focus_chat"].is_number_integer()) {
                                enabled = data["focus_chat"].get<int>() != 0;
                            }
                            g_chatboxFocusChatEnabled.store(enabled);
                            g_chatboxFocusChatInitialized.store(true);
                        }
                        if (data.contains("rechat_mode") && data["rechat_mode"].is_string()) {
                            g_chatboxCurrentRechatMode = data["rechat_mode"].get<std::string>();
                            g_chatboxRechatModeLoaded.store(true);
                        }
                    }
                } catch (...) {
                    // Ignore sync errors; next polling cycle retries.
                }
                g_chatboxStatusFetchInProgress.store(false);
            },
            "ChatboxStatusFetch",
            std::chrono::seconds(30)
        );
    }

    void CheckAndUpdateChatboxControls(bool force) {
        if (!g_prismaUI || !g_chatboxCreated.load() || !g_chatboxDomReady.load()) {
            return;
        }

        // Hidden panel: skip the per-agent navmesh Dijkstra unless caller forces a refresh.
        if (!force && g_chatboxState.load() == 0) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        if (!force &&
            now - g_lastChatboxControlsCheck < kChatboxControlsMinInterval) {
            return;
        }
        g_lastChatboxControlsCheck = now;

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastChatboxStatusSync).count() >= 5000) {
            g_lastChatboxStatusSync = now;
            SyncChatboxStatusFromServerAsync();
        }

        auto nearbyAgents = CollectChatboxNearbyAgents();
        const bool narratorOnlyMode = IsChatboxNarratorOnlyMode();
        const bool directorMode = IsChatboxDirectorMode();
        const bool overrideSupported = !narratorOnlyMode && !directorMode && !IsChatboxSpawnMode();
        const bool everyoneSupported = overrideSupported;

        ChatboxNearbyAgent autoTarget{};
        bool hasAutoTarget = false;
        uint32_t selectedFormId = 0;
        std::string selectedName;
        float selectedDistance = 0.0f;
        bool everyoneActive = false;

        if (narratorOnlyMode) {
            if (!nearbyAgents.empty()) {
                autoTarget = nearbyAgents.front();
                hasAutoTarget = true;
            }
            if (g_chatboxTargetMode != ChatboxTargetMode::Auto ||
                g_chatboxTargetOverrideFormId != 0 || !g_chatboxTargetOverrideName.empty()) {
                ClearChatboxTargetOverride();
            }
        } else {
            RE::TESObjectREFRPtr crosshairTarget;
            if (auto crosshairData = RE::CrosshairPickData::GetSingleton(); crosshairData) {
                crosshairTarget = crosshairData->target.get();
            }
            if (crosshairTarget && crosshairTarget.get()->GetFormType() == RE::FormType::ActorCharacter) {
                auto potentialTarget = crosshairTarget.get()->As<RE::Actor>();
                if (potentialTarget) {
                    uint32_t crosshairFormId = potentialTarget->GetFormID();
                    for (const auto& nearbyAgent : nearbyAgents) {
                        if (nearbyAgent.formId == crosshairFormId && nearbyAgent.targetable) {
                            autoTarget = nearbyAgent;
                            hasAutoTarget = true;
                            break;
                        }
                    }
                }
            }

            if (!hasAutoTarget) {
                auto eligibleIt = std::find_if(
                    nearbyAgents.begin(),
                    nearbyAgents.end(),
                    [](const ChatboxNearbyAgent& nearbyAgent) {
                        return nearbyAgent.autoEligible;
                    });
                if (eligibleIt != nearbyAgents.end()) {
                    autoTarget = *eligibleIt;
                    hasAutoTarget = true;
                }
            }
        }

        const ChatboxNearbyAgent* overrideTarget = nullptr;
        if (overrideSupported && g_chatboxTargetMode == ChatboxTargetMode::Everyone) {
            everyoneActive = true;
            selectedName = "Everyone";
            selectedDistance = 0.0f;
        } else if (overrideSupported && g_chatboxTargetMode == ChatboxTargetMode::NPC &&
                   (g_chatboxTargetOverrideFormId != 0 || !g_chatboxTargetOverrideName.empty())) {
            for (const auto& nearbyAgent : nearbyAgents) {
                const bool formMatch = g_chatboxTargetOverrideFormId != 0 &&
                    nearbyAgent.formId == g_chatboxTargetOverrideFormId;
                const bool nameMatch = !g_chatboxTargetOverrideName.empty() &&
                    nearbyAgent.name == g_chatboxTargetOverrideName;
                if (formMatch || nameMatch) {
                    overrideTarget = &nearbyAgent;
                    if (g_chatboxTargetOverrideFormId == 0 && nearbyAgent.formId != 0) {
                        g_chatboxTargetOverrideFormId = nearbyAgent.formId;
                    }
                    if (g_chatboxTargetOverrideName.empty()) {
                        g_chatboxTargetOverrideName = nearbyAgent.name;
                    }
                    break;
                }
            }
        } else if (!overrideSupported && (g_chatboxTargetMode != ChatboxTargetMode::Auto ||
                   g_chatboxTargetOverrideFormId != 0 || !g_chatboxTargetOverrideName.empty())) {
            ClearChatboxTargetOverride();
        }

        if (overrideTarget) {
            selectedFormId = overrideTarget->formId;
            selectedName = overrideTarget->name;
            selectedDistance = overrideTarget->distanceMeters;
        } else if (g_chatboxTargetMode == ChatboxTargetMode::NPC &&
                   (g_chatboxTargetOverrideFormId != 0 || !g_chatboxTargetOverrideName.empty())) {
            logger::info(
                "[Chatbox] Clearing unavailable target override formId={:08X} name='{}'",
                g_chatboxTargetOverrideFormId, g_chatboxTargetOverrideName);
            ClearChatboxTargetOverride();
            if (hasAutoTarget) {
                selectedFormId = autoTarget.formId;
                selectedName = autoTarget.name;
                selectedDistance = autoTarget.distanceMeters;
            }
        } else if (!everyoneActive && hasAutoTarget) {
            selectedFormId = autoTarget.formId;
            selectedName = autoTarget.name;
            selectedDistance = autoTarget.distanceMeters;
        }

        if (selectedFormId == 0 && selectedName.empty()) {
            if (!g_lastChatboxTarget.empty()) {
                UpdateChatboxTargetUI("", 0.0f);
                g_lastChatboxTarget.clear();
                g_lastChatboxTargetFormId = 0;
            }
        } else if (selectedName != g_lastChatboxTarget || selectedFormId != g_lastChatboxTargetFormId) {
            UpdateChatboxTargetUI(selectedName, selectedDistance);
            g_lastChatboxTarget = selectedName;
            g_lastChatboxTargetFormId = selectedFormId;
        }

        json targetsPayload;
        targetsPayload["mode"] = g_chatboxCurrentMode;
        targetsPayload["override_active"] = everyoneActive || overrideTarget != nullptr;
        targetsPayload["override_mode"] = everyoneActive ? "everyone" : (overrideTarget ? "npc" : "auto");
        targetsPayload["override_form_id"] = overrideTarget ? overrideTarget->formId : 0;
        targetsPayload["override_name"] = everyoneActive ? "Everyone" : (overrideTarget ? overrideTarget->name : "");
        targetsPayload["active_form_id"] = selectedFormId;
        targetsPayload["active_name"] = selectedName;
        targetsPayload["show_auto"] = overrideSupported;
        targetsPayload["auto_active"] = !everyoneActive && !overrideTarget && hasAutoTarget;
        targetsPayload["show_everyone"] = everyoneSupported;
        targetsPayload["everyone_active"] = everyoneActive;
        targetsPayload["empty_message"] = IsChatboxSpawnMode()
            ? "Target override is unavailable in Spawn mode."
            : (directorMode ? "No direct speaker targets are available in Director mode."
                : (narratorOnlyMode ? "Only The Narrator is available in Narrator mode."
                    : "No spatially available targets right now."));

        json targetItems = json::array();
        for (const auto& nearbyAgent : nearbyAgents) {
            json target;
            target["form_id"] = nearbyAgent.formId;
            target["name"] = nearbyAgent.name;
            target["distance"] = nearbyAgent.distanceMeters;
            target["status"] = nearbyAgent.status;
            target["targetable"] = nearbyAgent.targetable;
            target["active"] = (selectedFormId != 0 && nearbyAgent.formId == selectedFormId) ||
                (!selectedName.empty() && nearbyAgent.name == selectedName);
            target["override"] = overrideTarget &&
                ((overrideTarget->formId != 0 && nearbyAgent.formId == overrideTarget->formId) ||
                 (!overrideTarget->name.empty() && nearbyAgent.name == overrideTarget->name));
            target["narrator"] = nearbyAgent.isNarrator;
            targetItems.push_back(target);
        }
        targetsPayload["targets"] = targetItems;

        const std::string serializedTargets = targetsPayload.dump();
        const bool targetsPayloadChanged = serializedTargets != g_lastChatboxTargetsPayload;
        if (targetsPayloadChanged) {
            UpdateChatboxTargetsUI(serializedTargets);
            g_lastChatboxTargetsPayload = serializedTargets;
        }

        if (!g_chatboxModeInitialized || g_lastChatboxMode != g_chatboxCurrentMode) {
            UpdateChatboxModeUI(g_chatboxCurrentMode);
            g_lastChatboxMode = g_chatboxCurrentMode;
            g_chatboxModeInitialized = true;
        }

        if (!g_chatboxModelInitialized || g_lastChatboxModelLabel != g_chatboxCurrentModelLabel) {
            UpdateChatboxModelUI(g_chatboxCurrentModelLabel);
            g_lastChatboxModelLabel = g_chatboxCurrentModelLabel;
            g_chatboxModelInitialized = true;
        }

        if (g_chatboxFocusChatInitialized.load()) {
            bool enabled = g_chatboxFocusChatEnabled.load();
            if (!g_chatboxFocusChatSentInitialized || enabled != g_lastChatboxFocusChatSent) {
                UpdateChatboxFocusUI(enabled);
                g_lastChatboxFocusChatSent = enabled;
                g_chatboxFocusChatSentInitialized = true;
            }
        }

        if (g_chatboxRechatModeLoaded.load() &&
            (!g_chatboxRechatModeSentInitialized || g_lastChatboxRechatMode != g_chatboxCurrentRechatMode)) {
            UpdateChatboxRechatModeUI(g_chatboxCurrentRechatMode);
            g_lastChatboxRechatMode = g_chatboxCurrentRechatMode;
            g_chatboxRechatModeSentInitialized = true;
        }
    }

    void CreateChatboxPanel() {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create chatbox - Prisma UI not initialized");
            return;
        }

        if (g_chatboxCreated.load()) {
            logger::debug("[PrismaUIBridge] Chatbox panel already created");
            return;
        }

        logger::info("[PrismaUIBridge] Creating CHIM chatbox panel from CHIM/chatbox.html...");

        g_chatboxView = g_prismaUI->CreateView("CHIM/chatbox.html", OnChatboxDomReady);

        if (g_chatboxView == 0) {
            g_lastError = "Failed to create chatbox view - check that Data/PrismaUI/views/CHIM/chatbox.html exists";
            logger::error("[PrismaUIBridge] {}", g_lastError);
            
            // Get game timestamp as string for system log
            char timeDateString[200];
            RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, false);
            PushSystemLogEntry("error", g_lastError, std::string(timeDateString));
            return;
        }

        logger::info("[PrismaUIBridge] View created with ID: {}", g_chatboxView);

        // Set view order (high number = on top)
        g_prismaUI->SetOrder(g_chatboxView, 110);

        // Set scroll speed
        g_prismaUI->SetScrollingPixelSize(g_chatboxView, 100);

        // Register listener for commands from JS
        g_prismaUI->RegisterJSListener(g_chatboxView, "chimChatboxCommand", OnChatboxCommand);

        // Warm-load the shared chatbox view in the background but keep it hidden until one of
        // the chatbox hotkeys explicitly shows/focuses it.
        g_prismaUI->Hide(g_chatboxView);
        g_chatboxState.store(0);

        g_chatboxCreated.store(true);
        logger::info("[PrismaUIBridge] Chatbox panel created successfully");
    }

    static void OnChatboxDomReady(PrismaView view) {
        logger::info("[PrismaUIBridge] Chatbox panel DOM ready");
        g_chatboxDomReady.store(true);
        g_lastChatboxTargetsPayload.clear();
        g_prismaDisplayStatusCache.clear();
        
        // Push welcome system message with configuration info
        char timeDateString[200];
        RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, false);
        
        Conf& conf = Conf::getInstance();
        std::string welcomeMsg = "CHIM Chatbox initialized. System information:\n";
        welcomeMsg += "- Plugin: CHIM\n";
        welcomeMsg += "- PrismaUI: Active\n";
        welcomeMsg += "- Server: " + conf.getServer() + ":" + conf.getPort() + "\n";
        welcomeMsg += "- Status: " + std::string(conf.isOk() ? "Connected" : "Disconnected");
        
        PushSystemLogEntry("info", welcomeMsg, std::string(timeDateString));
        UpdateChatboxModeUI(g_chatboxCurrentMode);
        SyncChatboxStatusFromServerAsync();
        logger::info("[PrismaUIBridge] Pushed welcome message to chatbox");
    }

    static void OnChatboxCommand(const char* argument) {
        if (!argument) return;

        std::string cmd(argument);
        logger::debug("[PrismaUIBridge] Received chatbox command: {}", cmd);

        if (cmd == "close") {
            ClearChatboxTargetOverride();
            if (g_prismaUI && g_prismaUI->HasFocus(g_chatboxView)) {
                g_prismaUI->Unfocus(g_chatboxView);
            }
            HideChatboxPanel();
        } else if (cmd.starts_with("send|")) {
            // Extract message after "send|"
            std::string message = cmd.substr(5);
            if (!message.empty()) {
                SendChatboxMessage(message);
            }
        } else if (cmd == "focus") {
            // Focus the chatbox for typing
            FocusChatboxPanel();
        } else if (cmd == "unfocus") {
            // Unfocus the chatbox, return control to game
            UnfocusChatboxPanel();
        } else if (cmd.starts_with("mode_")) {
            if (ApplyModeSelection(cmd, "Chatbox", true)) {
                UpdateChatboxModeUI(g_chatboxCurrentMode);
                CheckAndUpdateChatboxControls(true);
            }
        } else if (cmd.starts_with("llm_")) {
            ApplyLLMProfileSelection(cmd, "Chatbox", true);
        } else if (cmd == "continue_chat") {
            CheckAndUpdateChatboxControls(true);
            if (g_chatboxTargetMode == ChatboxTargetMode::Everyone) {
                TriggerContinueConversationForEveryone("Chatbox", false);
            } else {
                TriggerContinueConversationForNpc(g_lastChatboxTarget, "Chatbox", true);
            }
        } else if (cmd == "stop_all_dialogue") {
            StopAllDialogueNow("Chatbox");
        } else if (cmd == "halt_ai_actions") {
            {
                std::lock_guard<std::mutex> lock(g_settingsMenuMutex);
                g_pendingSettingsAction = "rp_halt";
            }

            logger::info("[Chatbox] Queued global halt AI action");
        } else if (cmd.starts_with("debug_notify|")) {
            std::string message = cmd.substr(13);
            if (!message.empty()) {
                if (message.find("[CHIM]") != 0) {
                    message = "[CHIM] " + message;
                }
                RE::DebugNotification(message.c_str());
            }
        } else if (cmd == "target_override_clear") {
            ClearChatboxTargetOverride();
            CheckAndUpdateChatboxControls(true);
        } else if (cmd == "target_override_everyone") {
            if (IsChatboxNarratorOnlyMode() || IsChatboxDirectorMode() || IsChatboxSpawnMode()) {
                ClearChatboxTargetOverride();
                CheckAndUpdateChatboxControls(true);
                return;
            }

            SetChatboxEveryoneTargetOverride();
            logger::info("[Chatbox] Applied Everyone target override");
            CheckAndUpdateChatboxControls(true);
        } else if (cmd.starts_with("target_override|")) {
            std::string payload = cmd.substr(16);
            std::string formIdText;
            std::string targetName;
            size_t pipePos = payload.find('|');
            if (pipePos == std::string::npos) {
                formIdText = payload;
            } else {
                formIdText = payload.substr(0, pipePos);
                targetName = payload.substr(pipePos + 1);
            }

            uint32_t formId = 0;
            if (!formIdText.empty()) {
                try {
                    formId = static_cast<uint32_t>(std::stoul(formIdText));
                } catch (...) {
                    formId = 0;
                }
            }

            if (IsChatboxNarratorOnlyMode()) {
                ClearChatboxTargetOverride();
                CheckAndUpdateChatboxControls(true);
                return;
            }

            if (!targetName.empty() || formId != 0) {
                auto agent = FindChatboxAgentByFormIdOrName(formId, targetName);
                auto nearbyAgents = CollectChatboxNearbyAgents();
                const bool targetIsNearby = agent && agent->getActor() &&
                    std::find_if(
                        nearbyAgents.begin(),
                        nearbyAgents.end(),
                        [&](const ChatboxNearbyAgent& nearbyAgent) {
                            return nearbyAgent.formId == agent->getActor()->GetFormID();
                        }) != nearbyAgents.end();
                if (targetIsNearby) {
                    SetChatboxTargetOverride(agent->getActor()->GetFormID(), agent->getActorName());
                    logger::info("[Chatbox] Applied explicit target override to {}", agent->getActorName());
                    CheckAndUpdateChatboxControls(true);
                } else {
                    logger::warn("[Chatbox] Ignoring unavailable target override formId={} name='{}'", formId, targetName);
                    ClearChatboxTargetOverride();
                    CheckAndUpdateChatboxControls(true);
                    RE::DebugNotification("[CHIM] That target is not currently available.");
                }
            }
        } else if (cmd == "focus_chat_toggle") {
            bool newFocusChatState = !g_chatboxFocusChatEnabled.load();
            HTTPManager::log(std::format("setconf|{}|{}|chim_context_mode@{}",
                getCurrentTimeMillis(), GetGameTimeStamp(), newFocusChatState ? 1 : 0));
            g_chatboxFocusChatEnabled.store(newFocusChatState);
            g_chatboxFocusChatInitialized.store(true);
            UpdateChatboxFocusUI(newFocusChatState);
            g_lastChatboxFocusChatSent = newFocusChatState;
            g_chatboxFocusChatSentInitialized = true;
            RE::DebugNotification(newFocusChatState ? "[CHIM] Focus Chat enabled." : "[CHIM] Focus Chat disabled.");
        }
    }

    void ToggleChatboxPanel() {
        logger::info("[PrismaUIBridge] Chatbox toggle requested");

        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot toggle - Prisma UI not initialized");
            return;
        }

        // If panel not created yet, create it
        if (!g_chatboxCreated.load()) {
            logger::info("[PrismaUIBridge] Panel not created yet, creating now...");
            CreateChatboxPanel();
            if (!g_chatboxCreated.load()) {
                logger::error("[PrismaUIBridge] Failed to create chatbox panel");
                return;
            }
        }

        // Check if DOM is ready
        if (!g_chatboxDomReady.load()) {
            logger::warn("[PrismaUIBridge] DOM not ready yet - HTML may still be loading");
            // Force show attempt anyway
            g_prismaUI->Show(g_chatboxView);
            g_chatboxState.store(1);
            return;
        }

        const bool isHidden = g_prismaUI->IsHidden(g_chatboxView);
        logger::info("[PrismaUIBridge] Current visibility: {}", isHidden ? "hidden" : "visible");

        if (isHidden) {
            logger::info("[PrismaUIBridge] Showing chatbox panel");
            ShowChatboxPanel();
        } else {
            logger::info("[PrismaUIBridge] Hiding chatbox panel");
            HideChatboxPanel();
        }
    }

    void ShowChatboxPanel() {
        if (!g_prismaUI || !g_chatboxCreated.load()) {
            // Try to create it if not yet created
            if (!g_chatboxCreated.load()) {
                CreateChatboxPanel();
            }
            if (!g_chatboxCreated.load()) {
                return;
            }
        }

        // Check if view is still valid
        if (!g_prismaUI->IsValid(g_chatboxView)) {
            logger::error("[PrismaUIBridge] Chatbox view is not valid");
            return;
        }

        logger::info("[PrismaUIBridge] Showing chatbox panel (no auto-focus)");
        g_chatboxQuickFocusActive.store(false);
        g_prismaUI->Show(g_chatboxView);
        g_chatboxState.store(1);
        CheckAndUpdateChatboxControls(true);
        
        // No auto-focus - player retains control until they press Enter
    }

    void HideChatboxPanel() {
        SetChatboxGameplayInputSuppressed(false);

        if (!g_prismaUI || !g_chatboxCreated.load()) {
            return;
        }

        ClearChatboxTargetOverride();
        g_chatboxQuickFocusActive.store(false);

        // Unfocus first if focused
        if (g_prismaUI->HasFocus(g_chatboxView)) {
            g_prismaUI->Unfocus(g_chatboxView);
        }

        logger::info("[PrismaUIBridge] Hiding chatbox panel");
        g_prismaUI->Hide(g_chatboxView);
        g_chatboxState.store(0); // Reset state to hidden
    }

    bool IsChatboxPanelVisible() {
        if (!g_prismaUI || !g_chatboxCreated.load()) {
            return false;
        }
        return !g_prismaUI->IsHidden(g_chatboxView);
    }

    bool FocusChatboxPanel() {
        if (!g_prismaUI) {
            SetChatboxGameplayInputSuppressed(false);
            logger::warn("[PrismaUIBridge] Cannot focus chatbox - Prisma UI not initialized");
            return false;
        }

        constexpr const char* kPrepareQuickChatJs = "window.prepareQuickChatFocus && window.prepareQuickChatFocus()";

        // Track visibility state before creating/showing so JS can decide focus-only mode.
        bool wasVisibleAtStart = false;
        if (g_chatboxCreated.load() && g_prismaUI->IsValid(g_chatboxView)) {
            wasVisibleAtStart = !g_prismaUI->IsHidden(g_chatboxView);
        }

        if (!g_chatboxCreated.load()) {
            logger::info("[PrismaUIBridge] Panel not created yet, creating for focus...");
            CreateChatboxPanel();
            if (!g_chatboxCreated.load()) {
                SetChatboxGameplayInputSuppressed(false);
                logger::error("[PrismaUIBridge] Failed to create chatbox panel");
                return false;
            }
        }

        // Wait for DOM ready with a cold-start timeout. First open can take longer than a second
        // while Prisma creates the view and loads the CHIM chatbox assets.
        if (!g_chatboxDomReady.load()) {
            logger::info("[PrismaUIBridge] Waiting for chatbox DOM ready...");
            constexpr auto kDomReadyPollInterval = std::chrono::milliseconds(20);
            constexpr auto kDomReadyTimeout = std::chrono::milliseconds(3000);
            const auto waitStart = std::chrono::steady_clock::now();
            while (!g_chatboxDomReady.load()) {
                const auto waited = std::chrono::steady_clock::now() - waitStart;
                if (waited >= kDomReadyTimeout) {
                    const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(waited).count();
                    SetChatboxGameplayInputSuppressed(false);
                    logger::warn("[PrismaUIBridge] DOM ready timeout after {} ms - cannot focus chatbox", waitedMs);
                    if (!wasVisibleAtStart && g_prismaUI->IsValid(g_chatboxView) && !g_prismaUI->IsHidden(g_chatboxView)) {
                        HideChatboxPanel();
                    }
                    return false;
                }
                std::this_thread::sleep_for(kDomReadyPollInterval);
            }
            const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - waitStart).count();
            logger::debug("[PrismaUIBridge] Chatbox DOM ready after {} ms", waitedMs);
        }

        // Already focused - skip (check using HasFocus instead of keyboard active flag)
        if (g_prismaUI->HasFocus(g_chatboxView)) {
            SetChatboxGameplayInputSuppressed(true);
            logger::debug("[PrismaUIBridge] Chatbox already focused, skipping");
            return true;
        }

        // Unfocus ALL other views first to avoid focus conflicts
        if (g_panelCreated.load() && g_prismaUI->HasFocus(g_historyView)) {
            g_prismaUI->Unfocus(g_historyView);
        }
        if (g_overlayCreated.load() && g_prismaUI->HasFocus(g_overlayView)) {
            g_prismaUI->Unfocus(g_overlayView);
        }
        if (g_diariesCreated.load() && g_prismaUI->HasFocus(g_diariesView)) {
            g_prismaUI->Unfocus(g_diariesView);
        }
        if (g_browserCreated.load() && g_prismaUI->HasFocus(g_browserView)) {
            g_prismaUI->Unfocus(g_browserView);
        }

        // Prepare the quick-chat modal state before/while showing the shared chatbox view.
        g_prismaUI->Invoke(g_chatboxView, kPrepareQuickChatJs, nullptr);

        // If hidden, show panel so focus modal can appear without exposing the full history view first.
        if (g_prismaUI->IsHidden(g_chatboxView)) {
            g_prismaUI->Show(g_chatboxView);
            g_chatboxState.store(1);
            g_prismaUI->Invoke(g_chatboxView, kPrepareQuickChatJs, nullptr);
        }

        // Focus with game PAUSED and FocusMenu for cursor.
        // Retry a few times because Show() can take a frame to become focusable.
        bool success = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            success = g_prismaUI->Focus(g_chatboxView, true, false);
            if (success) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        
        if (success) {
            SetChatboxGameplayInputSuppressed(true);
            g_chatboxQuickFocusActive.store(!wasVisibleAtStart);
            logger::info("[PrismaUIBridge] Chatbox focused - game paused (quickFocus={})", !wasVisibleAtStart);
            CheckAndUpdateChatboxControls(true);
            g_prismaUI->Invoke(
                g_chatboxView,
                wasVisibleAtStart ? "window.onChatboxFocused(false)" : "window.onChatboxFocused(true)",
                nullptr);
            // JS opens the centered focus chat modal and focuses its textarea.
        } else {
            SetChatboxGameplayInputSuppressed(false);
            g_chatboxQuickFocusActive.store(false);
            logger::warn("[PrismaUIBridge] Failed to focus chatbox panel");
            // Restore original hidden state so a failed quick-focus does not leave the tabbed chatbox visible.
            if (!wasVisibleAtStart) {
                HideChatboxPanel();
            }
        }

        return success;
    }

    void UnfocusChatboxPanel() {
        SetChatboxGameplayInputSuppressed(false);

        if (!g_prismaUI || !g_chatboxCreated.load()) {
            return;
        }

        const bool shouldHideAfterUnfocus = g_chatboxQuickFocusActive.load();
        if (g_chatboxDomReady.load()) {
            g_prismaUI->Invoke(g_chatboxView, "window.onChatboxUnfocused()", nullptr);
        }
        g_prismaUI->Unfocus(g_chatboxView);
        logger::info("[PrismaUIBridge] Chatbox unfocused - control returned to game");

        if (shouldHideAfterUnfocus) {
            logger::info("[PrismaUIBridge] Quick chat unfocus detected - hiding shared chatbox panel");
            HideChatboxPanel();
            return;
        }
    }

    bool IsChatboxPanelFocused() {
        if (!g_prismaUI || !g_chatboxCreated.load()) {
            return false;
        }
        return g_prismaUI->HasFocus(g_chatboxView);
    }

    bool IsAnyHotkeyPanelFocused() {
        if (!g_prismaUI) {
            return false;
        }

        const auto isFocused = [](PrismaView view, bool created) {
            return created && view != 0 && g_prismaUI->IsValid(view) && g_prismaUI->HasFocus(view);
        };

        return isFocused(g_historyView, g_panelCreated.load()) ||
               isFocused(g_diariesView, g_diariesCreated.load()) ||
               isFocused(g_browserView, g_browserCreated.load()) ||
               isFocused(g_debuggerView, g_debuggerCreated.load()) ||
               isFocused(g_chatboxView, g_chatboxCreated.load()) ||
               isFocused(g_settingsMenuView, g_settingsMenuCreated.load()) ||
               isFocused(g_masterMenuView, g_masterMenuCreated.load());
    }

    std::string GetCurrentChatboxMode() {
        return g_chatboxCurrentMode;
    }

    float GetPlayerSpeechDistanceMultiplier() {
        float multiplier = 1.0f;
        if (g_chatboxCurrentMode == "WHISPER") {
            multiplier = 0.35f;
        } else if (g_chatboxCurrentMode == "SHOUT") {
            multiplier = 2.0f;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && player->IsSneaking()) {
            // Sneak is a physical state modifier, not a UI mode switch. Keep the
            // selected mode visible, but make player speech carry less while crouched.
            multiplier *= 0.5f;
        }

        return multiplier;
    }

    float GetPlayerSpeechPlaybackVolumeMultiplier() {
        return g_chatboxCurrentMode == "SHOUT" ? 1.5f : 1.0f;
    }

    bool IsNarratorChatModeEnabled() {
        return g_chatboxCurrentMode == "NARRATOR";
    }

    // Deduplication cache for chatbox messages - prevents duplicate pushes
    // from multiple code paths (ProcedureListenToScene, MonitorAllSubtitles, direct pushes)
    static std::list<std::string> g_chatboxMessageCache;
    static const size_t MAX_CHATBOX_MSG_CACHE = 150;

    void PushChatboxMessage(const std::string& speaker, const std::string& text,
                            const std::string& timestamp, const std::string& type,
                            const std::string& source) {
        if (!g_prismaUI || !g_chatboxCreated.load() || !g_chatboxDomReady.load()) {
            return;
        }

        // Deduplicate: same speaker+text = same message regardless of caller
        std::string dedupeKey = speaker + "|" + text;
        auto it = std::find(g_chatboxMessageCache.begin(), g_chatboxMessageCache.end(), dedupeKey);
        if (it != g_chatboxMessageCache.end()) {
            return; // Already pushed this exact message recently
        }
        g_chatboxMessageCache.push_back(dedupeKey);
        while (g_chatboxMessageCache.size() > MAX_CHATBOX_MSG_CACHE) {
            g_chatboxMessageCache.pop_front();
        }

        try {
            // Escape all parameters for JavaScript
            auto escapeForJS = [](const std::string& str) -> std::string {
                std::string escaped = str;
                size_t pos = 0;
                // Escape backslashes
                while ((pos = escaped.find('\\', pos)) != std::string::npos) {
                    escaped.replace(pos, 1, "\\\\");
                    pos += 2;
                }
                // Escape single quotes
                pos = 0;
                while ((pos = escaped.find('\'', pos)) != std::string::npos) {
                    escaped.replace(pos, 1, "\\'");
                    pos += 2;
                }
                // Escape newlines
                pos = 0;
                while ((pos = escaped.find('\n', pos)) != std::string::npos) {
                    escaped.replace(pos, 1, "\\n");
                    pos += 2;
                }
                return escaped;
            };

            std::string escapedSpeaker = escapeForJS(speaker);
            std::string escapedText = escapeForJS(text);
            std::string escapedTimestamp = escapeForJS(timestamp);
            std::string escapedType = escapeForJS(type);
            std::string escapedSource = escapeForJS(source);

            std::string jsCall = "window.pushChatMessage('" + escapedSpeaker + "', '" + 
                                escapedText + "', '" + escapedTimestamp + "', '" + 
                                escapedType + "', '" + escapedSource + "')";

            g_prismaUI->Invoke(g_chatboxView, jsCall.c_str(), nullptr);
            logger::debug("[PrismaUIBridge] Pushed chat message: {} - {}", speaker, text.substr(0, 50));

        } catch (const std::exception& e) {
            logger::error("[PrismaUIBridge] Error pushing chat message: {}", e.what());
        }
    }

    void PushSystemLogEntry(const std::string& level, const std::string& message,
                            const std::string& timestamp) {
        if (!g_prismaUI || !g_chatboxCreated.load() || !g_chatboxDomReady.load()) {
            return;
        }

        // Push logs even when hidden so they appear when user opens chatbox
        // Removed: if (g_prismaUI->IsHidden(g_chatboxView)) { return; }

        try {
            // Escape all parameters for JavaScript
            auto escapeForJS = [](const std::string& str) -> std::string {
                std::string escaped = str;
                size_t pos = 0;
                while ((pos = escaped.find('\\', pos)) != std::string::npos) {
                    escaped.replace(pos, 1, "\\\\");
                    pos += 2;
                }
                pos = 0;
                while ((pos = escaped.find('\'', pos)) != std::string::npos) {
                    escaped.replace(pos, 1, "\\'");
                    pos += 2;
                }
                pos = 0;
                while ((pos = escaped.find('\n', pos)) != std::string::npos) {
                    escaped.replace(pos, 1, "\\n");
                    pos += 2;
                }
                return escaped;
            };

            std::string escapedLevel = escapeForJS(level);
            std::string escapedMessage = escapeForJS(message);
            std::string escapedTimestamp = escapeForJS(timestamp);

            std::string jsCall = "window.pushSystemLog('" + escapedLevel + "', '" + 
                                escapedMessage + "', '" + escapedTimestamp + "')";

            g_prismaUI->Invoke(g_chatboxView, jsCall.c_str(), nullptr);
            logger::debug("[PrismaUIBridge] Pushed system log: {} - {}", level, message.substr(0, 50));

        } catch (const std::exception& e) {
            logger::error("[PrismaUIBridge] Error pushing system log: {}", e.what());
        }
    }

    void SendChatboxMessage(const std::string& message) {
        if (message.empty()) {
            return;
        }

        logger::info("[PrismaUIBridge] Sending chatbox message: {}", message);

        // Get player name
        auto player = RE::PlayerCharacter::GetSingleton();
        std::string playerName = player ? player->GetName() : "Player";
        
        // Push to chatbox UI with actual player name
        // This will show the single message with the correct player name
        PushChatboxMessage(playerName, message, "", "player");
        
        // Send to server - this will interrupt conversations and generate AI response (same as MCM text hotkey)
        // sendMessageReal handles: queue deletion, stream cancellation, and NPC interruption
        sendMessageReal(message, "");
    }

    static void StopAllDialogueNow(const char* sourceTag) {
        logger::info("[{}] Stop All Dialogue requested", sourceTag);
        g_dialogueStopGeneration.fetch_add(1, std::memory_order_relaxed);

        ThreadPool::getInstance().cancelTasksByType("HTTPStream");
        ThreadPool::getInstance().cancelTasksByType("HTTPStreamRechat");
        SPGResponse::getInstance().clearAllQueues();

        SpeakManager& speakManager = SpeakManager::getInstance();
        speakManager.abortPendingUtterances("hard_stop");
        speakManager.deleteQueue();
        speakManager.deleteQueuedPlayerLines();
        if (speakManager.getProcessing()) {
            speakManager.abortPlay(true);
        }
        speakManager.stopRechatForNseconds(3);

        RE::DebugNotification("[CHIM] Stopped all dialogue.");
    }

    // ===== CHIM Settings Menu Functions =====

    void CreateSettingsMenu() {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create settings menu - Prisma UI not initialized");
            return;
        }

        if (g_settingsMenuCreated.load()) {
            logger::debug("[PrismaUIBridge] Settings menu already created");
            return;
        }

        logger::info("[PrismaUIBridge] Creating CHIM settings menu from CHIM/settings_menu.html...");

        // Create the view - path is relative to Data/PrismaUI/views/
        g_settingsMenuView = g_prismaUI->CreateView("CHIM/settings_menu.html", OnSettingsMenuDomReady);

        if (g_settingsMenuView == 0) {
            g_lastError = "Failed to create settings menu view - check that Data/PrismaUI/views/CHIM/settings_menu.html exists";
            logger::error("[PrismaUIBridge] {}", g_lastError);
            return;
        }

        logger::info("[PrismaUIBridge] Settings menu view created with ID: {}", g_settingsMenuView);

        // Set view order (on top of everything except maybe browser)
        g_prismaUI->SetOrder(g_settingsMenuView, 130);

        // Register listeners for commands from JS
        g_prismaUI->RegisterJSListener(g_settingsMenuView, "chimSettingsMenuCommand", OnSettingsMenuCommand);
        g_prismaUI->RegisterJSListener(g_settingsMenuView, "chimSettingsMenuReady", [](const char*) {
            logger::info("[PrismaUIBridge] Settings menu JavaScript ready");
        });

        g_settingsMenuCreated.store(true);
        logger::info("[PrismaUIBridge] Settings menu created successfully");
    }

    static void OnSettingsMenuDomReady(PrismaView view) {
        logger::info("[PrismaUIBridge] Settings menu DOM ready");
        g_settingsMenuDomReady.store(true);
    }

    static void OnSettingsMenuCommand(const char* argument) {
        if (!argument) return;

        std::string cmd(argument);

        // Parse command - format: "actionId" or "actionId|npcName"
        std::string actionId = cmd;
        std::string npcName = "";
        
        size_t pipePos = cmd.find('|');
        if (pipePos != std::string::npos) {
            actionId = cmd.substr(0, pipePos);
            npcName = cmd.substr(pipePos + 1);
        }

        // Handle commands that can be processed immediately in C++
        if (actionId == "close" || actionId == "dom_ready" || actionId == "trigger_papyrus_check") {
            if (actionId == "close") {
                HideSettingsMenu();
            } else if (actionId == "trigger_papyrus_check") {
                // Just close the menu and let Papyrus polling detect the pending action
                HideSettingsMenu();
            }
            return;
        }

        // Mode wheel actions
        if (actionId.starts_with("mode_")) {
            if (ApplyModeSelection(actionId, "Settings Menu", true)) {
                UpdateChatboxModeUI(g_chatboxCurrentMode);
                CheckAndUpdateChatboxControls(true);
            }
            HideSettingsMenu();
            return;
        }

        // LLM profile selection
        if (actionId.starts_with("llm_")) {
            if (actionId == "llm_focus") {
                // Use HTTPManager::log like the original wheel menus
                HTTPManager::log(std::format("setconf|{}|{}|chim_context_mode@1", 
                    getCurrentTimeMillis(), GetGameTimeStamp()));
                logger::info("[Settings Menu] Enabled Focus Chat");
                RE::DebugNotification("[CHIM] Focus Chat enabled.");
                HideSettingsMenu();
                return;
            }
            ApplyLLMProfileSelection(actionId, "Settings Menu", true);
            HideSettingsMenu();
            return;
        }

        if (actionId == "open_ai_quest_manager") {
            HideSettingsMenu();
            ShowQuestManagerPanel();
            return;
        }

        // NPC Profile assignment
        if (actionId.starts_with("profile_") && !npcName.empty()) {
            std::string profileNum = actionId.substr(8); // Extract number after "profile_"
            // Use HTTPManager::log with actor parameter like logMessageForActor
            HTTPManager::log(std::format("core_profile_assign|{}|{}|{}", 
                getCurrentTimeMillis(), GetGameTimeStamp(), profileNum), npcName);
            RE::DebugNotification(("[CHIM] Assigned Profile " + profileNum + " to " + npcName + ".").c_str());
            HideSettingsMenu();
            return;
        }

        if (actionId == "continue_chat") {
            TriggerContinueConversationForNpc(npcName, "Settings Menu", false);
            HideSettingsMenu();
            return;
        }

        // Roleplay actions that need Papyrus
        if (actionId.starts_with("rp_")) {
            bool requiresTarget = (actionId == "rp_write_diary" || actionId == "rp_update_npc" ||
                                   actionId == "rp_wait" || actionId == "rp_follow" || actionId == "rp_rename");
            if (npcName.empty() && requiresTarget) {
                logger::warn("[Settings Menu] NPC-specific action {} requires crosshair target", actionId);
                HideSettingsMenu();
                return;
            }

            // Store for Papyrus to retrieve
            {
                std::lock_guard<std::mutex> lock(g_settingsMenuMutex);
                g_pendingSettingsAction = cmd;
            }
            HideSettingsMenu();
            return;
        }

        // Halt AI - can be done in C++
        if (actionId == "rp_halt") {
            // Store as pending action for Papyrus
            std::lock_guard<std::mutex> lock(g_settingsMenuMutex);
            g_pendingSettingsAction = cmd;
            return;
        }

        // Soulgaze actions
        if (actionId.starts_with("sg_")) {
            std::lock_guard<std::mutex> lock(g_settingsMenuMutex);
            g_pendingSettingsAction = cmd;
            return;
        }

        logger::warn("[Settings Menu] Unknown action: {}", actionId);
        HideSettingsMenu();
    }

    void ToggleSettingsMenu() {
        logger::info("[PrismaUIBridge] Settings menu toggle requested");
        
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot toggle settings menu - Prisma UI not initialized");
            return;
        }

        // If menu not created yet, create it
        if (!g_settingsMenuCreated.load()) {
            logger::info("[PrismaUIBridge] Settings menu not created yet, creating now...");
            CreateSettingsMenu();
            if (!g_settingsMenuCreated.load()) {
                logger::error("[PrismaUIBridge] Failed to create settings menu");
                return;
            }
        }

        // Use our visibility flag instead of IsHidden()
        if (!g_settingsMenuVisible.load()) {
            logger::info("[PrismaUIBridge] Showing settings menu (was hidden)");
            ShowSettingsMenu();
        } else {
            logger::info("[PrismaUIBridge] Hiding settings menu (was visible)");
            HideSettingsMenu();
        }
    }

    void ShowSettingsMenu() {
        if (!g_prismaUI || !g_settingsMenuCreated.load()) {
            if (!g_settingsMenuCreated.load()) {
                CreateSettingsMenu();
            }
            if (!g_settingsMenuCreated.load()) {
                return;
            }
        }

        if (!g_prismaUI->IsValid(g_settingsMenuView)) {
            logger::error("[PrismaUIBridge] Settings menu view is not valid!");
            return;
        }

        logger::info("[PrismaUIBridge] Showing settings menu panel");
        g_prismaUI->Show(g_settingsMenuView);

        // Wait for DOM to be ready if it isn't already
        if (!g_settingsMenuDomReady.load()) {
            logger::info("[PrismaUIBridge] Waiting for settings menu DOM to be ready...");
            int waitCount = 0;
            while (!g_settingsMenuDomReady.load() && waitCount < 20) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                waitCount++;
            }
            logger::info("[PrismaUIBridge] Settings menu DOM ready status: {}", g_settingsMenuDomReady.load());
        }

        // Small delay to ensure everything is settled
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Pass server URL to JavaScript for API calls
        std::string server = Conf::getInstance().getServer();
        std::string port = Conf::getInstance().getPort();
        std::string serverUrl = "http://" + server + ":" + port;
        std::string setServerUrlCall = "window.chimServerUrl = '" + serverUrl + "'";
        g_prismaUI->Invoke(g_settingsMenuView, setServerUrlCall.c_str(), nullptr);
        logger::info("[Settings Menu] Set server URL: {}", serverUrl);

        // Check for crosshair target and pass to JS
        auto player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            auto crosshairRef = RE::Console::GetSelectedRef();
            if (!crosshairRef) {
                // Try alternative method
                auto crosshairPickData = RE::CrosshairPickData::GetSingleton();
                if (crosshairPickData && crosshairPickData->target) {
                    crosshairRef = crosshairPickData->target.get();
                }
            }

            if (crosshairRef) {
                auto actor = crosshairRef->As<RE::Actor>();
                if (actor) {
                    std::string npcName = actor->GetDisplayFullName();
                    if (!npcName.empty()) {
                        std::string jsCall = "window.setNPCTarget('" + npcName + "')";
                        g_prismaUI->Invoke(g_settingsMenuView, jsCall.c_str(), nullptr);
                        logger::info("[Settings Menu] Set NPC target: {}", npcName);
                    }
                }
            } else {
                // No target - clear NPC context
                g_prismaUI->Invoke(g_settingsMenuView, "window.setNPCTarget('none')", nullptr);
            }
        }

        // Focus with game PAUSED and FocusMenu enabled for cursor
        bool focused = g_prismaUI->Focus(g_settingsMenuView, true, false);
        logger::info("[PrismaUIBridge] Settings menu focus result: {} (paused game, using FocusMenu)", focused ? "SUCCESS" : "FAILED");

        // Verify focus was set
        if (g_prismaUI->HasFocus(g_settingsMenuView)) {
            logger::info("[PrismaUIBridge] Settings menu confirmed to have focus");
        } else {
            logger::warn("[PrismaUIBridge] Settings menu does NOT have focus after Focus() call!");
            // Try one more time
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            g_prismaUI->Focus(g_settingsMenuView, true, false);
            if (g_prismaUI->HasFocus(g_settingsMenuView)) {
                logger::info("[PrismaUIBridge] Settings menu focus succeeded on retry");
            }
        }
        
        // Mark as visible
        g_settingsMenuVisible.store(true);
    }

    void HideSettingsMenu() {
        if (!g_prismaUI || !g_settingsMenuCreated.load()) {
            return;
        }

        // Unfocus first if focused
        if (g_prismaUI->HasFocus(g_settingsMenuView)) {
            g_prismaUI->Unfocus(g_settingsMenuView);
        }

        logger::info("[PrismaUIBridge] Hiding settings menu panel");
        g_prismaUI->Hide(g_settingsMenuView);
        
        // Mark as hidden
        g_settingsMenuVisible.store(false);
    }

    bool IsSettingsMenuVisible() {
        if (!g_prismaUI || !g_settingsMenuCreated.load()) {
            return false;
        }
        return !g_prismaUI->IsHidden(g_settingsMenuView);
    }

    std::uint64_t GetDialogueStopGeneration() {
        return g_dialogueStopGeneration.load(std::memory_order_relaxed);
    }

    void BumpDialogueStopGeneration() {
        g_dialogueStopGeneration.fetch_add(1, std::memory_order_relaxed);
    }

    std::string GetPendingSettingsAction() {
        std::lock_guard<std::mutex> lock(g_settingsMenuMutex);
        return g_pendingSettingsAction;
    }

    void ClearPendingSettingsAction() {
        std::lock_guard<std::mutex> lock(g_settingsMenuMutex);
        g_pendingSettingsAction = "";
    }

    // ===== CHIM Master Menu Functions =====

    void CreateMasterMenu() {
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot create master menu - Prisma UI not initialized");
            return;
        }

        if (g_masterMenuCreated.load()) {
            logger::debug("[PrismaUIBridge] Master menu already created");
            return;
        }

        logger::info("[PrismaUIBridge] Creating CHIM master menu from CHIM/master_menu.html...");

        // Create the view - path is relative to Data/PrismaUI/views/
        g_masterMenuView = g_prismaUI->CreateView("CHIM/master_menu.html", OnMasterMenuDomReady);

        if (g_masterMenuView == 0) {
            g_lastError = "Failed to create master menu view - check that Data/PrismaUI/views/CHIM/master_menu.html exists";
            logger::error("[PrismaUIBridge] {}", g_lastError);
            return;
        }

        logger::info("[PrismaUIBridge] Master menu view created with ID: {}", g_masterMenuView);

        // Keep master menu above quest manager so it can toggle quest manager off.
        g_prismaUI->SetOrder(g_masterMenuView, 140);

        // Register listeners for commands from JS
        g_prismaUI->RegisterJSListener(g_masterMenuView, "chimMasterMenuCommand", OnMasterMenuCommand);
        g_prismaUI->RegisterJSListener(g_masterMenuView, "chimMasterMenuReady", [](const char*) {
            logger::info("[PrismaUIBridge] Master menu JavaScript ready");
        });

        g_masterMenuCreated.store(true);
        logger::info("[PrismaUIBridge] Master menu created successfully");
    }

    static void OnMasterMenuDomReady(PrismaView view) {
        logger::info("[PrismaUIBridge] Master menu DOM ready");
        g_masterMenuDomReady.store(true);
    }

    static void OnMasterMenuCommand(const char* argument) {
        if (!argument) return;

        std::string cmd(argument);
        logger::debug("[PrismaUIBridge] Received master menu command: {}", cmd);

        // Handle close command
        if (cmd == "close" || cmd == "dom_ready") {
            if (cmd == "close") {
                HideMasterMenu();
            }
            return;
        }

        const bool shouldCloseFirst =
            cmd == "history" ||
            cmd == "diaries" ||
            cmd == "overlay" ||
            cmd == "statushud" ||
            cmd == "aiview" ||
            cmd == "browser" ||
            cmd == "debugger" ||
            cmd == "chatbox" ||
            cmd == "settings" ||
            cmd == "questmanager" ||
            cmd == "tools_sync_factions_locations" ||
            cmd == "tools_send_all_voice_samples";

        if (shouldCloseFirst) {
            HideMasterMenu();
        }

        // Handle panel toggle commands
        if (cmd == "textchat") {
            HideMasterMenu();
            FocusChatboxPanel();
            return;
        } else if (cmd == "history") {
            ToggleHistoryPanel();
        } else if (cmd == "diaries") {
            ToggleDiariesPanel();
        } else if (cmd == "overlay") {
            ToggleOverlayPanel();
        } else if (cmd == "statushud") {
            ToggleStatusHUDPanel();
        } else if (cmd == "aiview") {
            ToggleAIViewPanel();
        } else if (cmd == "browser") {
            ToggleBrowserPanel();
        } else if (cmd == "debugger") {
            ToggleDebuggerPanel();
        } else if (cmd == "chatbox") {
            ToggleChatboxPanel();
        } else if (cmd == "settings") {
            ToggleSettingsMenu();
        } else if (cmd == "questmanager") {
            ToggleQuestManagerPanel();
        } else if (cmd == "tools_sync_factions_locations" || cmd == "tools_send_all_voice_samples") {
            std::lock_guard<std::mutex> lock(g_settingsMenuMutex);
            g_pendingSettingsAction = cmd;
            logger::info("[PrismaUIBridge] Queued tools action from master menu: {}", cmd);
        } else {
            logger::warn("[PrismaUIBridge] Unknown master menu command: {}", cmd);
            return;
        }
    }

    void ToggleMasterMenu() {
        logger::info("[PrismaUIBridge] Master menu toggle requested");
        
        if (!g_prismaUI) {
            logger::error("[PrismaUIBridge] Cannot toggle master menu - Prisma UI not initialized");
            return;
        }

        // If menu not created yet, create it
        if (!g_masterMenuCreated.load()) {
            logger::info("[PrismaUIBridge] Master menu not created yet, creating now...");
            CreateMasterMenu();
            if (!g_masterMenuCreated.load()) {
                logger::error("[PrismaUIBridge] Failed to create master menu");
                return;
            }
        }

        // Use our visibility flag instead of IsHidden()
        if (!g_masterMenuVisible.load()) {
            logger::info("[PrismaUIBridge] Showing master menu (was hidden)");
            ShowMasterMenu();
        } else {
            logger::info("[PrismaUIBridge] Hiding master menu (was visible)");
            HideMasterMenu();
        }
    }

    void ShowMasterMenu() {
        if (!g_prismaUI) {
            if (!g_masterMenuCreated.load()) {
                CreateMasterMenu();
            }
            if (!g_masterMenuCreated.load()) {
                return;
            }
        }

        if (!g_prismaUI->IsValid(g_masterMenuView)) {
            logger::error("[PrismaUIBridge] Master menu view is not valid!");
            return;
        }

        // Reassert menu order in case another panel changed stacking.
        g_prismaUI->SetOrder(g_masterMenuView, 140);
        logger::info("[PrismaUIBridge] Showing master menu panel");
        g_prismaUI->Show(g_masterMenuView);

        // Wait for DOM to be ready if it isn't already
        if (!g_masterMenuDomReady.load()) {
            logger::info("[PrismaUIBridge] Waiting for master menu DOM to be ready...");
            int waitCount = 0;
            while (!g_masterMenuDomReady.load() && waitCount < 20) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                waitCount++;
            }
            logger::info("[PrismaUIBridge] Master menu DOM ready status: {}", g_masterMenuDomReady.load());
        }

        // Small delay to ensure everything is settled
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Focus the view (pauses game and shows cursor)
        bool focused = g_prismaUI->Focus(g_masterMenuView, true, false);
        logger::info("[PrismaUIBridge] Master menu focus: {}", focused ? "SUCCESS" : "FAILED");

        // Verify focus was set
        if (g_prismaUI->HasFocus(g_masterMenuView)) {
            logger::info("[PrismaUIBridge] Master menu confirmed to have focus");
        } else {
            logger::warn("[PrismaUIBridge] Master menu does NOT have focus after Focus() call!");
            // Try one more time
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            g_prismaUI->Focus(g_masterMenuView, true, false);
            if (g_prismaUI->HasFocus(g_masterMenuView)) {
                logger::info("[PrismaUIBridge] Master menu focus succeeded on retry");
            }
        }
        
        // Mark as visible
        g_masterMenuVisible.store(true);
    }

    void HideMasterMenu() {
        if (!g_prismaUI || !g_masterMenuCreated.load()) {
            return;
        }

        // Unfocus first if focused
        if (g_prismaUI->HasFocus(g_masterMenuView)) {
            g_prismaUI->Unfocus(g_masterMenuView);
        }

        logger::info("[PrismaUIBridge] Hiding master menu panel");
        g_prismaUI->Hide(g_masterMenuView);
        
        // Mark as hidden
        g_masterMenuVisible.store(false);
    }

    bool IsMasterMenuVisible() {
        if (!g_prismaUI || !g_masterMenuCreated.load()) {
            return false;
        }
        return !g_prismaUI->IsHidden(g_masterMenuView);
    }

}
