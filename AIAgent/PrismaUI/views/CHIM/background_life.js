(function () {
    'use strict';

    const modal = document.getElementById('background-life-modal');
    const activityList = document.getElementById('activity-list');
    const status = document.getElementById('panel-status');
    const npcFilter = document.getElementById('npc-filter');
    const searchInput = document.getElementById('search-input');
    const limitSelect = document.getElementById('limit-select');
    const refreshButton = document.getElementById('refresh-button');
    const previousButton = document.getElementById('previous-button');
    const nextButton = document.getElementById('next-button');
    const pageLabel = document.getElementById('page-label');
    const targetName = document.getElementById('target-name');
    const targetRefid = document.getElementById('target-refid');
    const targetSelect = document.getElementById('target-select');
    const targetStateBadge = document.getElementById('target-state-badge');
    const targetStatus = document.getElementById('target-status');
    const enrollmentButton = document.getElementById('enrollment-button');
    const autoActionsToggle = document.getElementById('auto-actions-toggle');
    const sendLettersToggle = document.getElementById('send-letters-toggle');
    const hourlyTrackingToggle = document.getElementById('hourly-tracking-toggle');
    const triggerActionButton = document.getElementById('trigger-action-button');
    const requestLetterButton = document.getElementById('request-letter-button');

    let currentPage = 1;
    let totalPages = 1;
    let searchTimer = null;
    let rumorCreateInFlight = false;
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
        targetSelect.disabled = busy || nearbyTargets.length === 0;
        enrollmentButton.disabled = busy || !currentTarget.has_target;
        autoActionsToggle.disabled = busy || !available;
        sendLettersToggle.disabled = busy || !available;
        hourlyTrackingToggle.disabled = busy || !available;
        triggerActionButton.disabled = busy || !available;
        requestLetterButton.disabled = busy || !available;
    }

    function renderTargetOptions(selectedFormId) {
        targetSelect.replaceChildren();
        if (nearbyTargets.length === 0) {
            const option = document.createElement('option');
            option.value = '';
            option.textContent = 'No activated NPCs nearby';
            targetSelect.appendChild(option);
            targetSelect.disabled = true;
            return;
        }

        nearbyTargets.forEach(function (target) {
            const option = document.createElement('option');
            option.value = String(target.form_id || '');
            const distance = Number(target.distance);
            const distanceLabel = Number.isFinite(distance) ? ` (${distance.toFixed(1)}m)` : '';
            option.textContent = `${target.name || 'Unknown NPC'}${distanceLabel}`;
            option.selected = option.value === String(selectedFormId || '');
            targetSelect.appendChild(option);
        });
        targetSelect.disabled = false;
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
        setTargetStatus(requestType === 'letter' ? 'Queueing letter request...' : 'Queueing action request...', '');

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
        } catch (error) {
            setTargetStatus(`Could not queue request: ${error.message || error}`, 'error');
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
        refreshTargetStatus({ quiet: true });
    };

    window.openRumorModal = function () {
        const overlay = getRumorModalOverlay();
        const form = getRumorForm();
        if (!overlay || !form) {
            return;
        }

        form.reset();
        setRumorFormStatus('', '');
        setRumorSubmitState(false);
        overlay.classList.remove('hidden');
        overlay.setAttribute('aria-hidden', 'false');

        const holdSelect = document.getElementById('rumor-hold-select');
        if (holdSelect) {
            window.setTimeout(function () {
                holdSelect.focus();
            }, 0);
        }
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
        npcFilter.replaceChildren();

        const allOption = document.createElement('option');
        allOption.value = '';
        allOption.textContent = 'All NPCs';
        npcFilter.appendChild(allOption);

        (npcs || []).forEach(function (npc) {
            const option = document.createElement('option');
            option.value = npc.name;
            option.textContent = npc.name + ' (' + npc.count + ')';
            npcFilter.appendChild(option);
        });

        npcFilter.value = selected;
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
        sendCommand('target_refresh');
    });
    targetSelect.addEventListener('change', function () {
        const selected = nearbyTargets.find(function (target) {
            return String(target.form_id || '') === targetSelect.value;
        });
        if (!selected) {
            return;
        }

        targetRequestGeneration += 1;
        currentTarget = Object.assign({}, currentTarget, {
            has_target: true,
            name: selected.name || '',
            refid: selected.refid || '',
            game_enrolled: !!selected.game_enrolled,
            exists: false,
            background_life_enabled: !!selected.game_enrolled,
            auto_actions: false,
            send_letters: false,
            hourly_tracking: false
        });
        renderTarget();
        setTargetControlsBusy(true);
        setTargetStatus('Loading selected NPC...', '');
        sendCommand(`target_select|${targetSelect.value}`);
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
            window.closePanel();
        }
    });
    document.addEventListener('DOMContentLoaded', function () {
        sendCommand('dom_ready');
    });
})();
