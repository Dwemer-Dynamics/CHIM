(function () {
    'use strict';

    const modal = document.getElementById('background-life-modal');
    const activityList = document.getElementById('activity-list');
    const status = document.getElementById('panel-status');
    const npcFilter = document.getElementById('npc-filter');
    const npcFilterToggle = document.getElementById('npc-filter-toggle');
    const npcFilterLabel = document.getElementById('npc-filter-label');
    const npcFilterOptions = document.getElementById('npc-filter-options');
    const searchInput = document.getElementById('search-input');
    const limitSelect = document.getElementById('limit-select');
    const limitSelectToggle = document.getElementById('limit-select-toggle');
    const limitSelectLabel = document.getElementById('limit-select-label');
    const limitSelectOptions = document.getElementById('limit-select-options');
    const refreshButton = document.getElementById('refresh-button');
    const previousButton = document.getElementById('previous-button');
    const nextButton = document.getElementById('next-button');
    const pageLabel = document.getElementById('page-label');
    const targetName = document.getElementById('target-name');
    const targetRefid = document.getElementById('target-refid');
    const targetMenuToggle = document.getElementById('target-menu-toggle');
    const targetMenuLabel = document.getElementById('target-menu-label');
    const targetMenuOptions = document.getElementById('target-menu-options');
    const targetStateBadge = document.getElementById('target-state-badge');
    const targetStatus = document.getElementById('target-status');
    const enrollmentButton = document.getElementById('enrollment-button');
    const autoActionsToggle = document.getElementById('auto-actions-toggle');
    const sendLettersToggle = document.getElementById('send-letters-toggle');
    const hourlyTrackingToggle = document.getElementById('hourly-tracking-toggle');
    const triggerActionButton = document.getElementById('trigger-action-button');
    const requestLetterButton = document.getElementById('request-letter-button');
    const rosterList = document.getElementById('roster-list');
    const rosterStatus = document.getElementById('roster-status');
    const rosterCount = document.getElementById('roster-count');
    const rumorHoldSelect = document.getElementById('rumor-hold-select');
    const rumorHoldSelectToggle = document.getElementById('rumor-hold-select-toggle');
    const rumorHoldSelectLabel = document.getElementById('rumor-hold-select-label');
    const rumorHoldSelectOptions = document.getElementById('rumor-hold-select-options');

    let currentPage = 1;
    let totalPages = 1;
    let searchTimer = null;
    let rumorCreateInFlight = false;
    let rosterRequestGeneration = 0;
    let rosterRemoveConfirmKey = '';
    let rosterRemoveConfirmTimer = null;
    let serverBaseUrl = 'http://127.0.0.1:8081/HerikaServer';
    let currentTarget = {
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
    let nearbyTargets = [];
    let targetRequestGeneration = 0;
    const tileDropdowns = [];

    function sendCommand(command) {
        if (window.chimBackgroundLifeCommand) {
            window.chimBackgroundLifeCommand(command);
        }
    }

    function createElement(tagName, className, text) {
        const element = document.createElement(tagName);
        if (className) {
            element.className = className;
        }
        if (text !== undefined) {
            element.textContent = text;
        }
        return element;
    }

    function closeTileDropdowns(except) {
        tileDropdowns.forEach(function (dropdown) {
            if (dropdown !== except) {
                dropdown.close();
            }
        });
    }

    function createTileDropdown(input, toggle, label, options) {
        const dropdown = {
            input: input,
            toggle: toggle,
            label: label,
            options: options,
            close: function () {
                options.classList.add('hidden');
                toggle.setAttribute('aria-expanded', 'false');
            },
            select: function (value, displayText, notify) {
                input.value = String(value);
                label.textContent = displayText;
                options.querySelectorAll('[data-value]').forEach(function (option) {
                    const selected = option.dataset.value === input.value;
                    option.classList.toggle('is-active', selected);
                    option.setAttribute('aria-selected', selected ? 'true' : 'false');
                });
                dropdown.close();
                if (notify !== false) {
                    input.dispatchEvent(new Event('change', { bubbles: true }));
                }
            },
            setOptions: function (entries, preferredValue) {
                options.replaceChildren();
                entries.forEach(function (entry) {
                    const option = document.createElement('button');
                    option.type = 'button';
                    option.className = 'target-option-tile';
                    option.setAttribute('role', 'option');
                    option.dataset.value = String(entry.value);
                    option.textContent = entry.label;
                    options.appendChild(option);
                });

                const preferred = String(preferredValue === undefined ? input.value : preferredValue);
                const selectedOption = Array.from(options.querySelectorAll('[data-value]')).find(function (option) {
                    return option.dataset.value === preferred;
                }) || options.querySelector('[data-value]');
                if (selectedOption) {
                    dropdown.select(selectedOption.dataset.value, selectedOption.textContent, false);
                }
            }
        };

        toggle.addEventListener('click', function () {
            const opening = options.classList.contains('hidden');
            closeTargetMenu();
            closeTileDropdowns(dropdown);
            options.classList.toggle('hidden', !opening);
            toggle.setAttribute('aria-expanded', opening ? 'true' : 'false');
        });
        options.addEventListener('click', function (event) {
            const option = event.target.closest('[data-value]');
            if (!option || !options.contains(option)) {
                return;
            }
            dropdown.select(option.dataset.value, option.textContent, true);
        });
        tileDropdowns.push(dropdown);
        return dropdown;
    }

    const npcFilterDropdown = createTileDropdown(
        npcFilter,
        npcFilterToggle,
        npcFilterLabel,
        npcFilterOptions
    );
    const limitDropdown = createTileDropdown(
        limitSelect,
        limitSelectToggle,
        limitSelectLabel,
        limitSelectOptions
    );
    const rumorHoldDropdown = createTileDropdown(
        rumorHoldSelect,
        rumorHoldSelectToggle,
        rumorHoldSelectLabel,
        rumorHoldSelectOptions
    );

    function setLoading(loading) {
        modal.classList.toggle('loading', loading);
        refreshButton.disabled = loading;
    }

    function setStatus(message, isError) {
        status.textContent = message;
        status.classList.toggle('error', !!isError);
    }

    function normalizeServerBaseUrl(value) {
        let baseUrl = String(value || '').trim().replace(/\/+$/, '');
        if (!/\/HerikaServer$/i.test(baseUrl)) {
            baseUrl += '/HerikaServer';
        }
        return baseUrl;
    }

    function setTargetStatus(message, type) {
        targetStatus.textContent = message || '';
        targetStatus.classList.remove('error', 'success');
        if (type === 'error' || type === 'success') {
            targetStatus.classList.add(type);
        }
    }

    function setTargetControlsBusy(busy) {
        const available = currentTarget.has_target && currentTarget.exists && currentTarget.background_life_enabled;
        targetMenuToggle.disabled = busy || nearbyTargets.length === 0;
        enrollmentButton.disabled = busy || !currentTarget.has_target;
        autoActionsToggle.disabled = busy || !available;
        sendLettersToggle.disabled = busy || !available;
        hourlyTrackingToggle.disabled = busy || !available;
        triggerActionButton.disabled = busy || !available;
        requestLetterButton.disabled = busy || !available;
    }

    function closeTargetMenu() {
        targetMenuOptions.classList.add('hidden');
        targetMenuToggle.setAttribute('aria-expanded', 'false');
    }

    function chooseNearbyTarget(target) {
        if (!target) {
            return;
        }

        targetRequestGeneration += 1;
        currentTarget = Object.assign({}, currentTarget, {
            has_target: true,
            name: target.name || '',
            refid: target.refid || '',
            selected_form_id: target.form_id || 0,
            game_enrolled: !!target.game_enrolled,
            exists: false,
            background_life_enabled: !!target.game_enrolled,
            auto_actions: false,
            send_letters: false,
            hourly_tracking: false
        });
        renderTargetOptions(target.form_id);
        renderTarget();
        setTargetControlsBusy(true);
        setTargetStatus('Loading selected NPC...', '');
        closeTargetMenu();
        sendCommand(`target_select|${target.form_id}`);
    }

    function renderTargetOptions(selectedFormId) {
        targetMenuOptions.replaceChildren();
        if (nearbyTargets.length === 0) {
            targetMenuLabel.textContent = 'No activated NPCs nearby';
            targetMenuToggle.disabled = true;
            closeTargetMenu();
            return;
        }

        let selectedLabel = '';
        nearbyTargets.forEach(function (target) {
            const option = document.createElement('button');
            option.type = 'button';
            option.className = 'target-option-tile';
            option.setAttribute('role', 'option');
            option.dataset.formId = String(target.form_id || '');
            const distance = Number(target.distance);
            const distanceLabel = Number.isFinite(distance) ? ` (${distance.toFixed(1)}m)` : '';
            const name = target.name || 'Unknown NPC';
            option.appendChild(document.createTextNode(name));
            if (distanceLabel) {
                option.appendChild(createElement('span', 'target-option-distance', distanceLabel.trim()));
            }
            const selected = option.dataset.formId === String(selectedFormId || '');
            option.classList.toggle('is-active', selected);
            option.setAttribute('aria-selected', selected ? 'true' : 'false');
            if (selected) {
                selectedLabel = `${name}${distanceLabel}`;
            }
            option.addEventListener('click', function () {
                chooseNearbyTarget(target);
            });
            targetMenuOptions.appendChild(option);
        });
        targetMenuLabel.textContent = selectedLabel || 'Choose nearby NPC';
        targetMenuToggle.disabled = false;
    }

    function renderTarget() {
        if (!currentTarget.has_target) {
            targetName.textContent = 'No activated NPCs nearby.';
            targetRefid.textContent = 'RefID unavailable';
            targetStateBadge.textContent = 'No target';
            targetStateBadge.classList.remove('enabled');
            enrollmentButton.textContent = 'Add to Background Life';
            enrollmentButton.classList.remove('danger');
            autoActionsToggle.checked = false;
            sendLettersToggle.checked = false;
            hourlyTrackingToggle.checked = false;
            setTargetControlsBusy(false);
            return;
        }

        targetName.textContent = currentTarget.name || 'Unknown NPC';
        targetRefid.textContent = currentTarget.refid ? `RefID ${currentTarget.refid}` : 'RefID unavailable';

        const enabled = !!currentTarget.background_life_enabled;
        targetStateBadge.textContent = enabled ? 'Enabled' : 'Disabled';
        targetStateBadge.classList.toggle('enabled', enabled);
        enrollmentButton.textContent = enabled ? 'Disable Background Life' : 'Add to Background Life';
        enrollmentButton.classList.toggle('danger', enabled);
        autoActionsToggle.checked = !!currentTarget.auto_actions;
        sendLettersToggle.checked = !!currentTarget.send_letters;
        hourlyTrackingToggle.checked = !!currentTarget.hourly_tracking;
        setTargetControlsBusy(false);
    }

    function targetParameters() {
        return new URLSearchParams({
            npc_name: currentTarget.name || '',
            refid: currentTarget.refid || ''
        });
    }

    async function parseJsonResponse(response) {
        let payload;
        try {
            payload = await response.json();
        } catch (error) {
            throw new Error(`Server returned invalid JSON (HTTP ${response.status})`);
        }

        if (!response.ok || !payload || !payload.success) {
            throw new Error(
                payload && (payload.error || payload.message)
                    ? payload.error || payload.message
                    : `HTTP ${response.status}`
            );
        }
        return payload;
    }

    function setRosterStatus(message, isError) {
        rosterStatus.textContent = message || '';
        rosterStatus.classList.toggle('error', !!isError);
    }

    function clearRosterRemoveConfirmation() {
        rosterRemoveConfirmKey = '';
        if (rosterRemoveConfirmTimer) {
            window.clearTimeout(rosterRemoveConfirmTimer);
            rosterRemoveConfirmTimer = null;
        }
    }

    function renderRoster(entries) {
        rosterList.replaceChildren();
        rosterCount.textContent = String((entries || []).length);
        clearRosterRemoveConfirmation();

        if (!entries || entries.length === 0) {
            rosterList.appendChild(createElement(
                'div',
                'roster-empty',
                'No NPCs are currently enrolled in Background Life.'
            ));
            return;
        }

        entries.forEach(function (entry) {
            const row = createElement('article', 'roster-entry');
            const name = entry.name || 'Unknown NPC';
            const key = entry.refid || name;
            row.appendChild(createElement('div', 'roster-name', name));

            const removeButton = createElement('button', 'roster-remove', 'Remove');
            removeButton.type = 'button';
            removeButton.title = `Remove ${name} from Background Life`;
            removeButton.addEventListener('click', function () {
                if (rosterRemoveConfirmKey !== key) {
                    clearRosterRemoveConfirmation();
                    rosterRemoveConfirmKey = key;
                    removeButton.textContent = 'Confirm';
                    removeButton.classList.add('confirm');
                    rosterRemoveConfirmTimer = window.setTimeout(function () {
                        rosterRemoveConfirmKey = '';
                        removeButton.textContent = 'Remove';
                        removeButton.classList.remove('confirm');
                    }, 3500);
                    return;
                }
                removeRosterNpc(entry, removeButton);
            });
            row.appendChild(removeButton);

            row.appendChild(createElement(
                'div',
                'roster-activity',
                entry.activity || 'No Background Life activity recorded yet.'
            ));
            if (entry.tamrielic_time) {
                row.appendChild(createElement('div', 'roster-time', entry.tamrielic_time));
            }
            rosterList.appendChild(row);
        });
    }

    async function refreshRoster() {
        const generation = ++rosterRequestGeneration;
        setRosterStatus('Loading NPCs...', false);

        try {
            const response = await fetch(
                `${serverBaseUrl}/ui/api/background_life_npc.php?operation=list`,
                { cache: 'no-store' }
            );
            const payload = await parseJsonResponse(response);
            if (generation !== rosterRequestGeneration) {
                return;
            }
            const roster = Array.isArray(payload.roster) ? payload.roster : [];
            renderRoster(roster);
            setRosterStatus(
                roster.length === 1 ? '1 enrolled NPC' : `${roster.length} enrolled NPCs`,
                false
            );
        } catch (error) {
            if (generation !== rosterRequestGeneration) {
                return;
            }
            renderRoster([]);
            setRosterStatus(`Could not load NPCs: ${error.message || error}`, true);
        }
    }

    async function removeRosterNpc(entry, button) {
        clearRosterRemoveConfirmation();
        button.disabled = true;
        button.textContent = 'Removing...';
        setRosterStatus(`Removing ${entry.name || 'NPC'}...`, false);

        try {
            const body = new URLSearchParams({
                operation: 'disable',
                npc_name: entry.name || '',
                refid: entry.refid || ''
            });
            const response = await fetch(`${serverBaseUrl}/ui/api/background_life_npc.php`, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8'
                },
                body: body.toString()
            });
            const payload = await parseJsonResponse(response);
            const formId = parseInt(String(entry.refid || '').replace(/^0x/i, ''), 16);
            if (Number.isFinite(formId) && formId > 0) {
                sendCommand(`roster_remove|${formId}`);
            }
            if (
                currentTarget.has_target &&
                String(currentTarget.refid || '').toUpperCase() === String(entry.refid || '').toUpperCase()
            ) {
                currentTarget.background_life_enabled = false;
                currentTarget.game_enrolled = false;
                renderTarget();
            }
            setRosterStatus(payload.message || 'NPC removed from Background Life.', false);
            await refreshRoster();
        } catch (error) {
            button.disabled = false;
            button.textContent = 'Remove';
            setRosterStatus(`Could not remove NPC: ${error.message || error}`, true);
        }
    }

    async function refreshTargetStatus(options) {
        const generation = ++targetRequestGeneration;
        const quiet = !!(options && options.quiet);

        if (!currentTarget.has_target) {
            renderTarget();
            return;
        }

        if (!quiet) {
            setTargetStatus('Loading NPC settings...', '');
        }

        try {
            const response = await fetch(
                `${serverBaseUrl}/ui/api/background_life_npc.php?${targetParameters().toString()}`,
                { cache: 'no-store' }
            );
            const payload = await parseJsonResponse(response);
            if (generation !== targetRequestGeneration) {
                return;
            }

            currentTarget = Object.assign({}, currentTarget, payload.data || {});
            renderTarget();
            if (!currentTarget.exists) {
                setTargetStatus('This NPC has not been discovered by CHIM yet.', 'error');
            } else if (!quiet) {
                setTargetStatus('NPC settings loaded.', 'success');
            }
        } catch (error) {
            if (generation !== targetRequestGeneration) {
                return;
            }
            currentTarget.exists = false;
            currentTarget.background_life_enabled = !!currentTarget.game_enrolled;
            renderTarget();
            setTargetStatus(`Could not load NPC settings: ${error.message || error}`, 'error');
        }
    }

    function scheduleTargetRefreshes() {
        [350, 1000, 2200].forEach(function (delay) {
            window.setTimeout(function () {
                refreshTargetStatus({ quiet: true });
                refreshRoster();
            }, delay);
        });
    }

    window.updateBackgroundLifeTarget = function (payload) {
        try {
            const nextTarget = typeof payload === 'string' ? JSON.parse(payload) : payload;
            targetRequestGeneration += 1;
            nearbyTargets = Array.isArray(nextTarget && nextTarget.targets)
                ? nextTarget.targets
                : [];
            renderTargetOptions(nextTarget && nextTarget.selected_form_id);
            currentTarget = Object.assign({
                has_target: false,
                name: '',
                refid: '',
                game_enrolled: false,
                exists: false,
                background_life_enabled: false,
                auto_actions: false,
                send_letters: false,
                hourly_tracking: false
            }, nextTarget || {});
            renderTarget();
            setTargetStatus('', '');
            refreshTargetStatus();
        } catch (error) {
            setTargetStatus('Invalid target data received from CHIM.', 'error');
        }
    };

    async function updateTargetSetting(setting, value) {
        setTargetControlsBusy(true);
        setTargetStatus('Saving NPC setting...', '');

        try {
            const body = targetParameters();
            body.set('operation', 'toggle');
            body.set('setting', setting);
            body.set('value', value ? '1' : '0');

            const response = await fetch(`${serverBaseUrl}/ui/api/background_life_npc.php`, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8'
                },
                body: body.toString()
            });
            const payload = await parseJsonResponse(response);
            currentTarget = Object.assign({}, currentTarget, payload.data || {});
            renderTarget();
            setTargetStatus(payload.message || 'NPC setting saved.', 'success');
        } catch (error) {
            renderTarget();
            setTargetStatus(`Could not save NPC setting: ${error.message || error}`, 'error');
        }
    }

    async function queueImmediateRequest(requestType) {
        setTargetControlsBusy(true);
        const pendingMessages = {
            letter: 'Queueing letter request...',
            action: 'Queueing action request...'
        };
        setTargetStatus(pendingMessages[requestType] || 'Queueing request...', '');

        try {
            const body = targetParameters();
            body.set('request_type', requestType);
            const response = await fetch(`${serverBaseUrl}/ui/api/background_life_request.php`, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8'
                },
                body: body.toString()
            });
            const payload = await parseJsonResponse(response);
            setTargetStatus(payload.message || 'Request queued.', 'success');
            return payload;
        } catch (error) {
            setTargetStatus(`Could not queue request: ${error.message || error}`, 'error');
            return null;
        } finally {
            setTargetControlsBusy(false);
        }
    }

    function getRumorModalOverlay() {
        return document.getElementById('rumor-modal-overlay');
    }

    function getRumorForm() {
        return document.getElementById('rumor-create-form');
    }

    function isRumorModalOpen() {
        const overlay = getRumorModalOverlay();
        return !!overlay && !overlay.classList.contains('hidden');
    }

    function setRumorFormStatus(message, type) {
        const formStatus = document.getElementById('rumor-form-status');
        if (!formStatus) {
            return;
        }

        formStatus.textContent = message || '';
        formStatus.classList.remove('error', 'success');
        if (type === 'error' || type === 'success') {
            formStatus.classList.add(type);
        }
    }

    function setRumorSubmitState(busy) {
        rumorCreateInFlight = !!busy;
        const submitButton = document.getElementById('rumor-submit-button');
        if (submitButton) {
            submitButton.disabled = !!busy;
            submitButton.textContent = busy ? 'Creating Rumor...' : 'Create Rumor';
        }
    }

    window.setBackgroundLifeServerUrl = function (value) {
        serverBaseUrl = normalizeServerBaseUrl(value);
    };

    window.onBackgroundLifeShown = function () {
        closeTargetMenu();
        closeTileDropdowns();
        requestHistory();
        refreshRoster();
    };

    window.openRumorModal = function () {
        const overlay = getRumorModalOverlay();
        const form = getRumorForm();
        if (!overlay || !form) {
            return;
        }

        form.reset();
        rumorHoldDropdown.select('', 'Select hold', false);
        setRumorFormStatus('', '');
        setRumorSubmitState(false);
        overlay.classList.remove('hidden');
        overlay.setAttribute('aria-hidden', 'false');

        window.setTimeout(function () {
            rumorHoldSelectToggle.focus();
        }, 0);
    };

    window.closeRumorModal = function () {
        const overlay = getRumorModalOverlay();
        const form = getRumorForm();
        if (!overlay) {
            return;
        }

        overlay.classList.add('hidden');
        overlay.setAttribute('aria-hidden', 'true');
        setRumorFormStatus('', '');
        setRumorSubmitState(false);
        if (form) {
            form.reset();
            rumorHoldDropdown.select('', 'Select hold', false);
        }
    };

    window.submitRumorForm = async function (event) {
        if (event) {
            event.preventDefault();
        }
        if (rumorCreateInFlight) {
            return;
        }

        const form = getRumorForm();
        if (!form) {
            return;
        }

        const formData = new FormData(form);
        const hold = String(formData.get('rumor_hold') || '').trim();
        const type = String(formData.get('rumor_type') || '').trim();
        const content = String(formData.get('rumor_content') || '').trim();
        const rumorLengthDays = String(formData.get('rumor_length_days') || '').trim();

        if (!hold) {
            setRumorFormStatus('Select a hold for this rumor.', 'error');
            return;
        }
        if (!content) {
            setRumorFormStatus('Rumor content is required.', 'error');
            return;
        }
        if (rumorLengthDays !== '' && (!/^\d+$/.test(rumorLengthDays) || Number(rumorLengthDays) < 1)) {
            setRumorFormStatus('Rumor length must be a whole number of at least one day.', 'error');
            return;
        }

        setRumorSubmitState(true);
        setRumorFormStatus('Saving rumor...', '');

        try {
            const response = await fetch(`${serverBaseUrl}/ui/cmd/action_create_rumor.php`, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8'
                },
                body: new URLSearchParams({
                    rumor_hold: hold,
                    rumor_type: type,
                    rumor_content: content,
                    rumor_length_days: rumorLengthDays || '7'
                }).toString()
            });

            const result = await response.json();
            if (!response.ok || !result || !result.ok) {
                throw new Error(result && result.message ? result.message : `HTTP ${response.status}`);
            }

            setRumorFormStatus(result.message || 'Rumor created successfully.', 'success');
            window.setTimeout(window.closeRumorModal, 300);
        } catch (error) {
            console.error('[CHIM Background Life] Create Rumor failed:', error);
            setRumorFormStatus(`Create Rumor failed: ${error.message || error}`, 'error');
        } finally {
            setRumorSubmitState(false);
        }
    };

    function renderNpcOptions(npcs) {
        const selected = npcFilter.value;
        const entries = [{ value: '', label: 'All NPCs' }].concat((npcs || []).map(function (npc) {
            return {
                value: npc.name,
                label: npc.name + ' (' + npc.count + ')'
            };
        }));
        npcFilterDropdown.setOptions(entries, selected);
    }

    function renderEmpty(message) {
        activityList.replaceChildren(createElement('div', 'empty-state', message));
    }

    function renderEntries(entries) {
        activityList.replaceChildren();
        if (!entries || entries.length === 0) {
            renderEmpty('No Background Life activity matches these filters.');
            return;
        }

        entries.forEach(function (entry) {
            const item = createElement('article', 'activity-entry');
            item.tabIndex = 0;
            item.setAttribute('aria-expanded', 'false');

            item.appendChild(createElement('div', 'activity-time', entry.tamrielic_time || 'Unknown time'));
            item.appendChild(createElement('div', 'activity-npc', entry.npc || 'Unknown NPC'));

            const summary = createElement('div', 'activity-summary');
            summary.appendChild(createElement('span', 'category-badge', entry.category || 'activity'));
            summary.appendChild(createElement('span', 'activity-text', entry.activity || 'No details recorded'));
            item.appendChild(summary);

            const details = createElement(
                'div',
                'activity-details',
                [entry.server_time, entry.rowid ? 'History ID ' + entry.rowid : ''].filter(Boolean).join(' | ')
            );
            item.appendChild(details);

            function toggleExpanded() {
                const expanded = !item.classList.contains('expanded');
                item.classList.toggle('expanded', expanded);
                item.setAttribute('aria-expanded', expanded ? 'true' : 'false');
            }

            item.addEventListener('click', toggleExpanded);
            item.addEventListener('keydown', function (event) {
                if (event.key === 'Enter' || event.key === ' ') {
                    event.preventDefault();
                    toggleExpanded();
                }
            });

            activityList.appendChild(item);
        });
    }

    function requestHistory() {
        setLoading(true);
        setStatus('Loading activity...', false);

        const params = new URLSearchParams({
            page: String(currentPage),
            limit: limitSelect.value
        });
        if (npcFilter.value) {
            params.set('npc', npcFilter.value);
        }
        if (searchInput.value.trim()) {
            params.set('search', searchInput.value.trim());
        }

        sendCommand('fetch|' + params.toString());
    }

    window.updateBackgroundLifeHistory = function (payload) {
        try {
            const data = typeof payload === 'string' ? JSON.parse(payload) : payload;
            if (!data || !data.success) {
                throw new Error((data && data.error) || 'Unable to load activity');
            }

            renderNpcOptions(data.npcs);
            renderEntries(data.entries);

            const pagination = data.pagination || {};
            currentPage = pagination.current_page || 1;
            totalPages = pagination.total_pages || 1;
            previousButton.disabled = currentPage <= 1;
            nextButton.disabled = currentPage >= totalPages;
            pageLabel.textContent = 'Page ' + currentPage + ' of ' + totalPages;

            const total = pagination.total_records === undefined ? data.entries.length : pagination.total_records;
            setStatus(total + ' activit' + (total === 1 ? 'y' : 'ies'), false);
        } catch (error) {
            renderEmpty('Background Life history could not be loaded.');
            setStatus(error.message || 'Unable to load activity', true);
        } finally {
            setLoading(false);
        }
    };

    window.showBackgroundLifeError = function (message) {
        renderEmpty('Background Life history could not be loaded.');
        setStatus(message || 'Unable to load activity', true);
        setLoading(false);
    };

    window.closePanel = function () {
        sendCommand('close');
    };

    npcFilter.addEventListener('change', function () {
        currentPage = 1;
        requestHistory();
    });
    searchInput.addEventListener('input', function () {
        window.clearTimeout(searchTimer);
        searchTimer = window.setTimeout(function () {
            currentPage = 1;
            requestHistory();
        }, 300);
    });
    limitSelect.addEventListener('change', function () {
        currentPage = 1;
        requestHistory();
    });
    refreshButton.addEventListener('click', function () {
        requestHistory();
        refreshRoster();
        sendCommand('target_refresh');
    });
    targetMenuToggle.addEventListener('click', function () {
        if (targetMenuToggle.disabled) {
            return;
        }
        const opening = targetMenuOptions.classList.contains('hidden');
        closeTileDropdowns();
        targetMenuOptions.classList.toggle('hidden', !opening);
        targetMenuToggle.setAttribute('aria-expanded', opening ? 'true' : 'false');
    });
    enrollmentButton.addEventListener('click', function () {
        if (!currentTarget.has_target) {
            return;
        }

        const enable = !currentTarget.background_life_enabled;
        currentTarget.game_enrolled = enable;
        currentTarget.background_life_enabled = enable;
        renderTarget();
        setTargetControlsBusy(true);
        setTargetStatus(enable ? 'Adding NPC to Background Life...' : 'Disabling Background Life...', '');
        sendCommand(`enrollment|${enable ? 'enable' : 'disable'}`);
        scheduleTargetRefreshes();
    });
    autoActionsToggle.addEventListener('change', function () {
        updateTargetSetting('auto_actions', autoActionsToggle.checked);
    });
    sendLettersToggle.addEventListener('change', function () {
        updateTargetSetting('send_letters', sendLettersToggle.checked);
    });
    hourlyTrackingToggle.addEventListener('change', function () {
        updateTargetSetting('hourly_tracking', hourlyTrackingToggle.checked);
    });
    triggerActionButton.addEventListener('click', function () {
        queueImmediateRequest('action');
    });
    requestLetterButton.addEventListener('click', function () {
        queueImmediateRequest('letter');
    });
    document.querySelectorAll('.context-button').forEach(function (button) {
        button.addEventListener('click', function () {
            const mode = button.getAttribute('data-mode');
            if (mode) {
                sendCommand(`mode|${mode}`);
            }
        });
    });
    previousButton.addEventListener('click', function () {
        if (currentPage > 1) {
            currentPage -= 1;
            requestHistory();
        }
    });
    nextButton.addEventListener('click', function () {
        if (currentPage < totalPages) {
            currentPage += 1;
            requestHistory();
        }
    });
    document.addEventListener('keydown', function (event) {
        if (event.key === 'Escape') {
            event.preventDefault();
            if (isRumorModalOpen()) {
                window.closeRumorModal();
                return;
            }
            if (!targetMenuOptions.classList.contains('hidden')) {
                closeTargetMenu();
                return;
            }
            const openDropdown = tileDropdowns.find(function (dropdown) {
                return !dropdown.options.classList.contains('hidden');
            });
            if (openDropdown) {
                openDropdown.close();
                return;
            }
            window.closePanel();
        }
    });
    document.addEventListener('click', function (event) {
        if (
            !targetMenuOptions.classList.contains('hidden') &&
            !targetMenuOptions.contains(event.target) &&
            !targetMenuToggle.contains(event.target)
        ) {
            closeTargetMenu();
        }
        tileDropdowns.forEach(function (dropdown) {
            if (
                !dropdown.options.classList.contains('hidden') &&
                !dropdown.options.contains(event.target) &&
                !dropdown.toggle.contains(event.target)
            ) {
                dropdown.close();
            }
        });
    });
    document.addEventListener('DOMContentLoaded', function () {
        sendCommand('dom_ready');
    });
})();
