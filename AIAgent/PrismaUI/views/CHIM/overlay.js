/**
 * CHIM Overlay
 * JavaScript for displaying system status
 */

(function() {
    'use strict';

    // DOM Elements
    const modeElement = document.getElementById('current-mode');
    const activeModelElement = document.getElementById('active-model');
    const focusChatElement = document.getElementById('focus-chat');
    const agentsListElement = document.getElementById('agents-list');
    const slotsGridElement = document.getElementById('slots-grid');

    // Update timer
    let updateInterval = null;
    let lastSpatialAgentsAt = 0;
    const agentRowsByKey = new Map();

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

    function makeAgentRow(isEmpty) {
        const row = document.createElement('div');
        if (isEmpty) {
            row.className = 'empty-agents';
            return row;
        }

        row.className = 'agent-item';
        row.__chimNameElement = document.createElement('div');
        row.__chimNameElement.className = 'agent-name';
        row.__chimMetaElement = document.createElement('div');
        row.__chimMetaElement.className = 'agent-meta';
        row.appendChild(row.__chimNameElement);
        row.appendChild(row.__chimMetaElement);
        return row;
    }

    function updateAgentRow(row, spec) {
        if (spec.empty) {
            setClassNameIfChanged(row, 'empty-agents');
            setTextIfChanged(row, spec.message);
            return;
        }

        setClassNameIfChanged(row, spec.className);
        setTextIfChanged(row.__chimNameElement, spec.name);
        setTextIfChanged(row.__chimMetaElement, spec.meta);
    }

    function syncAgentRows(specs) {
        const seen = new Set();
        specs.forEach(function(spec, index) {
            seen.add(spec.key);
            let row = agentRowsByKey.get(spec.key);
            if (!row) {
                row = makeAgentRow(!!spec.empty);
                agentRowsByKey.set(spec.key, row);
            }

            updateAgentRow(row, spec);
            if (agentsListElement.children[index] !== row) {
                agentsListElement.insertBefore(row, agentsListElement.children[index] || null);
            }
        });

        agentRowsByKey.forEach(function(row, key) {
            if (!seen.has(key)) {
                row.remove();
                agentRowsByKey.delete(key);
            }
        });
    }

    // Mode display names and classes
    const modeConfig = {
        'STANDARD': { label: 'Standard', class: 'standard' },
        'WHISPER': { label: 'Whisper', class: 'whisper' },
        'DIRECTOR': { label: 'Director', class: 'director' },
        'SPAWN': { label: 'Spawn', class: 'director' },
        'CHEATMODE': { label: 'Cheat Mode', class: 'cheatmode' },
        'AUTOCHAT': { label: 'Auto Chat', class: 'autochat' },
        'INJECTION_LOG': { label: 'Event Inject', class: 'director' },
        'INJECT_LOG': { label: 'Event Inject', class: 'director' }
    };

    /**
     * Update the overlay with data from the server
     * Called from C++ via PrismaUI->Invoke()
     * @param {string} jsonString - JSON string containing the overlay data
     */
    window.updateOverlay = function(jsonString) {
        try {
            const data = JSON.parse(jsonString);
            
            if (!data.success || !data.data) {
                console.error('Invalid response format');
                return;
            }

            const overlay = data.data;
            
            console.log('Overlay data received:', overlay);
            
            // Update mode
            updateMode(overlay.mode);
            
            // Update active model
            updateActiveModel(overlay.active_model_slot, overlay.active_model_label, overlay.active_model_name);
            
            // Update Compact Chat
            updateFocusChat(overlay.focus_chat);
            
            // Update active agents from server only until the DLL starts pushing local spatial truth.
            if (!lastSpatialAgentsAt || Date.now() - lastSpatialAgentsAt > 5000) {
                updateActiveAgents(overlay.active_agents);
            }
            
            // Update profile slots
            updateProfileSlots(overlay.profile_slots, overlay.active_model_slot);
            
        } catch (e) {
            console.error('Error parsing overlay data:', e);
        }
    };

    /**
     * Update the mode display
     */
    function updateMode(mode) {
        const modeUpper = mode ? mode.toUpperCase().trim() : 'STANDARD';
        const config = modeConfig[modeUpper] || { label: mode || 'Unknown', class: 'standard' };
        setHtmlIfChanged(modeElement, `<span class="mode-badge ${config.class}">${config.label}</span>`);
    }

    /**
     * Update the active model display
     */
    function updateActiveModel(slot, modelLabel, modelName) {
        // Show just the model label with badge styling like mode
        const labelLower = modelLabel ? modelLabel.toLowerCase().trim() : 'standard';
        let modelClass = '';
        
        // Map model labels to CSS classes
        if (labelLower.includes('standard')) {
            modelClass = 'standard';
        } else if (labelLower.includes('fast')) {
            modelClass = 'fast';
        } else if (labelLower.includes('powerful')) {
            modelClass = 'powerful';
        } else if (labelLower.includes('experimental')) {
            modelClass = 'experimental';
        }
        
        setHtmlIfChanged(activeModelElement, `<span class="mode-badge ${modelClass}">${escapeHtml(modelLabel)}</span>`);
    }

    /**
     * Update the Compact Chat display
     */
    function updateFocusChat(enabled) {
        const statusClass = enabled ? 'on' : 'off';
        const statusText = enabled ? 'ON' : 'OFF';
        setHtmlIfChanged(focusChatElement, `<span class="toggle-indicator ${statusClass}">${statusText}</span>`);
    }

    /**
     * Update the active agents list
     */
    function updateActiveAgents(agents) {
        const countElement = document.querySelector('.agent-count');
        const previousScrollTop = agentsListElement.scrollTop;
        
        if (!agents || agents.length === 0) {
            syncAgentRows([{ key: 'empty', empty: true, message: 'No agents nearby' }]);
            setTextIfChanged(countElement, '0');
            return;
        }

        setTextIfChanged(countElement, agents.length.toString());

        const specs = agents.map((agent, index) => {
            if (typeof agent === 'string') {
                return {
                    key: `name:${agent}`,
                    className: 'agent-item',
                    name: agent,
                    meta: ''
                };
            }

            const name = agent && agent.name ? agent.name : 'Unknown Target';
            const distance = Number(agent && agent.distance ? agent.distance : 0).toFixed(1);
            const status = agent && agent.status ? agent.status : '';
            const meta = status ? `${distance}m - ${status}` : `${distance}m`;
            const classes = ['agent-item'];
            if (agent && agent.active) classes.push('active');
            if (agent && agent.targetable === false) classes.push('blocked');
            if (agent && agent.look_target) classes.push('look-target');

            const formId = agent && (agent.form_id || agent.formId) ? (agent.form_id || agent.formId) : '';
            return {
                key: formId ? `form:${formId}` : `name:${name}:${index}`,
                className: classes.join(' '),
                name,
                meta
            };
        });
        
        syncAgentRows(specs);
        agentsListElement.scrollTop = previousScrollTop;
    }

    window.updateSpatialAgents = function(payloadJson) {
        try {
            const parsed = JSON.parse(payloadJson);
            const agents = Array.isArray(parsed) ? parsed : [];
            lastSpatialAgentsAt = Date.now();
            updateActiveAgents(agents);
        } catch (e) {
            console.error('Error parsing spatial agents:', e);
        }
    };

    /**
     * Update the profile slots grid
     */
    function updateProfileSlots(slots, activeSlot) {
        let html = '';
        
        for (let i = 1; i <= 4; i++) {
            const slot = slots[i];
            const isActive = i === activeSlot;
            
            if (slot) {
                html += `<div class="slot-card ${isActive ? 'active' : ''}">`;
                html += `<div class="slot-header">`;
                html += `<span class="slot-number">Slot ${i}</span>`;
                if (isActive) {
                    html += `<span style="color: rgb(242, 124, 17); font-size: 0.7em;">●</span>`;
                }
                html += `</div>`;
                html += `<div class="slot-profile-name">${escapeHtml(slot.profile_name)}</div>`;
                
                // Show all connectors
                const connectorKeys = Object.keys(slot.connectors);
                if (connectorKeys.length > 0) {
                    connectorKeys.forEach((key) => {
                        const conn = slot.connectors[key];
                        const shortName = key.replace(' LLM', '');
                        html += `<div class="slot-connector">`;
                        html += `<span class="slot-connector-label">${shortName}:</span> `;
                        html += `${escapeHtml(conn.label)}`;
                        html += `</div>`;
                    });
                }
                
                html += `</div>`;
            } else {
                html += `<div class="slot-card empty">`;
                html += `<div class="slot-header">`;
                html += `<span class="slot-number">Slot ${i}</span>`;
                html += `</div>`;
                html += `<div class="empty-slot-text">No profile assigned</div>`;
                html += `</div>`;
            }
        }
        
        setHtmlIfChanged(slotsGridElement, html);
    }

    /**
     * Escape HTML special characters
     * @param {string} text - Text to escape
     * @returns {string} - Escaped text
     */
    function escapeHtml(text) {
        if (!text) return '';
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }

    /**
     * Update the crosshair target display
     * @param {string} name - Name of the targeted NPC
     * @param {number} distance - Distance to the NPC in meters
     * @param {string} status - Spatial targeting status
     * @param {boolean} targetable - Whether this is an active valid target
     */
    window.updateCrosshairTarget = function(name, distance, status, targetable) {
        const targetElement = document.getElementById('crosshair-target');
        const safeStatus = status ? escapeHtml(status) : '';
        const statusHtml = safeStatus ? `<div class="target-status">${safeStatus}</div>` : '';
        const isTargetable = targetable !== false;

        if (name && name !== '') {
            setHtmlIfChanged(targetElement, `
                <div>
                    <span class="target-name ${isTargetable ? '' : 'blocked-target'}">${escapeHtml(name)}</span>
                    <span class="target-distance">(${distance.toFixed(1)}m)</span>
                </div>
                ${statusHtml}
            `);
        } else {
            setHtmlIfChanged(targetElement, `
                <div><span class="target-name no-target">No target</span></div>
                ${statusHtml}
            `);
        }
    };

    /**
     * Close the overlay
     */
    window.closeOverlay = function() {
        stopAutoUpdate();
        if (window.chimOverlayCommand) {
            window.chimOverlayCommand('close');
        }
    };

    window.onOverlayShown = function() {
        window.startAutoUpdate();
    };

    window.onOverlayHidden = function() {
        stopAutoUpdate();
    };

    /**
     * Start auto-updating every 3 seconds
     */
    window.startAutoUpdate = function() {
        stopAutoUpdate(); // Clear any existing interval
        updateInterval = setInterval(function() {
            // Request C++ to fetch and update
            if (window.chimOverlayCommand) {
                window.chimOverlayCommand('refresh');
            }
        }, 3000); // 3 seconds
        console.log('CHIM Overlay auto-update started');
    };

    /**
     * Stop auto-updating
     */
    function stopAutoUpdate() {
        if (updateInterval) {
            clearInterval(updateInterval);
            updateInterval = null;
            console.log('CHIM Overlay auto-update stopped');
        }
    }

    window.stopAutoUpdate = stopAutoUpdate;

    // Apply corner placement via shared layout manager
    var overlayRoot = document.getElementById('chim-overlay');
    if (window.chimLayout) {
        window.chimLayout.apply(overlayRoot, 'overlay');
    }

    // Initialize with loading state
    console.log('CHIM Overlay initialized');
    
})();
