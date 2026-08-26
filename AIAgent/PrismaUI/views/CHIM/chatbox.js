/**
 * CHIM Chatbox JavaScript
 * Handles MMO-style chat interface and real-time updates
 * Focus text composition is handled by a movable modal
 */

(function() {
    'use strict';

    // DOM Elements
    const chatboxRoot = document.getElementById('chim-chatbox');
    const chatboxViewerElement = document.getElementById('chim-chatbox-viewer');
    const tabButtons = document.querySelectorAll('.tab-button');
    const tabPanes = document.querySelectorAll('.tab-pane');
    const focusModal = document.getElementById('focus-chatbox-modal');
    const focusShellElement = document.querySelector('.focus-chatbox-shell');
    const focusInput = document.getElementById('focus-chatbox-input');
    const playerMoodInputs = document.querySelectorAll('input[name="chatbox-player-mood"]');
    const currentTargetElement = document.getElementById('chatbox-current-target');
    const targetsListElement = document.getElementById('chatbox-targets-list');
    const currentModeElement = document.getElementById('chatbox-current-mode');
    const modeMenuToggleButton = document.getElementById('chatbox-mode-menu-toggle');
    const modeOptionsElement = document.getElementById('chatbox-mode-options');
    const modeOptionButtons = document.querySelectorAll('#chatbox-mode-options .chatbox-option-tile');
    const currentModelElement = document.getElementById('chatbox-current-model');
    const globalModelControlElement = document.getElementById('chatbox-global-model-control');
    const currentRechatModeElement = document.getElementById('chatbox-current-rechat-mode');
    const modelMenuToggleButton = document.getElementById('chatbox-model-menu-toggle');
    const modelOptionsElement = document.getElementById('chatbox-model-options');
    const modelOptionButtons = document.querySelectorAll('#chatbox-model-options .chatbox-option-tile');
    const profileMenuToggleButton = document.getElementById('chatbox-profile-menu-toggle');
    const profileMenuElement = document.getElementById('chatbox-profile-menu');
    const profileMenuCloseButton = document.getElementById('chatbox-profile-menu-close');
    const profileNameElement = document.getElementById('chatbox-profile-name');
    const profileModeElement = document.getElementById('chatbox-profile-mode');
    const profileTargetElement = document.getElementById('chatbox-profile-target');
    const profileSlotElement = document.getElementById('chatbox-profile-slot');
    const profileSelectElement = document.getElementById('chatbox-profile-select');
    const profileAssignmentHintElement = document.getElementById('chatbox-profile-assignment-hint');
    const profileRandomToggleButton = document.getElementById('chatbox-profile-random-toggle');
    const profileDefaultToggleButtons = document.querySelectorAll('.profile-default-toggle[data-profile-setting]');
    const profileConnectorsElement = document.getElementById('chatbox-profile-connectors');
    const rechatMenuToggleButton = document.getElementById('chatbox-rechat-menu-toggle');
    const rechatOptionsElement = document.getElementById('chatbox-rechat-options');
    const rechatOptionButtons = document.querySelectorAll('#chatbox-rechat-options .chatbox-option-tile');
    const focusPositionButtons = document.querySelectorAll('.focus-chatbox-position-btn');
    const deleteEventSelect = document.getElementById('chatbox-delete-events-select');
    const deleteEventConfirmButton = document.getElementById('chatbox-delete-events-confirm');
    const storyLogElement = document.getElementById('focus-chatbox-story-log');
    const storyEmptyElement = document.getElementById('focus-chatbox-story-empty');
    const storyNewEventsButton = document.getElementById('focus-chatbox-story-new');
    const contextPanelElement = chatboxRoot;
    const contextToggleButton = document.getElementById('focus-chatbox-context-toggle');

    // State
    let currentTab = 'chat';
    const maxStoryEntries = 150;
    const liveStoryDedupeWindowMs = 15000;
    const recentStoryRetentionMs = 60000;
    const focusPositionStorageKey = 'chim_focus_chat_position';
    const contextCollapsedStorageKey = 'chim_recent_context_collapsed';
    const focusPositionClasses = ['focus-position-center', 'focus-position-top', 'focus-position-bottom'];
    let isChatFocused = false;
    let quickChatMode = false;
    let currentMode = 'STANDARD';
    let currentModeAction = 'mode_standard';
    let currentModelAction = 'llm_standard';
    let currentGlobalModelLabel = 'Standard';
    let currentProfileLlmMode = 'fixed';
    let currentProfileLlmInfo = null;
    let profileLlmTargetKey = '';
    let profileLlmRequestSequence = 0;
    let profileLlmSaveInProgress = false;
    let profileDefaultSaveInProgress = false;
    let profileAssignmentInProgress = false;
    let currentRechatMode = 'random';
    let rechatModeSaveInProgress = false;
    let currentFocusPosition = 'center';
    let currentTargetName = '';
    let currentTargetFormId = 0;
    let currentTargetIsNarrator = false;
    let currentTargetOverrideActive = false;
    let currentTargetOverrideMode = 'auto';
    let pendingDeleteCount = 0;
    let pendingDeleteConfirmTimeoutId = null;
    const targetRowsByKey = new Map();
    const storyEntryKeys = new Set();
    const recentStoryContent = new Map();
    let narratorStoryName = 'The Narrator';
    
    // Server URL
    let serverUrl = window.CHIM_SERVER_URL || 'http://192.168.169.218:8081/HerikaServer';

    const modeConfig = {
        STANDARD: { label: 'Standard', class: 'standard', action: 'mode_standard' },
        WHISPER: { label: 'Whisper', class: 'whisper', action: 'mode_whisper' },
        CLOSE: { label: 'Close', class: 'close', action: 'mode_close' },
        SHOUT: { label: 'Shout', class: 'shout', action: 'mode_shout' },
        NARRATOR: { label: 'Narrator', class: 'narrator', action: 'mode_narrator' },
        DIRECTOR: { label: 'Director', class: 'director', action: 'mode_director' },
        CHEATMODE: { label: 'Cheat Mode', class: 'cheatmode', action: 'mode_cheat' },
        AUTOCHAT: { label: 'Auto Chat', class: 'autochat', action: 'mode_autochat' },
        INJECTION_LOG: { label: 'Event Inject', class: 'director', action: 'mode_inject_log' },
        INJECTION_CHAT: { label: 'Inject & Chat', class: 'director', action: 'mode_inject_chat' }
    };

    const symbolModeRules = [
        { prefix: '((', mode: 'INJECTION_LOG', display: '(…)' },
        { prefix: '||', mode: 'CLOSE' },
        { prefix: '!!', mode: 'SHOUT' },
        { prefix: '**', mode: 'AUTOCHAT' },
        { prefix: '|', mode: 'WHISPER' },
        { prefix: '@', mode: 'NARRATOR' },
        { prefix: '>', mode: 'DIRECTOR' },
        { prefix: '#', mode: 'CHEATMODE' },
        { prefix: '(', mode: 'INJECTION_CHAT', display: '…' }
    ];

    function detectSymbolMode(message) {
        return symbolModeRules.find(function(rule) {
            return String(message || '').startsWith(rule.prefix);
        }) || null;
    }

    // Preview the request-local symbol mode while leaving the saved selector state unchanged.
    function renderModeIndicator() {
        if (!currentModeElement) return;
        const symbolMode = detectSymbolMode(focusInput ? focusInput.value : '');
        const effectiveMode = symbolMode ? symbolMode.mode : currentMode;
        const config = modeConfig[effectiveMode] || modeConfig.STANDARD;
        currentModeElement.className = 'mode-badge ' + config.class;
        currentModeElement.textContent = symbolMode
            ? `${config.label} (${symbolMode.display || symbolMode.prefix})`
            : config.label;
        currentModeElement.title = symbolMode
            ? `One-shot ${config.label}; saved mode remains ${modeConfig[currentMode].label}.`
            : (config.label === 'Close' ? 'Private, close-range conversation' : '');
    }

    const modelConfig = {
        standard: { label: 'Standard', class: 'standard', action: 'llm_standard' },
        fast: { label: 'Fast', class: 'fast', action: 'llm_fast' },
        powerful: { label: 'Powerful', class: 'powerful', action: 'llm_powerful' },
        experimental: { label: 'Experimental', class: 'experimental', action: 'llm_experimental' }
    };

    const rechatModeConfig = {
        tight: { label: 'Tight', class: 'tight' },
        conversational: { label: 'Conversational', class: 'conversational' },
        group: { label: 'Group', class: 'group' },
        random: { label: 'Random', class: 'random' }
    };

    function decodeHtmlEntities(value) {
        const decoder = document.createElement('textarea');
        decoder.innerHTML = String(value || '');
        return decoder.value;
    }

    function isStoryAtBottom() {
        if (!storyLogElement) return true;
        return storyLogElement.scrollHeight - storyLogElement.scrollTop - storyLogElement.clientHeight < 48;
    }

    function scrollStoryToBottom() {
        if (!storyLogElement) return;
        storyLogElement.scrollTop = storyLogElement.scrollHeight;
        if (storyNewEventsButton) storyNewEventsButton.classList.add('hidden');
    }

    function scrollStoryToBottomAfterLayout() {
        const defer = typeof window.requestAnimationFrame === 'function'
            ? window.requestAnimationFrame.bind(window)
            : function(callback) { window.setTimeout(callback, 0); };
        defer(function() {
            defer(scrollStoryToBottom);
        });
    }

    window.scrollStandaloneContext = function(deltaY) {
        if (!storyLogElement || !contextPanelElement || !chatboxViewerElement) return;
        if (contextPanelElement.parentElement !== chatboxViewerElement) return;

        const delta = Number(deltaY);
        if (!Number.isFinite(delta)) return;
        storyLogElement.scrollTop += Math.max(-1200, Math.min(1200, delta));
    };

    function showStoryEmpty(message) {
        if (!storyEmptyElement || !storyLogElement) return;
        storyEmptyElement.textContent = message || 'No recent context.';
        storyEmptyElement.classList.toggle('hidden', storyLogElement.children.length > 0);
    }

    function pruneRecentStoryContent(now) {
        recentStoryContent.forEach(function(recent, key) {
            if (!recent || now - recent.seenAt > recentStoryRetentionMs) {
                recentStoryContent.delete(key);
            }
        });
    }

    function createStoryEntryElement(entry) {
        const row = document.createElement('div');
        row.className = 'story-entry ' + entry.kind + (entry.source === 'subtitle' ? ' non-llm' : '');
        if (entry.rowId > 0) {
            row.dataset.rowId = String(entry.rowId);
            row.classList.add('has-delete');
        }

        const time = document.createElement('span');
        time.className = 'story-entry-time';
        time.textContent = entry.timestamp || '';

        const line = document.createElement('div');
        line.className = 'story-entry-line';

        const speaker = document.createElement('span');
        speaker.className = 'story-entry-speaker';
        speaker.textContent = entry.speaker || '';

        const text = document.createElement('span');
        text.className = 'story-entry-text';
        text.textContent = entry.text || '';

        if (entry.speaker) line.appendChild(speaker);
        line.appendChild(text);
        row.appendChild(time);
        row.appendChild(line);
        if (entry.rowId > 0) {
            row.appendChild(createStoryDeleteButton(entry.rowId));
        }
        return row;
    }

    function createStoryDeleteButton(rowId) {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'story-entry-delete';
        button.textContent = '\u{1F5D1}';
        button.title = 'Delete this event';
        button.setAttribute('aria-label', 'Delete this event');
        button.addEventListener('click', function(event) {
            event.preventDefault();
            event.stopPropagation();
            confirmAndDeleteEvent(button, rowId);
        });
        return button;
    }

    async function confirmAndDeleteEvent(button, rowId) {
        if (button.disabled) return;
        if (!button.classList.contains('confirm-delete')) {
            button.classList.add('confirm-delete');
            button.title = 'Click again to delete';
            setTimeout(function() {
                button.classList.remove('confirm-delete');
                button.title = 'Delete this event';
            }, 3000);
            return;
        }

        button.disabled = true;
        try {
            const formData = new FormData();
            formData.append('rowid', String(rowId));
            const response = await fetch(`${serverUrl}/ui/cmd/action_delete_event.php`, {
                method: 'POST',
                body: formData,
                cache: 'no-store'
            });
            const result = await response.json();
            if (!response.ok || !result.ok) {
                throw new Error(result.message || 'Failed to delete event.');
            }
            window.removeEventLogEntry(rowId);
            sendControlCommand('event_deleted|' + rowId);
        } catch (error) {
            button.disabled = false;
            button.classList.remove('confirm-delete');
            button.title = 'Delete this event';
            pushChatboxSystemMessage(error.message || 'Failed to delete event.');
        }
    }

    window.setChatboxServerUrl = function(url) {
        const normalized = String(url || '').replace(/\/$/, '');
        if (normalized) serverUrl = normalized;
    };

    window.removeEventLogEntry = function(rowId) {
        const normalizedRowId = Number(rowId || 0);
        if (!storyLogElement || normalizedRowId <= 0) return;
        const rowKey = 'row:' + normalizedRowId;
        const row = storyLogElement.querySelector(`[data-row-id="${normalizedRowId}"]`);
        if (row) row.remove();
        storyEntryKeys.delete(rowKey);
        recentStoryContent.forEach(function(recent, key) {
            if (recent && recent.entry && Number(recent.entry.rowId || 0) === normalizedRowId) {
                recentStoryContent.delete(key);
            }
        });
        showStoryEmpty();
    };

    function appendStoryEntry(entry, isLive) {
        if (!storyLogElement || !entry) return false;

        const rowKey = entry.rowId > 0 ? 'row:' + entry.rowId : '';
        if (rowKey && storyEntryKeys.has(rowKey)) return false;

        const now = Date.now();
        pruneRecentStoryContent(now);
        const recent = recentStoryContent.get(entry.contentKey);
        if (!isLive && recent) {
            const matchesLiveEntry = recent.isLive &&
                now - recent.seenAt <= liveStoryDedupeWindowMs;
            const matchesPersistedEntry = !recent.isLive &&
                window.ChimStoryLog.isPersistedDuplicate(recent.entry, entry);
            if (matchesLiveEntry || matchesPersistedEntry) {
                if (rowKey) storyEntryKeys.add(rowKey);
                return false;
            }
        }
        recentStoryContent.set(entry.contentKey, {
            entry: entry,
            isLive: Boolean(isLive),
            seenAt: now
        });

        const shouldFollow = isStoryAtBottom();
        const row = createStoryEntryElement(entry);
        if (rowKey) {
            row.dataset.entryKey = rowKey;
            storyEntryKeys.add(rowKey);
        }
        storyLogElement.appendChild(row);

        while (storyLogElement.children.length > maxStoryEntries) {
            const first = storyLogElement.firstElementChild;
            if (first && first.dataset.entryKey) storyEntryKeys.delete(first.dataset.entryKey);
            storyLogElement.removeChild(first);
        }

        showStoryEmpty();
        if (shouldFollow) {
            scrollStoryToBottom();
        } else if (storyNewEventsButton) {
            storyNewEventsButton.classList.remove('hidden');
        }
        return true;
    }

    function resetStoryLog() {
        if (!storyLogElement) return;
        storyLogElement.innerHTML = '';
        storyEntryKeys.clear();
        recentStoryContent.clear();
        if (storyNewEventsButton) storyNewEventsButton.classList.add('hidden');
    }

    function loadContextCollapsed() {
        try {
            return localStorage.getItem(contextCollapsedStorageKey) === 'true';
        } catch (_err) {
            return false;
        }
    }

    function applyContextCollapsed(collapsed) {
        if (!contextPanelElement || !contextToggleButton) return;
        contextPanelElement.classList.toggle('collapsed', collapsed);
        contextToggleButton.textContent = collapsed ? '+' : '\u2212';
        contextToggleButton.setAttribute('aria-expanded', collapsed ? 'false' : 'true');
        contextToggleButton.title = collapsed ? 'Expand recent context' : 'Minimize recent context';
        try {
            localStorage.setItem(contextCollapsedStorageKey, collapsed ? 'true' : 'false');
        } catch (_err) {
            // Keep the current session state when storage is unavailable.
        }
    }

    function setContextPlacement(focused) {
        if (!contextPanelElement || !chatboxViewerElement || !focusShellElement) return;

        const destination = focused ? focusShellElement : chatboxViewerElement;
        if (contextPanelElement.parentElement !== destination) {
            destination.appendChild(contextPanelElement);
        }
        chatboxViewerElement.classList.toggle('context-attached', focused);
        if (focused) {
            applyContextCollapsed(loadContextCollapsed());
        } else {
            contextPanelElement.classList.remove('collapsed');
        }
    }

    window.updateStoryLog = function(jsonString, replaceExisting) {
        if (!window.ChimStoryLog || !storyLogElement) return;
        try {
            const payload = JSON.parse(jsonString);
            if (!payload || payload.success !== true || !Array.isArray(payload.data)) {
                showStoryEmpty('Recent context is unavailable.');
                return;
            }

            narratorStoryName = decodeHtmlEntities(payload.narrator_name || narratorStoryName) || 'The Narrator';
            const normalized = window.ChimStoryLog.normalizeEntries(
                payload.data,
                narratorStoryName,
                decodeHtmlEntities
            );

            if (replaceExisting) resetStoryLog();
            normalized.forEach(function(entry) {
                appendStoryEntry(entry, false);
            });

            showStoryEmpty('No recent context.');
            if (replaceExisting) scrollStoryToBottom();
        } catch (error) {
            console.error('[Chatbox] Failed to update story log:', error);
            showStoryEmpty('Recent context is unavailable.');
        }
    };

    window.setStoryLogUnavailable = function() {
        showStoryEmpty('Recent context is unavailable.');
    };

    function setActiveTile(buttons, attribute, value) {
        buttons.forEach(function(button) {
            const active = button.dataset[attribute] === value;
            button.classList.toggle('is-active', active);
            button.setAttribute('aria-selected', active ? 'true' : 'false');
        });
    }

    function setTileSelectorDisabled(toggleButton, optionButtons, disabled, title) {
        if (toggleButton) {
            toggleButton.disabled = disabled;
            if (title) toggleButton.title = title;
        }
        optionButtons.forEach(function(button) {
            button.disabled = disabled;
        });
    }

    function closeTileMenu(toggleButton, optionsElement) {
        if (toggleButton) toggleButton.setAttribute('aria-expanded', 'false');
        if (optionsElement) optionsElement.classList.add('hidden');
    }

    function closeAllTileMenus(exceptOptionsElement) {
        [
            [modeMenuToggleButton, modeOptionsElement],
            [modelMenuToggleButton, modelOptionsElement],
            [rechatMenuToggleButton, rechatOptionsElement]
        ].forEach(function(selector) {
            if (selector[1] !== exceptOptionsElement) {
                closeTileMenu(selector[0], selector[1]);
            }
        });
    }

    function toggleTileMenu(toggleButton, optionsElement) {
        if (!toggleButton || !optionsElement || toggleButton.disabled) return;
        const opening = optionsElement.classList.contains('hidden');
        closeAllTileMenus(opening ? optionsElement : null);
        toggleButton.setAttribute('aria-expanded', opening ? 'true' : 'false');
        optionsElement.classList.toggle('hidden', !opening);
    }

    /**
     * Switch between available chatbox tabs.
     */
    window.switchTab = function(tabName) {
        currentTab = tabName;
        tabButtons.forEach(btn => btn.classList.toggle('active', btn.dataset.tab === tabName));
        tabPanes.forEach(pane => pane.classList.toggle('active', pane.id === 'tab-' + tabName));
    };

    /**
     * Push a new chat message (called from C++ via Invoke)
     */
    window.pushChatMessage = function(speaker, text, timestamp, type, source) {
        if (!speaker || !text) return;
        type = type || 'npc';
        source = source || 'llm';
        timestamp = timestamp || getCurrentTime();

        if (window.ChimStoryLog) {
            appendStoryEntry(
                window.ChimStoryLog.normalizeLiveMessage(speaker, text, timestamp, type, source),
                true
            );
        }
    };

    /**
     * Push a system log entry (called from C++ via Invoke)
     */
    window.pushSystemLog = function(level, message, timestamp) {
        // System tab removed from chatbox view. Keep bridge hook as a harmless no-op.
    }

    /**
     * Send user message through Prisma bridge
     */
    function normalizePlayerMood(mood) {
        return ['happy', 'sad', 'angry', 'annoyed', 'scared', 'surprised', 'confused', 'suspicious', 'playful', 'flirty'].indexOf(mood) >= 0 ? mood : '';
    }

    function getSelectedPlayerMood() {
        const selected = Array.prototype.find.call(playerMoodInputs, function(input) {
            return input.checked;
        });
        return normalizePlayerMood(selected ? selected.value : '');
    }

    function resetPlayerMood() {
        playerMoodInputs.forEach(function(input) {
            input.checked = input.value === '';
        });
    }

    function sendMessageToBridge(message, playerMood) {
        if (!message || !message.trim()) return;
        if (window.chimChatboxCommand) {
            const mood = normalizePlayerMood(playerMood);
            window.chimChatboxCommand(mood ? 'send_mood|' + mood + '|' + message : 'send|' + message);
        }
    }

    function sendControlCommand(command) {
        if (window.chimChatboxCommand) {
            window.chimChatboxCommand(command);
        }
    }

    function showInGameDebugNotification(message) {
        if (!message || !message.trim()) return;
        sendControlCommand('debug_notify|' + message);
    }

    function pushChatboxSystemMessage(message) {
        if (!message || !message.trim()) return;
        window.pushChatMessage('CHIM', message, getCurrentTime(), 'system');
    }

    function escapeHtml(text) {
        if (!text) return '';
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }

    function setHtmlIfChanged(element, html) {
        if (!element || element.__chimLastHtml === html) return false;
        element.innerHTML = html;
        element.__chimLastHtml = html;
        return true;
    }

    function setTextIfChanged(element, text) {
        if (!element || element.__chimLastText === text) return false;
        element.textContent = text;
        element.__chimLastText = text;
        return true;
    }

    function setClassNameIfChanged(element, className) {
        if (!element || element.className === className) return false;
        element.className = className;
        return true;
    }

    function setDatasetValue(element, key, value) {
        if (!element) return;
        if (value === null || value === undefined || value === false) {
            delete element.dataset[key];
            return;
        }

        const text = String(value);
        if (element.dataset[key] !== text) {
            element.dataset[key] = text;
        }
    }

    function makeTargetRow(isEmpty) {
        const row = document.createElement(isEmpty ? 'div' : 'button');
        if (isEmpty) {
            row.className = 'chatbox-target-empty';
            return row;
        }

        row.type = 'button';
        row.className = 'chatbox-target-item';
        row.__chimMetaElement = document.createElement('span');
        row.__chimMetaElement.className = 'chatbox-target-meta';
        row.__chimNameElement = document.createElement('span');
        row.__chimNameElement.className = 'chatbox-target-name';
        row.__chimDistanceElement = document.createElement('span');
        row.__chimDistanceElement.className = 'chatbox-target-distance';
        row.__chimMetaElement.appendChild(row.__chimNameElement);
        row.appendChild(row.__chimMetaElement);
        row.appendChild(row.__chimDistanceElement);
        return row;
    }

    function updateTargetRow(row, spec) {
        if (spec.empty) {
            setClassNameIfChanged(row, 'chatbox-target-empty');
            setTextIfChanged(row, spec.message);
            return;
        }

        setClassNameIfChanged(row, spec.className);
        setTextIfChanged(row.__chimNameElement, spec.name);
        setTextIfChanged(row.__chimDistanceElement, spec.distance);
        setDatasetValue(row, 'auto', spec.auto ? 'true' : null);
        setDatasetValue(row, 'everyone', spec.everyone ? 'true' : null);
        setDatasetValue(row, 'formId', spec.formId);
        setDatasetValue(row, 'targetName', spec.targetName);
    }

    function syncTargetRows(specs) {
        if (!targetsListElement) return;
        const seen = new Set();
        specs.forEach(function(spec, index) {
            seen.add(spec.key);
            let row = targetRowsByKey.get(spec.key);
            if (!row) {
                row = makeTargetRow(!!spec.empty);
                targetRowsByKey.set(spec.key, row);
            }

            updateTargetRow(row, spec);
            if (targetsListElement.children[index] !== row) {
                targetsListElement.insertBefore(row, targetsListElement.children[index] || null);
            }
        });

        targetRowsByKey.forEach(function(row, key) {
            if (!seen.has(key)) {
                row.remove();
                targetRowsByKey.delete(key);
            }
        });
    }

    function updateFocusPositionButtons() {
        focusPositionButtons.forEach(function(button) {
            const isActive = button.dataset.position === currentFocusPosition;
            button.classList.toggle('active', isActive);
            button.setAttribute('aria-pressed', isActive ? 'true' : 'false');
        });
    }

    function normalizeFocusPosition(position) {
        if (position === 'top' || position === 'bottom') {
            return position;
        }
        return 'center';
    }

    function loadFocusPosition() {
        try {
            const storedPosition = localStorage.getItem(focusPositionStorageKey);
            if (storedPosition === null) {
                return currentFocusPosition;
            }
            return normalizeFocusPosition(storedPosition);
        } catch (_err) {
            return currentFocusPosition;
        }
    }

    function saveFocusPosition(position) {
        try {
            localStorage.setItem(focusPositionStorageKey, position);
        } catch (_err) {
            // Ignore storage failures and fall back to the in-memory selection.
        }
    }

    function applyFocusPosition(position) {
        if (!focusModal) return;
        currentFocusPosition = normalizeFocusPosition(position);
        focusPositionClasses.forEach(function(className) {
            focusModal.classList.remove(className);
        });
        focusModal.classList.add('focus-position-' + currentFocusPosition);
        updateFocusPositionButtons();
    }

    function normalizeDeleteEventCount(count) {
        const deleteCount = Number(count || 0);
        return [5, 10, 20, 50, 100].includes(deleteCount) ? deleteCount : 0;
    }

    function setDeleteEventControlsBusy(isBusy) {
        if (deleteEventSelect) {
            deleteEventSelect.disabled = !!isBusy;
        }
        if (deleteEventConfirmButton) {
            deleteEventConfirmButton.disabled = !!isBusy;
        }
    }

    function clearPendingDeleteConfirmation() {
        pendingDeleteCount = 0;
        if (pendingDeleteConfirmTimeoutId) {
            window.clearTimeout(pendingDeleteConfirmTimeoutId);
            pendingDeleteConfirmTimeoutId = null;
        }

        if (deleteEventConfirmButton) {
            deleteEventConfirmButton.textContent = 'Delete';
            deleteEventConfirmButton.title = 'Delete the selected number of recent events';
        }
    }

    function armDeleteConfirmation(deleteCount) {
        clearPendingDeleteConfirmation();
        pendingDeleteCount = deleteCount;
        if (deleteEventConfirmButton) {
            deleteEventConfirmButton.textContent = 'Are you sure?';
            deleteEventConfirmButton.title = 'Press again to delete the selected events';
        }
        pendingDeleteConfirmTimeoutId = window.setTimeout(function() {
            clearPendingDeleteConfirmation();
        }, 5000);
    }

    function resetTargetSelectionForModeChange() {
        if (!targetsListElement) {
            sendControlCommand('target_override_clear');
            return;
        }

        const hasAutoTarget = !!targetsListElement.querySelector('.chatbox-target-item[data-auto="true"]');
        if (hasAutoTarget) {
            sendControlCommand('target_override_clear');
            return;
        }

        const hasEveryoneTarget = !!targetsListElement.querySelector('.chatbox-target-item[data-everyone="true"]');
        if (hasEveryoneTarget) {
            sendControlCommand('target_override_everyone');
            return;
        }

        sendControlCommand('target_override_clear');
    }

    /**
     * Close chatbox
     */
    window.closeChat = function() {
        window.closeFocusChatbox(false);
        isChatFocused = false;
        quickChatMode = false;
        if (window.chimChatboxCommand) {
            window.chimChatboxCommand('close');
        }
    };

    /**
     * Prepare the quick text chat mode before the Prisma view is shown/focused.
     * This keeps the tabbed chat/history viewer hidden so only the modal is exposed.
     */
    window.prepareQuickChatFocus = function() {
        quickChatMode = true;
        currentTab = 'chat';
        setContextPlacement(true);
        if (focusModal) {
            focusModal.classList.add('hidden');
            focusModal.setAttribute('aria-hidden', 'true');
        }
        if (focusInput) {
            focusInput.value = '';
            focusInput.blur();
            renderModeIndicator();
        }
        window.switchTab('chat');
    };

    /**
     * Open focus chat modal
     */
    window.openFocusChatbox = function() {
        if (!focusModal || !focusInput) return;
        applyFocusPosition(loadFocusPosition());
        setContextPlacement(true);
        focusModal.classList.remove('hidden');
        focusModal.setAttribute('aria-hidden', 'false');
        scrollStoryToBottomAfterLayout();
        if (storyLogElement && storyLogElement.children.length === 0) {
            showStoryEmpty('Loading recent context...');
        }
        focusInput.value = '';
        resetPlayerMood();
        renderModeIndicator();
        setTimeout(function() {
            focusInput.focus();
            focusInput.selectionStart = focusInput.value.length;
            focusInput.selectionEnd = focusInput.value.length;
        }, 0);
    };

    /**
     * Close focus chat modal
     */
    window.closeFocusChatbox = function(notifyBridge) {
        if (!focusModal || !focusInput) return;
        const shouldNotifyBridge = notifyBridge !== false;
        focusModal.classList.add('hidden');
        focusModal.setAttribute('aria-hidden', 'true');
        focusInput.value = '';
        resetPlayerMood();
        renderModeIndicator();
        focusInput.blur();
        setContextPlacement(false);
        if (shouldNotifyBridge && window.chimChatboxCommand) {
            if (quickChatMode) {
                window.chimChatboxCommand('close');
            } else {
                window.chimChatboxCommand('unfocus');
            }
        }
    };

    /**
     * Send message from focus chat modal
     */
    window.sendFocusMessage = function() {
        if (!focusInput) return;
        const message = focusInput.value;
        if (!message.trim()) return;
        sendMessageToBridge(message, getSelectedPlayerMood());
        focusInput.value = '';
        renderModeIndicator();
        window.closeFocusChatbox(true);
    };

    window.clearFocusMessage = function() {
        if (!focusInput) return;
        focusInput.value = '';
        resetPlayerMood();
        renderModeIndicator();
        focusInput.focus();
    };

    window.triggerContinueSpeaking = function() {
        if (!currentTargetName) {
            sendControlCommand('continue_chat');
            return;
        }

        sendControlCommand('continue_chat');
        window.closeFocusChatbox(true);
    };

    window.triggerStopAllDialogue = function() {
        sendControlCommand('stop_all_dialogue');
    };

    window.triggerHaltAIActions = function() {
        sendControlCommand('halt_ai_actions');
    };

    window.deleteRecentEvents = async function(count) {
        const deleteCount = normalizeDeleteEventCount(count);
        if (!deleteCount) {
            clearPendingDeleteConfirmation();
            return;
        }

        setDeleteEventControlsBusy(true);
        try {
            const formData = new FormData();
            formData.append('count', String(deleteCount));

            const response = await fetch(`${serverUrl}/ui/cmd/action_delete_recent_events.php`, {
                method: 'POST',
                body: formData,
                cache: 'no-store'
            });

            let result = null;
            try {
                result = await response.json();
            } catch (_err) {
                result = null;
            }

            if (!response.ok || !result || !result.ok) {
                const errorMessage = (result && result.message) ? result.message : `Failed to delete the last ${deleteCount} events.`;
                pushChatboxSystemMessage(errorMessage);
                return;
            }

            const deletedCount = Number(result.deleted_count || 0);
            pushChatboxSystemMessage(`Deleted ${deletedCount} latest visible event${deletedCount === 1 ? '' : 's'}.`);
            showInGameDebugNotification(`Deleted last ${deletedCount} events`);
            sendControlCommand('story_refresh');
        } catch (_err) {
            pushChatboxSystemMessage(`Failed to delete the last ${deleteCount} events.`);
        } finally {
            setDeleteEventControlsBusy(false);
            clearPendingDeleteConfirmation();
        }
    };

    if (focusInput) {
        focusInput.addEventListener('input', renderModeIndicator);
        focusInput.addEventListener('keydown', function(e) {
            if (e.key === 'Escape') {
                e.preventDefault();
                if (isProfileMenuOpen()) {
                    closeProfileMenu();
                    return;
                }
                window.closeFocusChatbox(true);
                return;
            }

            if (e.key === 'Enter' && !e.shiftKey) {
                e.preventDefault();
                if (e.ctrlKey && currentModeAction !== 'mode_close') {
                    resetTargetSelectionForModeChange();
                    sendControlCommand('mode_close');
                }
                window.sendFocusMessage();
            }
        });
    }

    window.onChatboxShown = function() {
        setContextPlacement(false);
        scrollStoryToBottomAfterLayout();
    };

    /**
     * Called when chatbox gains focus from C++
     */
    window.onChatboxFocused = function(quickChat) {
        isChatFocused = true;
        quickChatMode = !!quickChat;
        refreshProfileLlmMode(true);
        setContextPlacement(true);
        window.openFocusChatbox();
    };

    /**
     * Called when chatbox loses focus from C++
     */
    window.onChatboxUnfocused = function() {
        const wasQuickChatMode = quickChatMode;
        isChatFocused = false;
        quickChatMode = false;
        window.closeFocusChatbox(false);
        if (!wasQuickChatMode) {
            setContextPlacement(false);
        }
    };

    window.updateChatboxTarget = function(name, distance) {
        if (!currentTargetElement) return;
        currentTargetName = name || '';
        const suffix = currentTargetOverrideActive ? '<span class="target-distance">(Override)</span>' : '';
        if (currentTargetOverrideMode === 'everyone') {
            setHtmlIfChanged(currentTargetElement, `
                <span class="target-name">Everyone</span>
                <span class="target-distance">(Broadcast)</span>
                ${suffix}
            `);
        } else if (name && name !== '') {
            setHtmlIfChanged(currentTargetElement, `
                <span class="target-name">${escapeHtml(name)}</span>
                <span class="target-distance">(${distance.toFixed(1)}m)</span>
                ${suffix}
            `);
        } else {
            setHtmlIfChanged(currentTargetElement, '<span class="target-name no-target">No target</span>');
        }
    };

    window.updateChatboxTargets = function(payloadJson) {
        // Keep spatial targets live while focused so the quick chat UI reflects
        // the current audience snapshot. Scroll position is preserved below to
        // avoid jumpiness while the player is selecting a target.

        let payload = null;
        try {
            payload = JSON.parse(payloadJson);
        } catch (_err) {
            return;
        }

        const targets = Array.isArray(payload.targets) ? payload.targets : [];
        currentTargetOverrideActive = !!payload.override_active;
        currentTargetOverrideMode = payload.override_mode || 'auto';
        currentTargetFormId = Number(payload.active_form_id || 0);
        currentTargetName = payload.active_name || '';
        const previousScrollTop = targetsListElement ? targetsListElement.scrollTop : 0;

        const specs = [];
        if (payload.show_auto) {
            specs.push({
                key: 'mode:auto',
                className: `chatbox-target-item auto-target ${payload.auto_active ? 'active' : ''}`,
                name: 'Auto',
                distance: 'Mode',
                auto: true
            });
        }
        if (payload.show_everyone) {
            specs.push({
                key: 'mode:everyone',
                className: `chatbox-target-item everyone-target ${payload.everyone_active ? 'active' : ''}`,
                name: 'Everyone',
                distance: 'Broadcast',
                everyone: true
            });
        }

        const visibleTargets = targets.filter(function(target) {
            if (target.override) return true;
            return target.targetable !== false;
        });
        visibleTargets.forEach(function(target) {
            const formId = Number(target.form_id || 0);
            const itemClasses = ['chatbox-target-item'];
            if (target.active) itemClasses.push('active');
            if (target.override) itemClasses.push('override');
            const statusLabel = target.narrator ? 'Narrator' : `${Number(target.distance || 0).toFixed(1)}m`;
            const name = target.name || 'Unknown Target';
            specs.push({
                key: formId ? `form:${formId}` : `name:${name}`,
                className: itemClasses.join(' '),
                name,
                distance: statusLabel,
                formId,
                targetName: target.name || ''
            });
        });

        if (targetsListElement) {
            syncTargetRows(specs);
            targetsListElement.scrollTop = previousScrollTop;
        }
        let activeTarget = null;
        if (currentTargetOverrideMode !== 'everyone') {
            if (currentTargetFormId) {
                activeTarget = targets.find(function(target) {
                    return Number(target.form_id || 0) === currentTargetFormId;
                }) || null;
            }
            if (!activeTarget && currentTargetName) {
                activeTarget = targets.find(function(target) {
                    return (target.name || '') === currentTargetName;
                }) || null;
            }
        }
        currentTargetIsNarrator = !!(activeTarget && activeTarget.narrator);
        window.updateChatboxTarget(currentTargetName, Number(activeTarget ? activeTarget.distance || 0 : 0));
        refreshProfileLlmMode();
    };

    window.updateChatboxMode = function(mode) {
        const modeUpper = mode ? mode.toUpperCase().trim() : 'STANDARD';
        currentMode = modeConfig[modeUpper] ? modeUpper : 'STANDARD';
        const config = modeConfig[currentMode];

        currentModeAction = config.action;
        renderModeIndicator();
        setActiveTile(modeOptionButtons, 'action', config.action);
        refreshProfileLlmMode();
    };

    window.updateChatboxModel = function(modelLabel) {
        const labelLower = modelLabel ? modelLabel.toLowerCase().trim() : 'standard';
        const config = modelConfig[labelLower] || modelConfig.standard;
        currentGlobalModelLabel = config.label;
        renderProfileLlmMode();
    };

    function renderProfileLlmMode() {
        const globalConfig = modelConfig[currentGlobalModelLabel.toLowerCase()] || modelConfig.standard;
        currentModelAction = globalConfig.action;
        if (currentModelElement) {
            currentModelElement.className = 'mode-badge ' + globalConfig.class;
            currentModelElement.textContent = globalConfig.label;
        }
        setActiveTile(modelOptionButtons, 'action', globalConfig.action);
        const modelDisabled = profileLlmSaveInProgress ||
            !!(currentProfileLlmInfo && currentProfileLlmMode === 'random');
        setTileSelectorDisabled(
            modelMenuToggleButton,
            modelOptionButtons,
            modelDisabled,
            currentProfileLlmMode === 'random'
                ? 'Disable Random LLM on the target profile to change the LLM model.'
                : 'Switch LLM model'
        );
        if (modelDisabled) closeTileMenu(modelMenuToggleButton, modelOptionsElement);
        if (globalModelControlElement) {
            globalModelControlElement.classList.toggle(
                'profile-random-muted',
                !!currentProfileLlmInfo && currentProfileLlmMode === 'random'
            );
        }

        renderProfileMenu();
    }

    function getProfileLlmTarget() {
        if (currentModeAction === 'mode_director' ||
            currentTargetOverrideMode === 'everyone') {
            return null;
        }
        if (currentModeAction === 'mode_narrator' || currentTargetIsNarrator) {
            return { type: 'narrator', name: '', key: 'narrator' };
        }
        if (!currentTargetName) return null;
        return {
            type: 'npc',
            name: currentTargetName,
            key: `npc:${currentTargetFormId || 0}:${currentTargetName}`
        };
    }

    function renderProfileMenu() {
        const profile = currentProfileLlmInfo;
        const hasProfile = !!profile;
        const isRandom = hasProfile && currentProfileLlmMode === 'random';
        const target = getProfileLlmTarget();

        if (profileMenuToggleButton) {
            profileMenuToggleButton.disabled = !hasProfile;
        }
        setTextIfChanged(profileNameElement, hasProfile ? profile.profile_name : 'No Profile');
        if (profileModeElement) {
            profileModeElement.className = 'profile-mode-dot ' + (isRandom ? 'random' : 'fixed');
            profileModeElement.textContent = isRandom ? 'Random' : 'Fixed';
        }
        setTextIfChanged(
            profileTargetElement,
            hasProfile ? `${profile.target_name} Profile` : 'No target selected'
        );
        const profileSlot = hasProfile ? Number(profile.profile_slot || 0) : 0;
        setTextIfChanged(
            profileSlotElement,
            hasProfile
                ? `${profile.profile_name} - ${profileSlot > 0 ? `Slot ${profileSlot}` : 'Not assigned to a slot'}`
                : 'No profile slot'
        );

        if (profileSelectElement) {
            const profiles = hasProfile && Array.isArray(profile.available_profiles)
                ? profile.available_profiles
                : [];
            profileSelectElement.replaceChildren();
            if (profiles.length === 0) {
                const option = document.createElement('option');
                option.value = '';
                option.textContent = hasProfile ? profile.profile_name : 'No profile available';
                profileSelectElement.appendChild(option);
            } else {
                const currentIsSlotted = profiles.some(function(item) {
                    return Number(item.profile_id) === Number(profile.profile_id);
                });
                if (!currentIsSlotted) {
                    const currentOption = document.createElement('option');
                    currentOption.value = '';
                    currentOption.textContent = `${profile.profile_name} (not assigned to a slot)`;
                    profileSelectElement.appendChild(currentOption);
                }
                profiles.forEach(function(item) {
                    const option = document.createElement('option');
                    option.value = String(item.slot);
                    option.dataset.profileId = String(item.profile_id);
                    option.textContent = `${item.slot}. ${item.profile_name}`;
                    option.selected = Number(item.profile_id) === Number(profile.profile_id);
                    profileSelectElement.appendChild(option);
                });
            }

            const canAssign = hasProfile && target && target.type === 'npc';
            profileSelectElement.disabled = !canAssign || profileAssignmentInProgress ||
                profileLlmSaveInProgress || profileDefaultSaveInProgress;
            if (canAssign) {
                setTextIfChanged(profileAssignmentHintElement, 'Changing this reassigns only the targeted NPC.');
            } else if (hasProfile && target && target.type === 'narrator') {
                setTextIfChanged(profileAssignmentHintElement, 'Narrator profile assignment is managed in the web UI.');
            } else {
                setTextIfChanged(profileAssignmentHintElement, 'Select a single NPC target to assign a profile.');
            }
        }

        const connectorCount = hasProfile ? Number(profile.configured_slot_count || 0) : 0;

        if (profileRandomToggleButton) {
            profileRandomToggleButton.classList.toggle('on', isRandom);
            profileRandomToggleButton.classList.toggle('off', !isRandom);
            profileRandomToggleButton.setAttribute('aria-pressed', isRandom ? 'true' : 'false');
            profileRandomToggleButton.disabled = !hasProfile || profileLlmSaveInProgress ||
                profileDefaultSaveInProgress || profileAssignmentInProgress ||
                (!isRandom && connectorCount === 0);
            setTextIfChanged(
                profileRandomToggleButton.querySelector('.profile-default-state'),
                isRandom ? 'ON' : 'OFF'
            );
        }

        const profileDefaults = hasProfile && profile.profile_defaults
            ? profile.profile_defaults
            : {};
        profileDefaultToggleButtons.forEach(function(button) {
            const setting = button.dataset.profileSetting || '';
            const enabled = !!profileDefaults[setting];
            const state = button.querySelector('.profile-default-state');
            button.classList.toggle('on', enabled);
            button.classList.toggle('off', !enabled);
            button.setAttribute('aria-pressed', enabled ? 'true' : 'false');
            button.disabled = !hasProfile || profileDefaultSaveInProgress ||
                profileLlmSaveInProgress || profileAssignmentInProgress;
            setTextIfChanged(state, enabled ? 'ON' : 'OFF');
        });

        if (profileConnectorsElement) {
            profileConnectorsElement.replaceChildren();
            const connectors = hasProfile && Array.isArray(profile.configured_connectors)
                ? profile.configured_connectors
                : [];
            if (connectors.length === 0) {
                const empty = document.createElement('div');
                empty.className = 'profile-connector-empty';
                empty.textContent = hasProfile
                    ? 'No LLM connectors are configured on this profile.'
                    : 'Select a target profile to view its connectors.';
                profileConnectorsElement.appendChild(empty);
            } else {
                connectors.forEach(function(connector) {
                    const row = document.createElement('div');
                    row.className = 'profile-connector-row';
                    const slot = document.createElement('div');
                    slot.className = 'profile-connector-slot';
                    slot.textContent = connector.label || `Slot ${connector.slot}`;
                    const name = document.createElement('div');
                    name.className = 'profile-connector-name';
                    name.textContent = connector.connector_name || `Connector ${connector.connector_id}`;
                    row.append(slot, name);
                    profileConnectorsElement.appendChild(row);
                });
            }
        }
    }

    async function refreshProfileLlmMode(force) {
        const target = getProfileLlmTarget();
        if (!target) {
            profileLlmTargetKey = '';
            currentProfileLlmInfo = null;
            currentProfileLlmMode = 'fixed';
            closeProfileMenu();
            renderProfileLlmMode();
            return null;
        }
        if (!force && target.key === profileLlmTargetKey) {
            return currentProfileLlmInfo;
        }

        if (target.key !== profileLlmTargetKey) {
            closeProfileMenu();
        }
        profileLlmTargetKey = target.key;
        currentProfileLlmInfo = null;
        currentProfileLlmMode = 'fixed';
        renderProfileLlmMode();
        const requestSequence = ++profileLlmRequestSequence;

        try {
            const params = new URLSearchParams({
                target_type: target.type,
                target_name: target.name
            });
            const response = await fetch(`${serverUrl}/ui/api/chim_profile_llm_mode.php?${params.toString()}`, {
                cache: 'no-store'
            });
            const result = await response.json();
            if (!response.ok || !result || !result.ok || !result.profile) {
                throw new Error((result && result.message) || 'Profile mode unavailable.');
            }
            if (requestSequence !== profileLlmRequestSequence || target.key !== profileLlmTargetKey) {
                return;
            }

            currentProfileLlmInfo = result.profile;
            currentProfileLlmMode = result.profile.random_enabled ? 'random' : 'fixed';
            renderProfileLlmMode();
            return currentProfileLlmInfo;
        } catch (_err) {
            if (requestSequence !== profileLlmRequestSequence) return;
            currentProfileLlmInfo = null;
            currentProfileLlmMode = 'fixed';
            renderProfileLlmMode();
            return null;
        }
    }

    async function saveProfileLlmMode(mode) {
        const target = getProfileLlmTarget();
        if (!target || profileLlmSaveInProgress) {
            return false;
        }

        const previousMode = currentProfileLlmMode;
        profileLlmSaveInProgress = true;
        renderProfileLlmMode();

        try {
            const formData = new FormData();
            formData.append('target_type', target.type);
            formData.append('target_name', target.name);
            formData.append('mode', mode);
            if (currentProfileLlmInfo && currentProfileLlmInfo.profile_id) {
                formData.append('expected_profile_id', String(currentProfileLlmInfo.profile_id));
            }

            const response = await fetch(`${serverUrl}/ui/api/chim_profile_llm_mode.php`, {
                method: 'POST',
                body: formData,
                cache: 'no-store'
            });
            const result = await response.json();
            if (!response.ok || !result || !result.ok || !result.profile) {
                throw new Error((result && result.message) || 'Failed to update profile LLM mode.');
            }

            currentProfileLlmInfo = result.profile;
            currentProfileLlmMode = result.profile.random_enabled ? 'random' : 'fixed';
            renderProfileLlmMode();
            const count = Number(result.profile.shared_count || 0);
            const usage = count === 1 ? 'used by 1 character' : `used by ${count} characters`;
            pushChatboxSystemMessage(
                `${currentProfileLlmMode === 'random' ? 'Random' : 'Fixed'} LLM selection enabled for ` +
                `${result.profile.profile_name} (${usage}).`
            );
            return true;
        } catch (_err) {
            currentProfileLlmMode = previousMode;
            renderProfileLlmMode();
            pushChatboxSystemMessage('Failed to update the target profile LLM mode.');
            showInGameDebugNotification('Failed to update target profile LLM mode.');
            return false;
        } finally {
            profileLlmSaveInProgress = false;
            renderProfileLlmMode();
        }
    }

    async function saveProfileDefault(setting, enabled) {
        const target = getProfileLlmTarget();
        if (!target || !currentProfileLlmInfo || profileDefaultSaveInProgress ||
            profileLlmSaveInProgress || profileAssignmentInProgress) {
            return false;
        }

        const targetKey = target.key;
        const previousProfile = currentProfileLlmInfo;
        profileDefaultSaveInProgress = true;
        renderProfileLlmMode();

        try {
            const formData = new FormData();
            formData.append('target_type', target.type);
            formData.append('target_name', target.name);
            formData.append('setting', setting);
            formData.append('enabled', enabled ? '1' : '0');
            formData.append('expected_profile_id', String(currentProfileLlmInfo.profile_id));

            const response = await fetch(`${serverUrl}/ui/api/chim_profile_llm_mode.php`, {
                method: 'POST',
                body: formData,
                cache: 'no-store'
            });
            const result = await response.json();
            if (!response.ok || !result || !result.ok || !result.profile) {
                throw new Error((result && result.message) || 'Failed to update profile default.');
            }

            const activeTarget = getProfileLlmTarget();
            if (!activeTarget || activeTarget.key !== targetKey) return true;

            currentProfileLlmInfo = result.profile;
            currentProfileLlmMode = result.profile.random_enabled ? 'random' : 'fixed';
            renderProfileLlmMode();
            const button = Array.from(profileDefaultToggleButtons).find(function(item) {
                return item.dataset.profileSetting === setting;
            });
            const label = button ? button.querySelector('.profile-default-label').textContent.trim() : setting;
            pushChatboxSystemMessage(
                `${label} ${enabled ? 'enabled' : 'disabled'} for ${result.profile.profile_name}.`
            );
            return true;
        } catch (_err) {
            currentProfileLlmInfo = previousProfile;
            renderProfileLlmMode();
            pushChatboxSystemMessage('Failed to update the target profile setting.');
            showInGameDebugNotification('Failed to update target profile setting.');
            return false;
        } finally {
            profileDefaultSaveInProgress = false;
            renderProfileLlmMode();
        }
    }

    function openProfileMenu() {
        if (!profileMenuElement || !currentProfileLlmInfo) return;
        profileMenuElement.classList.remove('hidden');
        if (profileMenuToggleButton) profileMenuToggleButton.setAttribute('aria-expanded', 'true');
        refreshProfileLlmMode(true);
    }

    function closeProfileMenu() {
        if (profileMenuElement) profileMenuElement.classList.add('hidden');
        if (profileMenuToggleButton) profileMenuToggleButton.setAttribute('aria-expanded', 'false');
    }

    function isProfileMenuOpen() {
        return !!profileMenuElement && !profileMenuElement.classList.contains('hidden');
    }

    async function assignTargetProfile(slot) {
        const target = getProfileLlmTarget();
        if (!target || target.type !== 'npc' || profileAssignmentInProgress || !currentProfileLlmInfo) {
            return false;
        }

        const selectedProfile = (currentProfileLlmInfo.available_profiles || []).find(function(profile) {
            return Number(profile.slot) === Number(slot);
        });
        if (!selectedProfile || Number(selectedProfile.profile_id) === Number(currentProfileLlmInfo.profile_id)) {
            renderProfileMenu();
            return true;
        }

        profileAssignmentInProgress = true;
        renderProfileMenu();
        sendControlCommand(`profile_${slot}|${target.name}`);

        let assigned = false;
        for (const delay of [350, 800, 1500]) {
            await new Promise(function(resolve) { setTimeout(resolve, delay); });
            const activeTarget = getProfileLlmTarget();
            if (!activeTarget || activeTarget.key !== target.key) break;
            const refreshed = await refreshProfileLlmMode(true);
            if (refreshed && Number(refreshed.profile_id) === Number(selectedProfile.profile_id)) {
                assigned = true;
                break;
            }
        }

        profileAssignmentInProgress = false;
        renderProfileLlmMode();
        if (assigned) {
            pushChatboxSystemMessage(`Assigned ${selectedProfile.profile_name} to ${target.name}.`);
        } else {
            pushChatboxSystemMessage(`Profile assignment sent for ${target.name}. The server may still be processing it.`);
        }
        return assigned;
    }

    function renderRechatMode(mode) {
        const normalizedMode = ['tight', 'conversational', 'group', 'random'].includes(mode) ? mode : 'random';
        const config = rechatModeConfig[normalizedMode];
        currentRechatMode = normalizedMode;
        if (currentRechatModeElement) {
            currentRechatModeElement.className = 'mode-badge ' + config.class;
            currentRechatModeElement.textContent = config.label;
        }
        setActiveTile(rechatOptionButtons, 'mode', normalizedMode);
    }

    window.updateChatboxRechatMode = function(mode) {
        if (rechatModeSaveInProgress) return;
        renderRechatMode(mode);
    };

    async function saveRechatMode(mode) {
        if (rechatModeSaveInProgress) return;

        const previousMode = currentRechatMode;
        rechatModeSaveInProgress = true;
        setTileSelectorDisabled(
            rechatMenuToggleButton,
            rechatOptionButtons,
            true,
            'Updating global rechat mode'
        );
        closeTileMenu(rechatMenuToggleButton, rechatOptionsElement);

        try {
            const formData = new FormData();
            formData.append('mode', mode);
            const response = await fetch(`${serverUrl}/ui/cmd/action_set_rechat_mode.php`, {
                method: 'POST',
                body: formData,
                cache: 'no-store'
            });
            const result = await response.json();
            if (!response.ok || !result || !result.ok) {
                throw new Error((result && result.message) || 'Failed to update rechat mode.');
            }

            renderRechatMode(result.rechat_mode || mode);
            pushChatboxSystemMessage(`Global rechat mode changed to ${currentRechatMode}.`);
        } catch (_err) {
            renderRechatMode(previousMode);
            pushChatboxSystemMessage('Failed to update global rechat mode.');
            showInGameDebugNotification('Failed to update global rechat mode.');
        } finally {
            rechatModeSaveInProgress = false;
            setTileSelectorDisabled(
                rechatMenuToggleButton,
                rechatOptionButtons,
                false,
                'Switch global rechat mode'
            );
        }
    }

    function getCurrentTime() {
        return new Date().toLocaleTimeString('en-US', { hour12: false });
    }

    function formatTimestamp(timestamp) {
        try {
            return new Date(timestamp).toLocaleTimeString('en-US', { hour12: false });
        } catch (_e) {
            return timestamp;
        }
    }

    if (modeMenuToggleButton) {
        modeMenuToggleButton.addEventListener('click', function(event) {
            event.stopPropagation();
            toggleTileMenu(modeMenuToggleButton, modeOptionsElement);
        });
    }

    modeOptionButtons.forEach(function(button) {
        button.addEventListener('click', function(event) {
            event.stopPropagation();
            const action = button.dataset.action;
            closeTileMenu(modeMenuToggleButton, modeOptionsElement);
            if (!action || action === currentModeAction) return;
            resetTargetSelectionForModeChange();
            sendControlCommand(action);
        });
    });

    if (modelMenuToggleButton) {
        modelMenuToggleButton.addEventListener('click', function(event) {
            event.stopPropagation();
            toggleTileMenu(modelMenuToggleButton, modelOptionsElement);
        });
    }

    modelOptionButtons.forEach(function(button) {
        button.addEventListener('click', function(event) {
            event.stopPropagation();
            const action = button.dataset.action;
            closeTileMenu(modelMenuToggleButton, modelOptionsElement);
            if (!action || action === currentModelAction) return;

            const config = Object.values(modelConfig).find(function(item) {
                return item.action === action;
            });
            if (config) {
                currentGlobalModelLabel = config.label;
                renderProfileLlmMode();
            }
            sendControlCommand(action);
        });
    });

    if (profileMenuToggleButton) {
        profileMenuToggleButton.addEventListener('click', function(event) {
            event.stopPropagation();
            closeAllTileMenus();
            if (isProfileMenuOpen()) {
                closeProfileMenu();
            } else {
                openProfileMenu();
            }
        });
    }

    if (profileMenuCloseButton) {
        profileMenuCloseButton.addEventListener('click', function() {
            closeProfileMenu();
        });
    }

    if (profileMenuElement) {
        profileMenuElement.addEventListener('click', function(event) {
            event.stopPropagation();
        });
    }

    if (profileRandomToggleButton) {
        profileRandomToggleButton.addEventListener('click', function() {
            if (!currentProfileLlmInfo || profileLlmSaveInProgress) return;
            saveProfileLlmMode(currentProfileLlmMode === 'random' ? 'fixed' : 'random');
        });
    }

    profileDefaultToggleButtons.forEach(function(button) {
        button.addEventListener('click', function() {
            if (!currentProfileLlmInfo || profileDefaultSaveInProgress) return;
            const setting = button.dataset.profileSetting || '';
            const enabled = button.getAttribute('aria-pressed') === 'true';
            saveProfileDefault(setting, !enabled);
        });
    });

    if (profileSelectElement) {
        profileSelectElement.addEventListener('change', function() {
            const slot = Number(profileSelectElement.value || 0);
            if (slot > 0) assignTargetProfile(slot);
        });
    }

    document.addEventListener('click', function() {
        if (isProfileMenuOpen()) closeProfileMenu();
        closeAllTileMenus();
    });

    focusPositionButtons.forEach(function(button) {
        button.addEventListener('click', function() {
            const nextPosition = button.dataset.position || 'center';
            applyFocusPosition(nextPosition);
            saveFocusPosition(currentFocusPosition);
        });
    });

    if (deleteEventConfirmButton) {
        deleteEventConfirmButton.addEventListener('click', function() {
            const deleteCount = normalizeDeleteEventCount(deleteEventSelect ? deleteEventSelect.value : 0);
            if (!deleteCount) {
                clearPendingDeleteConfirmation();
                return;
            }
            if (pendingDeleteCount === deleteCount) {
                window.deleteRecentEvents(deleteCount);
                return;
            }

            armDeleteConfirmation(deleteCount);
        });
    }

    if (rechatMenuToggleButton) {
        rechatMenuToggleButton.addEventListener('click', function(event) {
            event.stopPropagation();
            toggleTileMenu(rechatMenuToggleButton, rechatOptionsElement);
        });
    }

    rechatOptionButtons.forEach(function(button) {
        button.addEventListener('click', function(event) {
            event.stopPropagation();
            const mode = button.dataset.mode;
            closeTileMenu(rechatMenuToggleButton, rechatOptionsElement);
            if (!mode || mode === currentRechatMode) return;
            saveRechatMode(mode);
        });
    });

    if (deleteEventSelect) {
        deleteEventSelect.addEventListener('change', function() {
            clearPendingDeleteConfirmation();
        });
    }

    if (storyNewEventsButton) {
        storyNewEventsButton.addEventListener('click', scrollStoryToBottom);
    }

    if (contextToggleButton && contextPanelElement) {
        contextToggleButton.addEventListener('click', function() {
            applyContextCollapsed(!contextPanelElement.classList.contains('collapsed'));
        });
    }

    if (targetsListElement) {
        targetsListElement.addEventListener('click', function(e) {
            const targetButton = e.target.closest('.chatbox-target-item');
            if (!targetButton) return;

            if (targetButton.dataset.auto === 'true') {
                sendControlCommand('target_override_clear');
                return;
            }
            if (targetButton.dataset.everyone === 'true') {
                sendControlCommand('target_override_everyone');
                return;
            }

            const formId = targetButton.dataset.formId || '0';
            const targetName = targetButton.dataset.targetName || '';
            if (!targetName) return;
            if (targetButton.classList.contains('override')) {
                sendControlCommand('target_override_clear');
                // Optimistic: clear local highlight before the next bridge refresh lands
                targetsListElement.querySelectorAll('.chatbox-target-item.override')
                    .forEach(function(el) { el.classList.remove('override'); });
                return;
            }
            sendControlCommand(`target_override|${formId}|${targetName}`);
            // Optimistic: move local highlight immediately before the bridge refresh lands
            targetsListElement.querySelectorAll('.chatbox-target-item.override')
                .forEach(function(el) { el.classList.remove('override'); });
            targetButton.classList.add('override');
        });
    }

    window.updateChatboxMode('STANDARD');
    window.updateChatboxModel('Standard');
    renderRechatMode('random');
    applyFocusPosition(loadFocusPosition());
    applyContextCollapsed(loadContextCollapsed());
    setContextPlacement(false);

    console.log('[Chatbox] Initialized - display mode + focus modal input');
})();
