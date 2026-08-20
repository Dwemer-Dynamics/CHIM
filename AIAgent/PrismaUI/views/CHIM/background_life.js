(function () {
    'use strict';

    const byId = (id) => document.getElementById(id);
    const pageTabs = Array.from(document.querySelectorAll('.page-tab'));
    const pagePanels = Array.from(document.querySelectorAll('.tab-panel'));
    const detailTabs = Array.from(document.querySelectorAll('.detail-tab'));
    const tileDropdowns = [];

    let serverBaseUrl = 'http://127.0.0.1:8081/HerikaServer';
    let activePage = 'dashboard';
    let dashboardData = null;
    let dashboardGeneration = 0;
    let rumorGeneration = 0;
    let detailData = null;
    let detailTab = 'events';
    let currentPage = 1;
    let totalPages = 1;
    let searchTimer = null;
    let rumorBusy = false;
    let npcCreateBusy = false;
    let removeConfirmation = '';
    let removeTimer = null;
    const mapProvinceBounds = {
        left: 0.025,
        top: 0.14,
        right: 0.98,
        bottom: 0.89
    };
    let mapFitScale = 1;
    let mapZoom = 1;
    let mapScale = 1;
    let mapOffsetX = 0;
    let mapOffsetY = 0;
    let mapInitialized = false;
    let mapDragging = false;
    let mapDragStart = null;
    let nearbyTargets = [];
    let targetGeneration = 0;
    let currentTarget = emptyTarget();
    let currentPlayerLocation = { formid: '', name: '' };
    let nativePostAvailable = false;
    let nativePostSequence = 0;
    const nativePostRequests = new Map();
    const nativePostEndpoints = new Map([
        ['/ui/api/background_life_dashboard.php', 'dashboard'],
        ['/ui/api/background_life_npc.php', 'npc'],
        ['/ui/api/background_life_npc_create.php', 'npc_create'],
        ['/ui/api/background_life_request.php', 'request'],
        ['/ui/api/background_life_rumors.php', 'rumors']
    ]);

    function emptyTarget() {
        return {
            has_target: false,
            name: '',
            refid: '',
            game_enrolled: false,
            exists: false,
            background_life_enabled: false,
            auto_actions: false,
            send_letters: false,
            hourly_tracking: false
        };
    }

    function sendCommand(command) {
        if (window.chimBackgroundLifeCommand) {
            window.chimBackgroundLifeCommand(command);
        }
    }

    function createElement(tagName, className, text) {
        const element = document.createElement(tagName);
        if (className) element.className = className;
        if (text !== undefined) element.textContent = text;
        return element;
    }

    function normalizeServerBaseUrl(value) {
        let baseUrl = String(value || '').trim().replace(/\/+$/, '');
        if (!/\/HerikaServer$/i.test(baseUrl)) baseUrl += '/HerikaServer';
        return baseUrl;
    }

    function resolveServerAssetUrl(value) {
        const assetUrl = String(value || '').trim();
        if (assetUrl === '' || /^https?:\/\//i.test(assetUrl)) return assetUrl;

        try {
            const serverUrl = new URL(serverBaseUrl);
            if (assetUrl.startsWith('/')) return `${serverUrl.origin}${assetUrl}`;
        } catch (error) {
            return assetUrl;
        }

        return `${serverBaseUrl}/${assetUrl.replace(/^\/+/, '')}`;
    }

    async function parseJsonResponse(response) {
        let payload;
        try {
            payload = await response.json();
        } catch (error) {
            throw new Error(`Server returned invalid JSON (HTTP ${response.status})`);
        }
        if (!response.ok || !payload || !payload.success) {
            throw new Error((payload && (payload.error || payload.message)) || `HTTP ${response.status}`);
        }
        return payload;
    }

    async function postForm(path, values) {
        const body = values instanceof URLSearchParams ? values : new URLSearchParams(values);
        const endpoint = nativePostEndpoints.get(path);
        if (nativePostAvailable && endpoint) {
            const requestId = `bgl_${Date.now()}_${nativePostSequence += 1}`;
            return new Promise((resolve, reject) => {
                const timeout = window.setTimeout(() => {
                    nativePostRequests.delete(requestId);
                    reject(new Error('Background Life request timed out'));
                }, 75000);
                nativePostRequests.set(requestId, { resolve, reject, timeout });
                sendCommand(`post|${requestId}|${endpoint}|${body.toString()}`);
            });
        }
        return parseJsonResponse(await fetch(`${serverBaseUrl}${path}`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8' },
            body: body.toString()
        }));
    }

    window.setBackgroundLifeNativePostAvailable = function (available) {
        nativePostAvailable = available === true;
    };

    window.resolveBackgroundLifePost = function (requestId, responseText) {
        const pending = nativePostRequests.get(requestId);
        if (!pending) return;
        nativePostRequests.delete(requestId);
        window.clearTimeout(pending.timeout);
        try {
            const payload = JSON.parse(responseText);
            if (!payload || !payload.success) {
                throw new Error((payload && (payload.error || payload.message)) || 'Background Life request failed');
            }
            pending.resolve(payload);
        } catch (error) {
            pending.reject(error);
        }
    };

    window.rejectBackgroundLifePost = function (requestId, message) {
        const pending = nativePostRequests.get(requestId);
        if (!pending) return;
        nativePostRequests.delete(requestId);
        window.clearTimeout(pending.timeout);
        pending.reject(new Error(message || 'Background Life request failed'));
    };

    function closeTileDropdowns(except) {
        tileDropdowns.forEach((dropdown) => {
            if (dropdown !== except) dropdown.close();
        });
    }

    function createTileDropdown(input, toggle, label, options, config) {
        if (!input || !toggle || !label || !options) return null;
        const settings = config || {};
        const dropdown = {
            input,
            toggle,
            label,
            options,
            close() {
                options.classList.add('hidden');
                toggle.setAttribute('aria-expanded', 'false');
            },
            select(value, displayText, notify) {
                input.value = String(value);
                label.textContent = displayText;
                options.querySelectorAll('[data-value]').forEach((option) => {
                    const selected = option.dataset.value === input.value;
                    option.classList.toggle('is-active', selected);
                    option.setAttribute('aria-selected', selected ? 'true' : 'false');
                });
                dropdown.close();
                if (notify !== false) input.dispatchEvent(new Event('change', { bubbles: true }));
            },
            setOptions(entries, preferredValue) {
                options.replaceChildren();
                if (settings.searchPlaceholder) {
                    const search = createElement('input', 'target-option-search');
                    search.type = 'search';
                    search.placeholder = settings.searchPlaceholder;
                    search.setAttribute('aria-label', settings.searchPlaceholder);
                    search.addEventListener('input', () => {
                        const query = search.value.trim().toLowerCase();
                        options.querySelectorAll('[data-value]').forEach((option) => {
                            option.hidden = query !== '' &&
                                !option.textContent.toLowerCase().includes(query);
                        });
                    });
                    options.appendChild(search);
                    dropdown.search = search;
                }
                entries.forEach((entry) => {
                    const option = createElement('button', 'target-option-tile', entry.label);
                    option.type = 'button';
                    option.setAttribute('role', 'option');
                    option.dataset.value = String(entry.value);
                    options.appendChild(option);
                });
                const preferred = String(preferredValue === undefined ? input.value : preferredValue);
                const selected = Array.from(options.querySelectorAll('[data-value]'))
                    .find((option) => option.dataset.value === preferred) || options.querySelector('[data-value]');
                if (selected) dropdown.select(selected.dataset.value, selected.textContent, false);
            }
        };

        toggle.addEventListener('click', () => {
            const opening = options.classList.contains('hidden');
            closeTargetMenu();
            closeTileDropdowns(dropdown);
            options.classList.toggle('hidden', !opening);
            toggle.setAttribute('aria-expanded', opening ? 'true' : 'false');
            if (opening && dropdown.search) {
                dropdown.search.value = '';
                options.querySelectorAll('[data-value]').forEach((option) => {
                    option.hidden = false;
                });
                window.setTimeout(() => dropdown.search.focus(), 0);
            }
        });
        options.addEventListener('click', (event) => {
            const option = event.target.closest('[data-value]');
            if (option && options.contains(option)) {
                dropdown.select(option.dataset.value, option.textContent, true);
            }
        });
        tileDropdowns.push(dropdown);
        return dropdown;
    }

    const npcFilterDropdown = createTileDropdown(
        byId('npc-filter'),
        byId('npc-filter-toggle'),
        byId('npc-filter-label'),
        byId('npc-filter-options')
    );
    const limitDropdown = createTileDropdown(
        byId('limit-select'),
        byId('limit-select-toggle'),
        byId('limit-select-label'),
        byId('limit-select-options')
    );
    const rumorHoldDropdown = createTileDropdown(
        byId('rumor-hold-select'),
        byId('rumor-hold-select-toggle'),
        byId('rumor-hold-select-label'),
        byId('rumor-hold-select-options')
    );
    const npcCreateGenderDropdown = createTileDropdown(
        byId('npc-create-gender'),
        byId('npc-create-gender-toggle'),
        byId('npc-create-gender-label'),
        byId('npc-create-gender-options')
    );
    const npcCreateRaceDropdown = createTileDropdown(
        byId('npc-create-race'),
        byId('npc-create-race-toggle'),
        byId('npc-create-race-label'),
        byId('npc-create-race-options')
    );
    const npcCreateClassDropdown = createTileDropdown(
        byId('npc-create-class'),
        byId('npc-create-class-toggle'),
        byId('npc-create-class-label'),
        byId('npc-create-class-options')
    );
    const npcCreateLocationDropdown = createTileDropdown(
        byId('npc-create-location'),
        byId('npc-create-location-toggle'),
        byId('npc-create-location-label'),
        byId('npc-create-location-options'),
        { searchPlaceholder: 'Type to find a location' }
    );

    function switchPage(page) {
        activePage = page;
        pageTabs.forEach((tab) => {
            const active = tab.dataset.tab === page;
            tab.classList.toggle('active', active);
            tab.setAttribute('aria-selected', active ? 'true' : 'false');
        });
        pagePanels.forEach((panel) => {
            const active = panel.dataset.panel === page;
            panel.classList.toggle('active', active);
            panel.hidden = !active;
        });
        if (page === 'dashboard') refreshDashboard();
        if (page === 'history') requestHistory();
        if (page === 'rumors') refreshRumors();
    }

    function setDashboardStatus(message, error) {
        byId('roster-status').textContent = message || '';
        byId('roster-status').classList.toggle('error', !!error);
    }

    async function refreshDashboard() {
        const generation = ++dashboardGeneration;
        setDashboardStatus('Loading NPCs...', false);
        try {
            const showAll = byId('show-all-coords-toggle').checked ? '1' : '0';
            const payload = await parseJsonResponse(await fetch(
                `${serverBaseUrl}/ui/api/background_life_dashboard.php?show_all_coords=${showAll}`,
                { cache: 'no-store' }
            ));
            if (generation !== dashboardGeneration) return;
            dashboardData = payload.data || {};
            renderDashboard();
        } catch (error) {
            if (generation !== dashboardGeneration) return;
            setDashboardStatus(`Could not load dashboard: ${error.message || error}`, true);
            byId('roster-list').replaceChildren(
                createElement('div', 'empty-state', 'Background Life NPCs could not be loaded.')
            );
        }
    }

    function renderDashboard() {
        const data = dashboardData || {};
        const npcs = Array.isArray(data.npcs) ? data.npcs : [];
        const settings = data.settings || {};
        const game = data.game || {};
        byId('dashboard-game-date').textContent = game.tamrielic_date || 'No game time recorded yet.';
        byId('trigger-hours-input').value = settings.trigger_hours || 24;
        byId('roster-count').textContent = String(npcs.length);
        setDashboardStatus(npcs.length === 1 ? '1 tracked NPC' : `${npcs.length} tracked NPCs`, false);
        renderMap(data.map || {}, npcs);
        renderNpcCards(npcs);
    }

    function renderMap(map, npcs) {
        const canvas = byId('map-canvas');
        const image = byId('map-image');
        if (map.image_url) image.src = resolveServerAssetUrl(map.image_url);
        const locationsLayer = byId('location-markers');
        const npcLayer = byId('npc-map-markers');
        locationsLayer.replaceChildren();
        npcLayer.replaceChildren();

        const mapWidth = Number(map.width) || 1950;
        const mapHeight = Number(map.height) || 1625;
        const dimensionsChanged =
            canvas.offsetWidth !== mapWidth || canvas.offsetHeight !== mapHeight;
        canvas.style.width = `${mapWidth}px`;
        canvas.style.height = `${mapHeight}px`;

        (map.locations || []).forEach((entry) => {
            const marker = createElement('button', 'location-marker');
            marker.type = 'button';
            marker.style.left = `${entry.percent_x}%`;
            marker.style.top = `${entry.percent_y}%`;
            marker.title = entry.description ? `${entry.name}: ${entry.description}` : entry.name;
            locationsLayer.appendChild(marker);
        });

        npcs.filter((entry) => entry.has_coordinates).forEach((entry) => {
            const marker = createElement('button', 'npc-map-marker');
            marker.type = 'button';
            marker.dataset.npc = entry.name;
            marker.style.left = `${entry.percent_x}%`;
            marker.style.top = `${entry.percent_y}%`;
            marker.style.background = entry.color || '#f27c11';
            marker.title = `${entry.name}${entry.location ? ` - ${entry.location}` : ''}`;
            marker.appendChild(createElement(
                'span',
                'npc-map-marker-icon',
                entry.activity_icon || '✨'
            ));
            marker.appendChild(createElement(
                'span',
                'npc-map-marker-label',
                entry.name || 'Unknown NPC'
            ));
            marker.addEventListener('click', (event) => {
                event.stopPropagation();
                window.openNpcDetailModal(entry.name);
            });
            npcLayer.appendChild(marker);
        });
        window.requestAnimationFrame(() => {
            if (!mapInitialized || dimensionsChanged) {
                fitMapProvince();
            } else {
                applyMapTransform();
            }
        });
    }

    function clamp(value, minimum, maximum) {
        return Math.min(maximum, Math.max(minimum, value));
    }

    function updateMapFitScale() {
        const viewport = byId('map-viewport');
        const canvas = byId('map-canvas');
        const provinceWidth =
            canvas.offsetWidth * (mapProvinceBounds.right - mapProvinceBounds.left);
        const provinceHeight =
            canvas.offsetHeight * (mapProvinceBounds.bottom - mapProvinceBounds.top);
        if (!provinceWidth || !provinceHeight) return false;

        mapFitScale = Math.min(
            viewport.clientWidth / provinceWidth,
            viewport.clientHeight / provinceHeight
        ) * 0.96;
        return Number.isFinite(mapFitScale) && mapFitScale > 0;
    }

    function clampMapPan() {
        const viewport = byId('map-viewport');
        const canvas = byId('map-canvas');
        const scaledWidth = canvas.offsetWidth * mapScale;
        const scaledHeight = canvas.offsetHeight * mapScale;

        mapOffsetX = scaledWidth <= viewport.clientWidth
            ? (viewport.clientWidth - scaledWidth) / 2
            : clamp(mapOffsetX, viewport.clientWidth - scaledWidth, 0);
        mapOffsetY = scaledHeight <= viewport.clientHeight
            ? (viewport.clientHeight - scaledHeight) / 2
            : clamp(mapOffsetY, viewport.clientHeight - scaledHeight, 0);
    }

    function applyMapTransform() {
        clampMapPan();
        const canvas = byId('map-canvas');
        canvas.style.transform =
            `translate3d(${mapOffsetX}px, ${mapOffsetY}px, 0) scale(${mapScale})`;
        canvas.style.setProperty('--bgl-info-scale', String(1 / mapZoom));
        byId('map-zoom-label').textContent = `${Math.round(mapZoom * 100)}%`;
    }

    function setMapCenter(centerX, centerY) {
        const viewport = byId('map-viewport');
        const canvas = byId('map-canvas');
        mapScale = mapFitScale * mapZoom;
        mapOffsetX = viewport.clientWidth / 2 - centerX * canvas.offsetWidth * mapScale;
        mapOffsetY = viewport.clientHeight / 2 - centerY * canvas.offsetHeight * mapScale;
        applyMapTransform();
    }

    function fitMapProvince() {
        if (!updateMapFitScale()) return;

        mapZoom = 1;
        setMapCenter(
            (mapProvinceBounds.left + mapProvinceBounds.right) / 2,
            (mapProvinceBounds.top + mapProvinceBounds.bottom) / 2
        );
        mapInitialized = true;
    }

    function zoomMapAt(factor, clientX, clientY) {
        if (!mapInitialized) {
            fitMapProvince();
            if (!mapInitialized) return;
        }

        const viewport = byId('map-viewport');
        const viewportRect = viewport.getBoundingClientRect();
        const focalX = clientX === undefined
            ? viewport.clientWidth / 2
            : clientX - viewportRect.left;
        const focalY = clientY === undefined
            ? viewport.clientHeight / 2
            : clientY - viewportRect.top;
        const mapX = (focalX - mapOffsetX) / mapScale;
        const mapY = (focalY - mapOffsetY) / mapScale;

        mapZoom = clamp(mapZoom * factor, 0.72, 3);
        mapScale = mapFitScale * mapZoom;
        mapOffsetX = focalX - mapX * mapScale;
        mapOffsetY = focalY - mapY * mapScale;
        applyMapTransform();
    }

    function focusNpcOnMap(entry) {
        if (!entry || !entry.has_coordinates) {
            setDashboardStatus(`${entry ? entry.name : 'NPC'} has no saved map coordinates.`, true);
            return;
        }
        if (!mapInitialized && !updateMapFitScale()) return;

        mapZoom = Math.max(mapZoom, 1.45);
        setMapCenter(entry.percent_x / 100, entry.percent_y / 100);
        document.querySelectorAll('.npc-map-marker').forEach((marker) => {
            marker.classList.toggle('focused', marker.dataset.npc === entry.name);
        });
        window.setTimeout(() => {
            document.querySelectorAll('.npc-map-marker').forEach((marker) => {
                marker.classList.remove('focused');
            });
        }, 2200);
    }

    function renderNpcCards(npcs) {
        const list = byId('roster-list');
        list.replaceChildren();
        clearRemoveConfirmation();
        if (!npcs.length) {
            list.appendChild(createElement(
                'div',
                'empty-state',
                'No NPCs are currently enrolled in Background Life.'
            ));
            return;
        }

        npcs.forEach((entry) => {
            const card = createElement('article', 'npc-card');
            const npcColor = entry.color || '#f27c11';
            card.style.borderLeftColor = npcColor;
            const portrait = createElement('img', 'npc-portrait');
            portrait.src = resolveServerAssetUrl(entry.portrait_url);
            portrait.alt = '';
            portrait.addEventListener('error', () => {
                portrait.style.visibility = 'hidden';
            });
            card.appendChild(portrait);

            const body = createElement('div', 'npc-card-body');
            const title = createElement('div', 'npc-card-title');
            const name = createElement('div', 'npc-card-name');
            const color = createElement('span', 'npc-card-color');
            color.style.backgroundColor = npcColor;
            name.appendChild(color);
            name.appendChild(document.createTextNode(entry.name || 'Unknown NPC'));
            title.appendChild(name);
            const mapButton = createElement('button', 'card-action map', '🗺');
            mapButton.type = 'button';
            mapButton.title = entry.has_coordinates ? 'Show NPC on map' : 'No saved coordinates';
            mapButton.disabled = !entry.has_coordinates;
            mapButton.addEventListener('click', (event) => {
                event.stopPropagation();
                focusNpcOnMap(entry);
            });
            title.appendChild(mapButton);
            body.appendChild(title);
            body.appendChild(createElement(
                'div',
                'npc-card-meta',
                [entry.race, entry.location].filter(Boolean).join(' · ') || 'Location unknown'
            ));
            const activity = createElement('div', 'npc-card-activity');
            activity.appendChild(createElement(
                'span',
                'npc-card-activity-icon',
                entry.activity_icon || '✨'
            ));
            const activityCopy = createElement('div', 'npc-card-activity-copy');
            activityCopy.appendChild(createElement(
                'div',
                'npc-card-activity-title',
                `Last activity: ${entry.activity_label || 'Activity'}`
            ));
            const activitySummary = createElement(
                'div',
                'npc-card-activity-summary',
                entry.activity || 'No recent activity recorded.'
            );
            activitySummary.title = entry.activity || 'No recent activity recorded.';
            activityCopy.appendChild(activitySummary);
            activity.appendChild(activityCopy);
            body.appendChild(activity);
            card.appendChild(body);

            card.appendChild(createElement('div', 'npc-card-row-label', 'Actions'));
            const requests = createElement('div', 'npc-card-requests');
            requests.appendChild(cardRequestButton(entry, 'action', 'Trigger Action'));
            requests.appendChild(cardRequestButton(entry, 'letter', 'Send Letter'));
            card.appendChild(requests);

            card.appendChild(createElement('div', 'npc-card-row-label', 'Rules'));
            const actions = createElement('div', 'npc-card-actions');
            actions.appendChild(cardSettingButton(
                entry,
                'auto_actions',
                'auto_actions',
                'Actions',
                'Automatic actions'
            ));
            actions.appendChild(cardSettingButton(
                entry,
                'send_letters',
                'send_letters',
                'Letters',
                'Automatic letters'
            ));
            actions.appendChild(cardSettingButton(
                entry,
                'hourly_tracking',
                'hourly_tracking',
                'Tracking',
                'Hourly tracking'
            ));
            const remove = createElement('button', 'card-action remove', 'Remove');
            remove.type = 'button';
            remove.title = 'Remove NPC from Background Life';
            remove.addEventListener('click', (event) => {
                event.stopPropagation();
                removeNpc(entry, remove);
            });
            actions.appendChild(remove);
            card.appendChild(actions);
            card.addEventListener('click', () => window.openNpcDetailModal(entry.name));
            list.appendChild(card);
        });
    }

    function cardRequestButton(entry, requestType, label) {
        const button = createElement('button', 'card-action card-request', label);
        button.type = 'button';
        button.title = requestType === 'action'
            ? `Trigger a Background Life action for ${entry.name}`
            : `Send a Background Life letter from ${entry.name}`;
        button.addEventListener('click', async (event) => {
            event.stopPropagation();
            button.disabled = true;
            button.textContent = requestType === 'action' ? 'Triggering...' : 'Sending...';
            try {
                const payload = await postForm('/ui/api/background_life_request.php', {
                    request_type: requestType,
                    npc_name: entry.name || '',
                    refid: entry.refid || ''
                });
                setDashboardStatus(
                    `${label} completed for ${entry.name}. ${payload.message || ''}`.trim(),
                    false
                );
                await refreshDashboard();
            } catch (error) {
                setDashboardStatus(`${label} failed: ${error.message || error}`, true);
            } finally {
                button.disabled = false;
                button.textContent = label;
            }
        });
        return button;
    }

    function cardSettingButton(entry, setting, stateKey, label, title) {
        const button = createElement('button', 'card-action card-setting-toggle', label);
        button.type = 'button';
        const renderState = () => {
            const enabled = !!entry[stateKey];
            button.classList.toggle('is-enabled', enabled);
            button.setAttribute('aria-pressed', enabled ? 'true' : 'false');
            button.title = `${title}: ${enabled ? 'enabled' : 'disabled'}`;
        };
        renderState();
        button.addEventListener('click', async (event) => {
            event.stopPropagation();
            button.disabled = true;
            const nextValue = !entry[stateKey];
            try {
                const payload = await postForm('/ui/api/background_life_npc.php', {
                    operation: 'toggle',
                    setting,
                    value: nextValue ? '1' : '0',
                    npc_name: entry.name || '',
                    refid: entry.refid || ''
                });
                entry[stateKey] = !!(payload.data && payload.data[stateKey]);
                renderState();
                setDashboardStatus(
                    `${label} ${entry[stateKey] ? 'enabled' : 'disabled'} for ${entry.name}.`,
                    false
                );
                await refreshDashboard();
            } catch (error) {
                setDashboardStatus(`${label} failed: ${error.message || error}`, true);
            } finally {
                button.disabled = false;
            }
        });
        return button;
    }

    function clearRemoveConfirmation() {
        removeConfirmation = '';
        if (removeTimer) window.clearTimeout(removeTimer);
        removeTimer = null;
    }

    async function removeNpc(entry, button) {
        const key = entry.refid || entry.name;
        if (removeConfirmation !== key) {
            clearRemoveConfirmation();
            removeConfirmation = key;
            button.textContent = 'Confirm';
            button.classList.add('danger');
            removeTimer = window.setTimeout(() => {
                removeConfirmation = '';
                button.textContent = 'Remove';
                button.classList.remove('danger');
            }, 3500);
            return;
        }

        clearRemoveConfirmation();
        button.disabled = true;
        button.textContent = 'Removing...';
        try {
            const payload = await postForm('/ui/api/background_life_npc.php', {
                operation: 'disable',
                npc_name: entry.name || '',
                refid: entry.refid || ''
            });
            const formId = parseInt(String(entry.refid || '').replace(/^0x/i, ''), 16);
            if (Number.isFinite(formId) && formId > 0) {
                sendCommand(`roster_remove|${formId}`);
            }
            setDashboardStatus(payload.message || 'NPC removed.', false);
            await refreshDashboard();
        } catch (error) {
            button.disabled = false;
            button.textContent = 'Remove';
            setDashboardStatus(`Could not remove NPC: ${error.message || error}`, true);
        }
    }

    function targetParameters() {
        return new URLSearchParams({
            npc_name: currentTarget.name || '',
            refid: currentTarget.refid || ''
        });
    }

    function setTargetStatus(message, type) {
        const targetStatus = byId('target-status');
        targetStatus.textContent = message || '';
        targetStatus.classList.remove('error', 'success');
        if (type) targetStatus.classList.add(type);
    }

    function setTargetControlsBusy(busy) {
        const available =
            currentTarget.has_target &&
            currentTarget.exists &&
            currentTarget.background_life_enabled;
        byId('target-menu-toggle').disabled = busy || nearbyTargets.length === 0;
        byId('enrollment-button').disabled = busy || !currentTarget.has_target;
        ['auto-actions-toggle', 'send-letters-toggle', 'hourly-tracking-toggle'].forEach((id) => {
            byId(id).disabled = busy || !available;
        });
    }

    function renderTarget() {
        byId('target-name').textContent = currentTarget.has_target
            ? (currentTarget.name || 'Unknown NPC')
            : 'No activated NPCs nearby.';
        byId('target-refid').textContent = currentTarget.refid
            ? `RefID ${currentTarget.refid}`
            : 'RefID unavailable';
        const enabled = !!currentTarget.background_life_enabled;
        byId('target-state-badge').textContent = currentTarget.has_target
            ? (enabled ? 'Enabled' : 'Disabled')
            : 'No target';
        byId('target-state-badge').classList.toggle('enabled', enabled);
        byId('enrollment-button').textContent = enabled
            ? 'Disable Background Life'
            : 'Add to Background Life';
        byId('enrollment-button').classList.toggle('danger', enabled);
        byId('auto-actions-toggle').checked = !!currentTarget.auto_actions;
        byId('send-letters-toggle').checked = !!currentTarget.send_letters;
        byId('hourly-tracking-toggle').checked = !!currentTarget.hourly_tracking;
        setTargetControlsBusy(false);
    }

    function closeTargetMenu() {
        byId('target-menu-options').classList.add('hidden');
        byId('target-menu-toggle').setAttribute('aria-expanded', 'false');
    }

    function renderTargetOptions(selectedFormId) {
        const options = byId('target-menu-options');
        options.replaceChildren();
        let selectedLabel = '';

        nearbyTargets.forEach((target) => {
            const option = createElement('button', 'target-option-tile');
            option.type = 'button';
            option.dataset.formId = String(target.form_id || '');
            option.appendChild(document.createTextNode(target.name || 'Unknown NPC'));
            const distance = Number(target.distance);
            if (Number.isFinite(distance)) {
                option.appendChild(createElement(
                    'span',
                    'target-option-distance',
                    `${distance.toFixed(1)}m`
                ));
            }
            const selected = option.dataset.formId === String(selectedFormId || '');
            option.classList.toggle('is-active', selected);
            if (selected) {
                selectedLabel = Number.isFinite(distance)
                    ? `${target.name} (${distance.toFixed(1)}m)`
                    : target.name;
            }
            option.addEventListener('click', () => chooseNearbyTarget(target));
            options.appendChild(option);
        });

        byId('target-menu-label').textContent =
            selectedLabel ||
            (nearbyTargets.length ? 'Choose nearby NPC' : 'No activated NPCs nearby');
        byId('target-menu-toggle').disabled = nearbyTargets.length === 0;
    }

    function chooseNearbyTarget(target) {
        currentTarget = Object.assign(emptyTarget(), {
            has_target: true,
            name: target.name || '',
            refid: target.refid || '',
            selected_form_id: target.form_id || 0,
            game_enrolled: !!target.game_enrolled,
            background_life_enabled: !!target.game_enrolled
        });
        renderTargetOptions(target.form_id);
        renderTarget();
        setTargetControlsBusy(true);
        setTargetStatus('Loading selected NPC...', '');
        closeTargetMenu();
        sendCommand(`target_select|${target.form_id}`);
    }

    async function refreshTargetStatus(quiet) {
        const generation = ++targetGeneration;
        if (!currentTarget.has_target) {
            renderTarget();
            return;
        }
        if (!quiet) setTargetStatus('Loading NPC settings...', '');

        try {
            const payload = await parseJsonResponse(await fetch(
                `${serverBaseUrl}/ui/api/background_life_npc.php?${targetParameters()}`,
                { cache: 'no-store' }
            ));
            if (generation !== targetGeneration) return;
            currentTarget = Object.assign({}, currentTarget, payload.data || {});
            renderTarget();
            if (!quiet) {
                setTargetStatus(
                    currentTarget.exists
                        ? 'NPC settings loaded.'
                        : 'NPC has not been discovered by CHIM.',
                    currentTarget.exists ? 'success' : 'error'
                );
            }
        } catch (error) {
            if (generation !== targetGeneration) return;
            setTargetStatus(`Could not load NPC settings: ${error.message || error}`, 'error');
            renderTarget();
        }
    }

    async function updateTargetSetting(setting, value) {
        setTargetControlsBusy(true);
        try {
            const body = targetParameters();
            body.set('operation', 'toggle');
            body.set('setting', setting);
            body.set('value', value ? '1' : '0');
            const payload = await postForm('/ui/api/background_life_npc.php', body);
            currentTarget = Object.assign({}, currentTarget, payload.data || {});
            renderTarget();
            setTargetStatus(payload.message || 'NPC setting saved.', 'success');
            refreshDashboard();
        } catch (error) {
            renderTarget();
            setTargetStatus(`Could not save NPC setting: ${error.message || error}`, 'error');
        }
    }

    window.updateBackgroundLifeTarget = function (payload) {
        try {
            const next = typeof payload === 'string' ? JSON.parse(payload) : payload;
            nearbyTargets = Array.isArray(next && next.targets) ? next.targets : [];
            currentPlayerLocation = {
                formid: String((next && next.player_location_formid) || ''),
                name: String((next && next.player_location_name) || '')
            };
            currentTarget = Object.assign(emptyTarget(), next || {});
            renderTargetOptions(next && next.selected_form_id);
            renderTarget();
            setTargetStatus('', '');
            refreshTargetStatus(false);
        } catch (error) {
            setTargetStatus('Invalid target data received from CHIM.', 'error');
        }
    };

    function renderNpcOptions(npcs) {
        const selected = byId('npc-filter').value;
        npcFilterDropdown.setOptions(
            [{ value: '', label: 'All NPCs' }].concat((npcs || []).map((npc) => ({
                value: npc.name,
                label: `${npc.name} (${npc.count})`
            }))),
            selected
        );
    }

    function renderHistoryEntries(entries) {
        const list = byId('activity-list');
        list.replaceChildren();
        if (!entries || !entries.length) {
            list.appendChild(createElement(
                'div',
                'empty-state',
                'No Background Life activity matches these filters.'
            ));
            return;
        }

        entries.forEach((entry) => {
            const item = createElement('article', 'activity-entry');
            item.tabIndex = 0;
            item.appendChild(createElement(
                'div',
                'activity-time',
                entry.tamrielic_time || 'Unknown time'
            ));
            item.appendChild(createElement(
                'div',
                'activity-npc',
                entry.npc || 'Unknown NPC'
            ));
            const summary = createElement('div', 'activity-summary');
            summary.appendChild(createElement(
                'span',
                'category-badge',
                entry.category || 'activity'
            ));
            summary.appendChild(createElement(
                'span',
                'activity-text',
                entry.activity || 'No details recorded'
            ));
            item.appendChild(summary);
            item.appendChild(createElement(
                'div',
                'activity-details',
                [
                    entry.server_time,
                    entry.rowid ? `History ID ${entry.rowid}` : ''
                ].filter(Boolean).join(' | ')
            ));

            const toggle = () => item.classList.toggle('expanded');
            item.addEventListener('click', toggle);
            item.addEventListener('keydown', (event) => {
                if (event.key === 'Enter' || event.key === ' ') {
                    event.preventDefault();
                    toggle();
                }
            });
            list.appendChild(item);
        });
    }

    function requestHistory() {
        byId('panel-status').textContent = 'Loading activity...';
        const params = new URLSearchParams({
            page: String(currentPage),
            limit: byId('limit-select').value || '20'
        });
        if (byId('npc-filter').value) params.set('npc', byId('npc-filter').value);
        if (byId('search-input').value.trim()) {
            params.set('search', byId('search-input').value.trim());
        }
        sendCommand(`fetch|${params}`);
    }

    window.updateBackgroundLifeHistory = function (payload) {
        try {
            const data = typeof payload === 'string' ? JSON.parse(payload) : payload;
            if (!data || !data.success) {
                throw new Error((data && data.error) || 'Unable to load activity');
            }
            renderNpcOptions(data.npcs);
            renderHistoryEntries(data.entries);
            const pagination = data.pagination || {};
            currentPage = pagination.current_page || 1;
            totalPages = pagination.total_pages || 1;
            byId('previous-button').disabled = currentPage <= 1;
            byId('next-button').disabled = currentPage >= totalPages;
            byId('page-label').textContent = `Page ${currentPage} of ${totalPages}`;
            const total = pagination.total_records === undefined
                ? data.entries.length
                : pagination.total_records;
            byId('panel-status').textContent =
                `${total} activit${total === 1 ? 'y' : 'ies'}`;
            byId('panel-status').classList.remove('error');
        } catch (error) {
            renderHistoryEntries([]);
            byId('panel-status').textContent = error.message || 'Unable to load activity';
            byId('panel-status').classList.add('error');
        }
    };

    window.showBackgroundLifeError = function (message) {
        renderHistoryEntries([]);
        byId('panel-status').textContent = message || 'Unable to load activity';
        byId('panel-status').classList.add('error');
    };

    async function refreshRumors() {
        const generation = ++rumorGeneration;
        byId('rumors-status').textContent = 'Loading rumors...';
        try {
            const payload = await parseJsonResponse(await fetch(
                `${serverBaseUrl}/ui/api/background_life_rumors.php`,
                { cache: 'no-store' }
            ));
            if (generation !== rumorGeneration) return;
            const data = payload.data || {};
            if (Array.isArray(data.holds) && data.holds.length) {
                rumorHoldDropdown.setOptions(
                    data.holds.map((hold) => ({ value: hold, label: hold })),
                    ''
                );
            }
            renderRumorList(byId('current-rumors-list'), data.current || [], false);
            renderRumorList(byId('outdated-rumors-list'), data.outdated || [], true);
            byId('rumors-status').textContent =
                `${(data.current || []).length} current, ${(data.outdated || []).length} outdated`;
            byId('rumors-status').classList.remove('error');
        } catch (error) {
            byId('rumors-status').textContent =
                `Could not load rumors: ${error.message || error}`;
            byId('rumors-status').classList.add('error');
        }
    }

    function renderRumorList(container, entries, outdated) {
        container.replaceChildren();
        if (!entries.length) {
            container.appendChild(createElement(
                'div',
                'empty-state',
                outdated ? 'No outdated rumors.' : 'No current rumors.'
            ));
            return;
        }

        entries.forEach((entry) => {
            const card = createElement('article', 'rumor-card');
            card.appendChild(createElement(
                'h4',
                '',
                `${entry.hold || 'Skyrim'} · ${entry.type || 'General'}`
            ));
            card.appendChild(createElement('p', '', entry.content || ''));
            card.appendChild(createElement(
                'div',
                'rumor-meta',
                `${entry.tamrielic_time || 'Unknown date'} · ${entry.length_days} days`
            ));
            const actions = createElement('div', 'rumor-actions');
            const edit = createElement('button', 'rumor-action', 'Edit');
            edit.type = 'button';
            edit.addEventListener('click', () => window.openRumorModal(entry));
            const remove = createElement('button', 'rumor-action danger', 'Delete');
            remove.type = 'button';
            remove.addEventListener('click', () => deleteRumor(entry, remove));
            actions.append(edit, remove);
            card.appendChild(actions);
            container.appendChild(card);
        });
    }

    async function deleteRumor(entry, button) {
        if (button.dataset.confirm !== '1') {
            button.dataset.confirm = '1';
            button.textContent = 'Confirm';
            window.setTimeout(() => {
                button.dataset.confirm = '';
                button.textContent = 'Delete';
            }, 3500);
            return;
        }
        button.disabled = true;
        try {
            await postForm('/ui/api/background_life_rumors.php', {
                operation: 'delete',
                id: entry.id
            });
            refreshRumors();
        } catch (error) {
            byId('rumors-status').textContent =
                `Could not delete rumor: ${error.message || error}`;
            byId('rumors-status').classList.add('error');
            button.disabled = false;
        }
    }

    window.openRumorModal = function (entry) {
        const form = byId('rumor-create-form');
        form.reset();
        const editing = !!(entry && entry.id);
        byId('rumor-id').value = editing ? String(entry.id) : '';
        rumorHoldDropdown.select(
            editing ? entry.hold : '',
            editing ? entry.hold : 'Select hold',
            false
        );
        byId('rumor-type-input').value = editing ? entry.type : '';
        byId('rumor-length-days-input').value = editing ? entry.length_days : 7;
        byId('rumor-content-input').value = editing ? entry.content : '';
        byId('rumor-modal-title').textContent = editing ? 'Edit Rumor' : 'Create Rumor';
        byId('rumor-submit-button').textContent =
            editing ? 'Save Rumor' : 'Create Rumor';
        byId('rumor-form-status').textContent = '';
        byId('rumor-modal-overlay').classList.remove('hidden');
        byId('rumor-modal-overlay').setAttribute('aria-hidden', 'false');
    };

    window.closeRumorModal = function () {
        byId('rumor-modal-overlay').classList.add('hidden');
        byId('rumor-modal-overlay').setAttribute('aria-hidden', 'true');
        closeTileDropdowns();
    };

    window.submitRumorForm = async function (event) {
        if (event) event.preventDefault();
        if (rumorBusy) return;
        const hold = String(byId('rumor-hold-select').value || '').trim();
        const content = String(byId('rumor-content-input').value || '').trim();
        const lengthDaysRaw = String(byId('rumor-length-days-input').value || '').trim();
        const lengthDays = lengthDaysRaw || '7';
        if (!hold) {
            byId('rumor-form-status').textContent = 'Select a hold for this rumor.';
            byId('rumor-form-status').className = 'rumor-form-status error';
            return;
        }
        if (!content) {
            byId('rumor-form-status').textContent = 'Enter rumor content.';
            byId('rumor-form-status').className = 'rumor-form-status error';
            return;
        }
        if (!/^\d+$/.test(lengthDays) || Number(lengthDays) < 1) {
            byId('rumor-form-status').textContent =
                'Length (Days) must be a whole number of 1 or more.';
            byId('rumor-form-status').className = 'rumor-form-status error';
            return;
        }

        rumorBusy = true;
        const id = byId('rumor-id').value;
        const button = byId('rumor-submit-button');
        button.disabled = true;
        button.textContent = 'Saving...';
        try {
            await postForm('/ui/api/background_life_rumors.php', {
                operation: id ? 'update' : 'create',
                id,
                hold,
                type: byId('rumor-type-input').value,
                content,
                length_days: lengthDays
            });
            window.closeRumorModal();
            await refreshRumors();
        } catch (error) {
            byId('rumor-form-status').textContent =
                `Could not save rumor: ${error.message || error}`;
            byId('rumor-form-status').className = 'rumor-form-status error';
        } finally {
            rumorBusy = false;
            button.disabled = false;
            button.textContent = id ? 'Save Rumor' : 'Create Rumor';
        }
    };

    window.openNpcDetailModal = async function (npcName) {
        detailData = null;
        detailTab = 'events';
        byId('npc-detail-title').textContent = npcName;
        byId('npc-detail-status').textContent = 'Loading NPC history...';
        byId('npc-detail-status').classList.remove('error');
        byId('npc-detail-content').replaceChildren();
        byId('npc-detail-overlay').classList.remove('hidden');
        byId('npc-detail-overlay').setAttribute('aria-hidden', 'false');
        renderDetailTabs();
        try {
            const payload = await parseJsonResponse(await fetch(
                `${serverBaseUrl}/ui/api/background_life_npc_detail.php?npc=${encodeURIComponent(npcName)}`,
                { cache: 'no-store' }
            ));
            detailData = payload.data || {};
            byId('npc-detail-status').textContent = '';
            renderNpcDetail();
        } catch (error) {
            byId('npc-detail-status').textContent =
                `Could not load NPC history: ${error.message || error}`;
            byId('npc-detail-status').classList.add('error');
        }
    };

    window.closeNpcDetailModal = function () {
        byId('npc-detail-overlay').classList.add('hidden');
        byId('npc-detail-overlay').setAttribute('aria-hidden', 'true');
    };

    function renderDetailTabs() {
        detailTabs.forEach((tab) => {
            tab.classList.toggle('active', tab.dataset.detailTab === detailTab);
        });
    }

    function renderNpcDetail() {
        renderDetailTabs();
        const entries = (detailData && detailData[detailTab]) || [];
        const content = byId('npc-detail-content');
        content.replaceChildren();
        if (!entries.length) {
            content.appendChild(createElement(
                'div',
                'empty-state',
                `No ${detailTab === 'events' ? 'events' : detailTab} recorded.`
            ));
            return;
        }
        entries.forEach((entry) => {
            const row = createElement('article', 'detail-entry');
            row.appendChild(createElement(
                'time',
                '',
                entry.tamrielic_time || 'Unknown time'
            ));
            row.appendChild(createElement(
                'h4',
                '',
                detailTab === 'events'
                    ? (entry.category || 'Activity')
                    : (entry.topic || 'Entry')
            ));
            row.appendChild(createElement(
                'p',
                '',
                detailTab === 'events' ? entry.activity : entry.content
            ));
            content.appendChild(row);
        });
    }

    function optionEntries(values) {
        return (values || []).map((value) => ({ value, label: value }));
    }

    function normalizeFormId(value) {
        return String(value || '').trim().replace(/^0x/i, '').replace(/^0+/, '').toUpperCase();
    }

    function applyNpcCreationOptions(options) {
        const defaults = options.defaults || {};
        const locations = options.locations || [];
        const playerLocationId = normalizeFormId(currentPlayerLocation.formid);
        const playerLocationName = String(currentPlayerLocation.name || '').trim().toLowerCase();
        const playerLocation = locations.find((location) => (
            playerLocationId !== '' && normalizeFormId(location.formid) === playerLocationId
        )) || locations.find((location) => (
            playerLocationName !== '' && String(location.name || '').trim().toLowerCase() === playerLocationName
        ));
        byId('npc-create-form').reset();
        npcCreateGenderDropdown.setOptions(
            optionEntries(options.genders),
            defaults.gender || 'male'
        );
        npcCreateRaceDropdown.setOptions(
            optionEntries(options.races),
            defaults.race || 'Nord'
        );
        npcCreateClassDropdown.setOptions(
            optionEntries(options.classes),
            defaults.class || 'farmer'
        );
        npcCreateLocationDropdown.setOptions(
            [{ value: '', label: 'Select discovered location' }].concat(
                locations.map((location) => ({
                    value: location.formid,
                    label: location.label || location.name
                }))
            ),
            (playerLocation && playerLocation.formid) || defaults.location || ''
        );
        byId('npc-create-disposition').value = defaults.disposition || 'friendly';
        byId('npc-create-gold').value = defaults.gold_qty || '100';
    }

    window.openNpcCreateModal = async function () {
        byId('npc-create-modal-overlay').classList.remove('hidden');
        byId('npc-create-modal-overlay').setAttribute('aria-hidden', 'false');
        byId('npc-create-form-status').textContent = 'Loading NPC creation options...';
        byId('npc-create-submit-button').disabled = true;
        try {
            const payload = await parseJsonResponse(await fetch(
                `${serverBaseUrl}/ui/api/background_life_npc_create.php`,
                { cache: 'no-store' }
            ));
            applyNpcCreationOptions(payload.data || {});
            byId('npc-create-form-status').textContent = '';
            byId('npc-create-form-status').className = 'rumor-form-status';
            byId('npc-create-submit-button').disabled = false;
        } catch (error) {
            byId('npc-create-form-status').textContent =
                `Could not load options: ${error.message || error}`;
            byId('npc-create-form-status').className = 'rumor-form-status error';
        }
    };

    window.closeNpcCreateModal = function () {
        if (npcCreateBusy) return;
        byId('npc-create-modal-overlay').classList.add('hidden');
        byId('npc-create-modal-overlay').setAttribute('aria-hidden', 'true');
        closeTileDropdowns();
    };

    window.submitNpcCreateForm = async function (event) {
        if (event) event.preventDefault();
        if (npcCreateBusy) return;
        const requiredFields = [
            ['npc-create-name', 'Name'],
            ['npc-create-location', 'Location'],
            ['npc-create-background', 'Background'],
            ['npc-create-speech-style', 'Speech Style'],
            ['npc-create-goal', 'Goals']
        ];
        const missingFields = requiredFields
            .filter(([id]) => !String(byId(id).value || '').trim())
            .map(([, label]) => label);
        if (missingFields.length) {
            byId('npc-create-form-status').textContent =
                `Complete the required fields: ${missingFields.join(', ')}.`;
            byId('npc-create-form-status').className = 'rumor-form-status error';
            return;
        }

        npcCreateBusy = true;
        byId('npc-create-submit-button').disabled = true;
        byId('npc-create-submit-button').textContent = 'Creating NPC...';
        byId('npc-create-form-status').textContent =
            'Creating NPC in Skyrim. This can take up to one minute...';
        byId('npc-create-form-status').className = 'rumor-form-status';
        sendCommand('close');
        try {
            const payload = await postForm('/ui/api/background_life_npc_create.php', {
                npc_name: byId('npc-create-name').value,
                npc_gender: byId('npc-create-gender').value,
                npc_race: byId('npc-create-race').value,
                npc_class: byId('npc-create-class').value,
                npc_location: byId('npc-create-location').value,
                npc_background: byId('npc-create-background').value,
                npc_speech_style: byId('npc-create-speech-style').value,
                npc_goal: byId('npc-create-goal').value,
                npc_appearance: byId('npc-create-appearance').value,
                npc_disposition: byId('npc-create-disposition').value,
                npc_starting_point: byId('npc-create-starting-point').value,
                npc_inventory_gold: byId('npc-create-gold').value
            });
            byId('npc-create-form-status').textContent =
                payload.message || 'NPC created.';
            byId('npc-create-form-status').className = 'rumor-form-status success';
            await refreshDashboard();
            sendCommand('target_refresh');
            window.setTimeout(() => {
                npcCreateBusy = false;
                window.closeNpcCreateModal();
            }, 650);
        } catch (error) {
            byId('npc-create-form-status').textContent =
                `Create NPC failed: ${error.message || error}`;
            byId('npc-create-form-status').className = 'rumor-form-status error';
        } finally {
            npcCreateBusy = false;
            byId('npc-create-submit-button').disabled = false;
            byId('npc-create-submit-button').textContent = 'Create NPC';
        }
    };

    async function refreshActivePage() {
        sendCommand('target_refresh');
        if (activePage === 'dashboard') await refreshDashboard();
        if (activePage === 'history') requestHistory();
        if (activePage === 'rumors') await refreshRumors();
    }

    window.setBackgroundLifeServerUrl = function (value) {
        serverBaseUrl = normalizeServerBaseUrl(value);
    };

    window.onBackgroundLifeShown = function () {
        closeTargetMenu();
        closeTileDropdowns();
        refreshActivePage();
    };

    window.closePanel = function () {
        sendCommand('close');
    };

    pageTabs.forEach((tab) => {
        tab.addEventListener('click', () => switchPage(tab.dataset.tab));
    });
    detailTabs.forEach((tab) => {
        tab.addEventListener('click', () => {
            detailTab = tab.dataset.detailTab;
            renderNpcDetail();
        });
    });
    byId('refresh-button').addEventListener('click', refreshActivePage);
    byId('show-all-coords-toggle').addEventListener('change', refreshDashboard);

    byId('save-trigger-hours').addEventListener('click', async () => {
        try {
            const payload = await postForm('/ui/api/background_life_dashboard.php', {
                operation: 'save_settings',
                trigger_hours: byId('trigger-hours-input').value || '24'
            });
            setDashboardStatus(payload.message || 'Trigger time saved.', false);
        } catch (error) {
            setDashboardStatus(
                `Could not save trigger time: ${error.message || error}`,
                true
            );
        }
    });

    byId('update-all-coords').addEventListener('click', async () => {
        const button = byId('update-all-coords');
        button.disabled = true;
        button.textContent = 'Updating...';
        try {
            const payload = await postForm('/ui/api/background_life_request.php', {
                request_type: 'track_all'
            });
            setDashboardStatus(payload.message || 'Coordinates updated.', false);
            await refreshDashboard();
        } catch (error) {
            setDashboardStatus(
                `Coordinate update failed: ${error.message || error}`,
                true
            );
        } finally {
            button.disabled = false;
            button.textContent = 'Update All NPC Coords';
        }
    });

    byId('map-zoom-in').addEventListener('click', () => zoomMapAt(1.25));
    byId('map-zoom-out').addEventListener('click', () => zoomMapAt(0.8));
    byId('map-reset').addEventListener('click', fitMapProvince);
    byId('map-viewport').addEventListener('wheel', (event) => {
        event.preventDefault();
        zoomMapAt(
            event.deltaY < 0 ? 1.12 : 1 / 1.12,
            event.clientX,
            event.clientY
        );
    }, { passive: false });
    byId('map-viewport').addEventListener('mousedown', (event) => {
        if (
            event.button !== 0
            || event.target.closest('.location-marker, .npc-map-marker, .map-navigation')
        ) {
            return;
        }
        mapDragging = true;
        mapDragStart = {
            x: event.clientX - mapOffsetX,
            y: event.clientY - mapOffsetY
        };
        byId('map-viewport').classList.add('dragging');
    });
    document.addEventListener('mousemove', (event) => {
        if (!mapDragging) return;
        mapOffsetX = event.clientX - mapDragStart.x;
        mapOffsetY = event.clientY - mapDragStart.y;
        applyMapTransform();
    });
    document.addEventListener('mouseup', () => {
        mapDragging = false;
        byId('map-viewport').classList.remove('dragging');
    });
    window.addEventListener('resize', () => {
        if (!mapInitialized || !updateMapFitScale()) return;
        setMapCenter(
            (mapProvinceBounds.left + mapProvinceBounds.right) / 2,
            (mapProvinceBounds.top + mapProvinceBounds.bottom) / 2
        );
    });

    function isTextEntry(element) {
        return element instanceof HTMLElement
            && element.matches(
                'textarea, input:not([type="checkbox"]):not([type="radio"]):not([type="hidden"]), [contenteditable="true"]'
            );
    }

    document.addEventListener('focusin', (event) => {
        if (isTextEntry(event.target)) {
            sendCommand('input_capture|on');
        }
    });
    document.addEventListener('focusout', () => {
        window.setTimeout(() => {
            if (!isTextEntry(document.activeElement)) {
                sendCommand('input_capture|off');
            }
        }, 0);
    });

    byId('target-menu-toggle').addEventListener('click', () => {
        if (byId('target-menu-toggle').disabled) return;
        const opening = byId('target-menu-options').classList.contains('hidden');
        closeTileDropdowns();
        byId('target-menu-options').classList.toggle('hidden', !opening);
        byId('target-menu-toggle').setAttribute(
            'aria-expanded',
            opening ? 'true' : 'false'
        );
    });
    byId('enrollment-button').addEventListener('click', () => {
        if (!currentTarget.has_target) return;
        const enable = !currentTarget.background_life_enabled;
        currentTarget.game_enrolled = enable;
        currentTarget.background_life_enabled = enable;
        renderTarget();
        setTargetStatus(enable ? 'Adding NPC...' : 'Disabling NPC...', '');
        sendCommand(`enrollment|${enable ? 'enable' : 'disable'}`);
        [400, 1100, 2300].forEach((delay) => {
            window.setTimeout(() => {
                refreshTargetStatus(true);
                refreshDashboard();
            }, delay);
        });
    });
    byId('auto-actions-toggle').addEventListener('change', () => {
        updateTargetSetting('auto_actions', byId('auto-actions-toggle').checked);
    });
    byId('send-letters-toggle').addEventListener('change', () => {
        updateTargetSetting('send_letters', byId('send-letters-toggle').checked);
    });
    byId('hourly-tracking-toggle').addEventListener('change', () => {
        updateTargetSetting(
            'hourly_tracking',
            byId('hourly-tracking-toggle').checked
        );
    });
    byId('npc-filter').addEventListener('change', () => {
        currentPage = 1;
        requestHistory();
    });
    byId('limit-select').addEventListener('change', () => {
        currentPage = 1;
        requestHistory();
    });
    byId('search-input').addEventListener('input', () => {
        window.clearTimeout(searchTimer);
        searchTimer = window.setTimeout(() => {
            currentPage = 1;
            requestHistory();
        }, 300);
    });
    byId('previous-button').addEventListener('click', () => {
        if (currentPage > 1) {
            currentPage -= 1;
            requestHistory();
        }
    });
    byId('next-button').addEventListener('click', () => {
        if (currentPage < totalPages) {
            currentPage += 1;
            requestHistory();
        }
    });

    document.addEventListener('click', (event) => {
        const targetOptions = byId('target-menu-options');
        if (
            !targetOptions.classList.contains('hidden') &&
            !targetOptions.contains(event.target) &&
            !byId('target-menu-toggle').contains(event.target)
        ) {
            closeTargetMenu();
        }
        tileDropdowns.forEach((dropdown) => {
            if (
                !dropdown.options.classList.contains('hidden') &&
                !dropdown.options.contains(event.target) &&
                !dropdown.toggle.contains(event.target)
            ) {
                dropdown.close();
            }
        });
    });

    document.addEventListener('keydown', (event) => {
        if (event.key !== 'Escape') return;
        event.preventDefault();
        const overlays = [
            'npc-detail-overlay',
            'npc-create-modal-overlay',
            'rumor-modal-overlay'
        ];
        const openOverlay = overlays.find((id) => {
            return !byId(id).classList.contains('hidden');
        });
        if (openOverlay === 'npc-detail-overlay') {
            window.closeNpcDetailModal();
            return;
        }
        if (openOverlay === 'npc-create-modal-overlay') {
            window.closeNpcCreateModal();
            return;
        }
        if (openOverlay === 'rumor-modal-overlay') {
            window.closeRumorModal();
            return;
        }
        const openDropdown = tileDropdowns.find((dropdown) => {
            return !dropdown.options.classList.contains('hidden');
        });
        if (openDropdown) {
            openDropdown.close();
            return;
        }
        if (!byId('target-menu-options').classList.contains('hidden')) {
            closeTargetMenu();
            return;
        }
        window.closePanel();
    });

    document.addEventListener('DOMContentLoaded', () => {
        limitDropdown.select('20', '20', false);
        renderTarget();
        sendCommand('dom_ready');
    });
}());
