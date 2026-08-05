/**
 * CHIM Conversation History Panel
 * JavaScript for handling UI updates and interactions
 */

(function() {
    'use strict';

    // DOM Elements
    const historyList = document.getElementById('history-list');
    const loadingIndicator = document.getElementById('loading-indicator');
    const emptyMessage = document.getElementById('empty-message');

    // State
    let entries = [];
    let lastRowId = 0;
    let narratorName = 'The Narrator';
    let serverUrl = 'http://192.168.169.218:8081/HerikaServer';
    const dialogueEventTypes = new Set(['chat', 'inputtext', 'ginputtext']);

    /**
     * Update the history panel with data from the server
     * Called from C++ via PrismaUI->Invoke()
     * @param {string} jsonString - JSON string containing the event data
     */
    window.updateHistory = function(jsonString) {
        try {
            const data = JSON.parse(jsonString);
            
            console.log('[CHIM History] Received data from server, success:', data.success);
            console.log('[CHIM History] Data array length:', data.data ? data.data.length : 0);
            
            if (!data.success || !data.data) {
                console.error('[CHIM History] Invalid response format');
                showEmpty();
                return;
            }

            if (data.narrator_name) {
                narratorName = stripHtml(data.narrator_name).trim() || 'The Narrator';
            }

            hideLoading();

            if (data.data.length === 0) {
                console.log('[CHIM History] No data returned from server');
                showEmpty();
                return;
            }

            hideEmpty();
            
            // Keep entries in DESC order (newest first at top, oldest at bottom)
            entries = data.data;
            
            console.log('[CHIM History] First entry (newest):', entries[0]);
            console.log('[CHIM History] Last entry (oldest):', entries[entries.length - 1]);
            
            renderEntries();
            
            // Update last row ID for incremental updates (first entry is most recent)
            if (entries.length > 0 && entries[0].ROWID) {
                lastRowId = parseInt(entries[0].ROWID);
                console.log('[CHIM History] Updated lastRowId to:', lastRowId);
            }
            
        } catch (e) {
            console.error('Error parsing history data:', e);
            showEmpty();
        }
    };

    /**
     * Push a single new entry to the panel
     * Called from C++ for real-time updates
     * @param {string} jsonString - JSON string containing a single entry
     */
    window.pushEntry = function(jsonString) {
        try {
            const entry = JSON.parse(jsonString);
            
            const eventType = entry.eventType || 'chat';
            if (!isDialogueEvent(eventType)) {
                return;
            }
            
            const fullText = sanitizeDialogueText((entry.speaker || '') + ': ' + (entry.text || ''));
            if (!fullText) {
                return;
            }
            
            hideEmpty();
            
            const entryEl = createEntryElement({
                'Event': eventType,
                'Events': fullText,
                'Tamrielic Time': entry.timestamp || '',
                'Source': entry.source || 'llm',
                'Speaker Type': entry.speakerType || ''
            });
            
            // Prepend new entry at top (newest first)
            historyList.insertBefore(entryEl, historyList.firstChild);
            
            // Scroll to top to show the new entry
            historyList.scrollTop = 0;
            
        } catch (e) {
            console.error('Error pushing entry:', e);
        }
    };

    window.setHistoryServerUrl = function(url) {
        const normalized = String(url || '').replace(/\/$/, '');
        if (normalized) serverUrl = normalized;
    };

    /**
     * Render all entries to the DOM
     */
    function renderEntries() {
        historyList.innerHTML = '';
        
        console.log('[CHIM History] Rendering entries, total count:', entries.length);
        
        let rendered = 0;
        
        // Entries are already in DESC order (newest first)
        entries.forEach((entry, index) => {
            const eventType = stripHtml(entry['Event'] || 'chat');
            if (!isDialogueEvent(eventType)) {
                return;
            }

            const rawEventData = stripHtml(entry['Events'] || '');
            const eventData = sanitizeDialogueText(rawEventData);
            if (!eventData) {
                return;
            }
            
            // Debug first 5 entries
            if (index < 5) {
                console.log('[CHIM History] Entry', index, ':', eventData.substring(0, 100));
            }
            
            const entryEl = createEntryElement(entry, eventData, inferDialogueSource(entry, rawEventData));
            historyList.appendChild(entryEl);
            rendered++;
        });
        
        console.log('[CHIM History] Rendered:', rendered);
        
        // Scroll to top (newest entries are at top)
        historyList.scrollTop = 0;
    }

    /**
     * Strip HTML tags from a string
     * @param {string} html - String potentially containing HTML
     * @returns {string} - Clean text without HTML
     */
    function stripHtml(html) {
        if (!html) return '';
        // First decode HTML entities, then strip tags
        const temp = document.createElement('div');
        temp.innerHTML = html;
        return temp.textContent || temp.innerText || '';
    }

    function isDialogueEvent(eventType) {
        return dialogueEventTypes.has(String(eventType || '').toLowerCase());
    }

    function sanitizeDialogueText(text) {
        let cleaned = String(text || '').trim();
        if (!cleaned) return '';

        if (/^\(?Context History/i.test(cleaned)) {
            return '';
        }

        cleaned = cleaned
            .replace(/^\(Context (?:new )?location:[^)]+\)\s*/i, '')
            .replace(/\s+\(Context (?:new )?location:[\s\S]*$/i, '')
            .replace(/\s*,+\s*Current Date in Skyrim World:[\s\S]*$/i, '')
            .trim();

        return cleaned;
    }

    function inferDialogueSource(entry, rawEventData) {
        const explicitSource = stripHtml(entry['Source'] || entry.source || '');
        if (explicitSource) return explicitSource;

        const raw = String(rawEventData || '');
        if (/\(Context (?:new )?location:[^)]*background chat/i.test(raw)) {
            return 'subtitle';
        }
        return 'llm';
    }

    /**
     * Create a DOM element for a history entry
     * @param {Object} entry - Entry data from the server
     * @returns {HTMLElement} - The entry element
     */
    function createEntryElement(entry, sanitizedEventData, sourceOverride) {
        const div = document.createElement('div');
        div.className = 'history-entry';
        const rowId = Number(entry.ROWID || entry.rowId || 0);
        if (rowId > 0) div.dataset.rowId = String(rowId);
        
        // Parse the event data - strip HTML from all fields
        const eventType = stripHtml(entry['Event'] || 'chat');
        let eventData = sanitizedEventData || sanitizeDialogueText(stripHtml(entry['Events'] || ''));
        const source = sourceOverride || inferDialogueSource(entry, entry['Events'] || '');
        
        // Handle timestamp - key might have HTML in older API responses
        let timestamp = '';
        for (const key of Object.keys(entry)) {
            if (key.toLowerCase().includes('tamrielic') || key.toLowerCase().includes('time')) {
                timestamp = stripHtml(entry[key] || '');
                break;
            }
        }
        if (!timestamp) {
            timestamp = stripHtml(entry['Tamrielic Time'] || '');
        }
        
        // Determine speaker and text
        let speaker = '';
        let text = eventData;
        
        // Check if this is an infoaction event (e.g., "Sinmir uses Chair")
        // These typically don't have a colon and describe actions
        if (eventType.toLowerCase() === 'infoaction') {
            speaker = 'Action';
            text = eventData;
        } else {
            // Extract speaker from "Speaker: text" format
            const colonIndex = eventData.indexOf(':');
            if (colonIndex > 0 && colonIndex < 50) {
                speaker = eventData.substring(0, colonIndex).trim();
                text = eventData.substring(colonIndex + 1).trim();
            }
        }
        
        // Determine entry class based on speaker/type
        const speakerLower = speaker.toLowerCase();
        const explicitSpeakerType = stripHtml(entry['Speaker Type'] || entry.speakerType || '').toLowerCase();
        if (speakerLower === 'player' || speakerLower.includes('dovahkiin')) {
            div.classList.add('player');
        } else if (explicitSpeakerType === 'narrator'
            || speakerLower === narratorName.toLowerCase()
            || speakerLower === 'the narrator'
            || speakerLower === 'narrator') {
            div.classList.add('narrator');
        } else if (speakerLower === 'action') {
            div.classList.add('action');
        } else {
            div.classList.add('npc');
        }
        if (source === 'subtitle') {
            div.classList.add('non-llm');
        }
        
        // Build entry HTML
        div.innerHTML = `
            <div class="entry-header">
                <span class="entry-speaker">${escapeHtml(speaker || 'Unknown')}</span>
                <span class="entry-header-actions">
                    <span class="entry-timestamp">${escapeHtml(timestamp)}</span>
                </span>
            </div>
            <div class="entry-text">${escapeHtml(text)}</div>
        `;
        if (rowId > 0) {
            div.querySelector('.entry-header-actions').appendChild(createDeleteButton(rowId));
        }
        
        return div;
    }

    function createDeleteButton(rowId) {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'entry-delete';
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
            if (window.chimHistoryCommand) {
                window.chimHistoryCommand('event_deleted|' + rowId);
            }
        } catch (error) {
            button.disabled = false;
            button.classList.remove('confirm-delete');
            button.title = error.message || 'Failed to delete event.';
        }
    }

    window.removeEventLogEntry = function(rowId) {
        const normalizedRowId = Number(rowId || 0);
        if (normalizedRowId <= 0) return;
        entries = entries.filter(function(entry) {
            return Number(entry.ROWID || entry.rowId || 0) !== normalizedRowId;
        });
        const row = historyList.querySelector(`[data-row-id="${normalizedRowId}"]`);
        if (row) row.remove();
        if (!historyList.querySelector('.history-entry')) showEmpty();
    };

    /**
     * Escape HTML special characters
     * @param {string} text - Text to escape
     * @returns {string} - Escaped text
     */
    function escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }

    /**
     * Show the loading indicator
     */
    function showLoading() {
        loadingIndicator.classList.remove('hidden');
        emptyMessage.classList.add('hidden');
    }

    /**
     * Hide the loading indicator
     */
    function hideLoading() {
        loadingIndicator.classList.add('hidden');
    }

    /**
     * Show the empty message
     */
    function showEmpty() {
        hideLoading();
        emptyMessage.classList.remove('hidden');
    }

    /**
     * Hide the empty message
     */
    function hideEmpty() {
        emptyMessage.classList.add('hidden');
    }

    /**
     * Refresh the history by requesting from C++
     * This sends a command back to the plugin
     */
    window.refreshHistory = function() {
        showLoading();
        // Call the registered JS listener in C++
        if (window.chimHistoryCommand) {
            window.chimHistoryCommand('refresh');
        }
    };

    /**
     * Close the panel by requesting from C++
     */
    window.closePanel = function() {
        if (window.chimHistoryCommand) {
            window.chimHistoryCommand('close');
        }
    };

    // Initialize
    showLoading();
    
})();
