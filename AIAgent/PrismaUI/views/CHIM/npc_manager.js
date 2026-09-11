(function () {
    'use strict';

    const byId = (id) => document.getElementById(id);
    const form = byId('npc-form');
    const featureDefinitions = [
        ['dynamic_profile', 'Dynamic Profile'],
        ['middle_term_enabled', 'Middle Term Memory'],
        ['individual_memory_enabled', 'Individual Memory'],
        ['auto_diary_enabled', 'Auto Diary'],
        ['auto_diary_wait_enabled', 'Auto Diary Wait'],
        ['salutation_after_a_while', 'Delayed Salutations']
    ];
    const relationshipTypes = [
        'neutral', 'romantic', 'platonic', 'familial', 'professional', 'rival', 'enemy',
        'nemesis', 'estranged', 'transactional', 'protective', 'indebted', 'fanatical',
        'mentor', 'student', 'servant', 'client', 'patron', 'crush', 'ex', 'betrayed',
        'suspicious', 'admirer', 'jealous', 'fearful', 'obsessed', 'awed', 'contempt',
        'pitying', 'grateful', 'curious', 'dismissive'
    ];

    let serverBaseUrl = 'http://127.0.0.1:8081/HerikaServer';
    let nearbyTargets = [];
    let scope = 'nearby';
    let page = 1;
    let pages = 1;
    let profiles = [];
    let currentDetail = null;
    let loadingGeneration = 0;
    let searchTimer = null;
    let historyRecipientSearchTimer = null;
    let historySearchGeneration = 0;
    let historyEventType = '';
    const historyRecipients = new Map();
    const VOICE_FILTER_NONE_ID = 'none';
    let voiceFilterPresets = [];
    let voiceFilterPreviewAudio = null;
    let voiceFilterPreviewCache = null;
    let voiceFilterPreviewGeneration = 0;
    const embeddedInSettings = !!byId('npcs-page');

    function sendCommand(command) {
        if (embeddedInSettings && window.chimConfigManagerCommand) {
            window.chimConfigManagerCommand(`npc|${command}`);
            return;
        }
        if (window.chimNpcManagerCommand) window.chimNpcManagerCommand(command);
    }

    document.querySelectorAll('[data-settings-page]').forEach((button) => {
        button.addEventListener('click', () => {
            if (button.dataset.settingsPage !== 'npcs') {
                sendCommand(`tab_${button.dataset.settingsPage}`);
            }
        });
    });

    function normalizeBaseUrl(value) {
        let base = String(value || '').trim().replace(/\/+$/, '');
        if (!/\/HerikaServer$/i.test(base)) base += '/HerikaServer';
        return base;
    }

    function resolveAssetUrl(value) {
        const url = String(value || '');
        if (!url || /^https?:\/\//i.test(url)) return url;
        try {
            const server = new URL(serverBaseUrl);
            return url.startsWith('/') ? server.origin + url : `${serverBaseUrl}/${url}`;
        } catch (_error) {
            return url;
        }
    }

    async function parseResponse(response) {
        let payload;
        try { payload = await response.json(); } catch (_error) {
            throw new Error(`Server returned invalid JSON (HTTP ${response.status})`);
        }
        if (!response.ok || !payload || !payload.success) {
            throw new Error((payload && payload.error) || `HTTP ${response.status}`);
        }
        return payload.data;
    }

    function setStatus(message, error) {
        const line = byId('status-text');
        line.textContent = message || '';
        line.parentElement.classList.toggle('error', !!error);
    }

    function nearbyLookup() {
        const map = new Map();
        nearbyTargets.forEach((target) => {
            const refid = String(target.refid || '').replace(/^0x/i, '').toUpperCase().padStart(8, '0');
            if (refid) map.set(refid, target);
            if (target.name) map.set(`name:${String(target.name).toLowerCase()}`, target);
        });
        return map;
    }

    function applyProfiles(nextProfiles) {
        profiles = Array.isArray(nextProfiles) ? nextProfiles : [];
        const filter = byId('profile-filter');
        const previous = filter.value;
        filter.replaceChildren(new Option('All profiles', ''));
        profiles.forEach((profile) => filter.appendChild(new Option(profile.label, String(profile.id))));
        if (Array.from(filter.options).some((option) => option.value === previous)) filter.value = previous;
    }

    async function loadNpcs() {
        const generation = ++loadingGeneration;
        setStatus(scope === 'nearby' ? 'Loading nearby NPCs...' : 'Loading NPC profiles...', false);
        const params = new URLSearchParams({ operation: 'list', page: String(page), limit: '48' });
        const search = byId('search-input').value.trim();
        const profileId = byId('profile-filter').value;
        if (search) params.set('search', search);
        if (profileId) params.set('profile_id', profileId);
        if (scope === 'nearby') {
            params.set('refids', nearbyTargets.map((target) => target.refid || '').filter(Boolean).join(','));
            params.set('names', nearbyTargets.map((target) => target.name || '').filter(Boolean).join('|'));
            if (nearbyTargets.length === 0) {
                renderCards([]);
                setStatus('No activated NPCs are nearby.', false);
                return;
            }
        }

        try {
            const data = await parseResponse(await fetch(
                `${serverBaseUrl}/ui/api/chim_npc_manager.php?${params.toString()}`,
                { cache: 'no-store' }
            ));
            if (generation !== loadingGeneration) return;
            applyProfiles(data.profiles);
            pages = Number(data.pagination && data.pagination.pages) || 1;
            page = Number(data.pagination && data.pagination.page) || 1;
            renderCards(data.npcs || []);
            byId('page-label').textContent = `Page ${page} of ${pages}`;
            byId('previous-page').disabled = page <= 1;
            byId('next-page').disabled = page >= pages;
            const total = Number(data.pagination && data.pagination.total) || 0;
            byId('result-count').textContent = `${total} NPC${total === 1 ? '' : 's'}`;
            setStatus(scope === 'nearby' ? 'Activated NPCs currently nearby' : 'All discovered NPC profiles', false);
        } catch (error) {
            if (generation !== loadingGeneration) return;
            renderCards([]);
            setStatus(`Could not load NPCs: ${error.message || error}`, true);
        }
    }

    function renderCards(npcs) {
        const grid = byId('npc-grid');
        grid.replaceChildren();
        const lookup = nearbyLookup();
        const ordered = Array.from(npcs);
        ordered.sort((left, right) => {
            const leftRef = String(left.refid || '').replace(/^0x/i, '').toUpperCase().padStart(8, '0');
            const rightRef = String(right.refid || '').replace(/^0x/i, '').toUpperCase().padStart(8, '0');
            const leftTarget = lookup.get(leftRef) || lookup.get(`name:${String(left.name).toLowerCase()}`);
            const rightTarget = lookup.get(rightRef) || lookup.get(`name:${String(right.name).toLowerCase()}`);
            return Number(leftTarget && leftTarget.distance || 99999) - Number(rightTarget && rightTarget.distance || 99999);
        });
        if (ordered.length === 0) {
            const empty = document.createElement('div');
            empty.className = 'empty-state';
            empty.textContent = scope === 'nearby'
                ? 'No nearby activated NPC has a discovered CHIM profile yet.'
                : 'No NPC profiles match the current filters.';
            grid.appendChild(empty);
            return;
        }

        ordered.forEach((npc) => {
            const refid = String(npc.refid || '').replace(/^0x/i, '').toUpperCase().padStart(8, '0');
            const target = lookup.get(refid) || lookup.get(`name:${String(npc.name).toLowerCase()}`);
            const card = document.createElement('button');
            card.type = 'button';
            card.className = 'npc-card';
            card.dataset.id = String(npc.id);
            const portrait = document.createElement('img');
            portrait.src = resolveAssetUrl(npc.portrait_url);
            portrait.alt = '';
            const copy = document.createElement('div');
            copy.className = 'npc-card-copy';
            const name = document.createElement('div');
            name.className = 'npc-card-name';
            name.textContent = npc.name || 'Unknown NPC';
            const meta = document.createElement('div');
            meta.className = 'npc-card-meta';
            meta.textContent = [npc.race, npc.gender].filter(Boolean).join(' · ') || 'Unknown race';
            const profile = document.createElement('div');
            profile.className = 'npc-card-profile';
            profile.textContent = npc.profile_label || 'No Profile';
            const flags = document.createElement('div');
            flags.className = 'npc-card-flags';
            if (target) flags.appendChild(pill(`${Number(target.distance || 0).toFixed(1)}m`, 'nearby'));
            if (npc.favorite) flags.appendChild(pill('Favorite', 'good'));
            if (npc.locked) flags.appendChild(pill('Locked'));
            copy.append(name, meta, profile, flags);
            card.append(portrait, copy);
            card.addEventListener('click', () => openEditor(npc.id));
            grid.appendChild(card);
        });
    }

    function pill(text, className) {
        const element = document.createElement('span');
        element.className = `pill ${className || ''}`.trim();
        element.textContent = text;
        return element;
    }

    async function openEditor(id) {
        byId('editor-backdrop').classList.remove('hidden');
        byId('editor-title').textContent = 'Loading NPC...';
        resetVoiceFilterPreview();
        byId('save-status').textContent = '';
        try {
            currentDetail = await parseResponse(await fetch(
                `${serverBaseUrl}/ui/api/chim_npc_manager.php?operation=detail&id=${encodeURIComponent(id)}`,
                { cache: 'no-store' }
            ));
            populateEditor(currentDetail);
        } catch (error) {
            byId('save-status').textContent = `Could not load NPC: ${error.message || error}`;
            byId('save-status').classList.add('error');
        }
    }

    function populateEditor(detail) {
        const fields = detail.fields || {};
        byId('npc-id').value = String(detail.card.id);
        byId('editor-title').textContent = detail.card.name || 'NPC Profile';
        byId('editor-subtitle').textContent = [detail.card.race, detail.card.profile_label].filter(Boolean).join(' · ');
        byId('editor-portrait').src = resolveAssetUrl(detail.card.portrait_url);
        Object.entries(fields).forEach(([name, value]) => {
            const control = form.elements.namedItem(name);
            if (!control) return;
            if (control.type === 'checkbox') control.checked = !!value;
            else control.value = value === null ? '' : String(value);
        });

        const profileSelect = form.elements.namedItem('profile_id');
        profileSelect.replaceChildren();
        (detail.profiles || []).forEach((profile) => profileSelect.appendChild(new Option(profile.label, String(profile.id))));
        profileSelect.value = String(fields.profile_id || '');
        renderVoiceFilterPresets(detail);
        renderFeatureToggles(detail.toggles || {});
        renderRelationships(detail.relationships || {});
        byId('relationships-locked').checked = !!detail.relationships_locked;
        byId('metadata-output').textContent = JSON.stringify(detail.metadata || {}, null, 2);
        renderTeleportAction(detail.metadata && detail.metadata.npc_manager_return_location);
        byId('bgl-inception-idea').value = '';
        byId('action-status').textContent = '';
        byId('action-status').classList.remove('error');
        byId('bgl-action-status').textContent = '';
        byId('bgl-action-status').classList.remove('error');
        resetNpcHistory(detail.card);
        switchEditorTab('general');
        byId('save-status').textContent = '';
        byId('save-status').classList.remove('error');
    }

    function voiceFilterControl(name) {
        return form.elements.namedItem(name);
    }

    function setVoiceFilterStatus(message, error) {
        const status = byId('voice-filter-status');
        status.textContent = message || '';
        status.classList.toggle('error', !!error);
    }

    function stopVoiceFilterAudio() {
        const audio = voiceFilterPreviewAudio;
        voiceFilterPreviewAudio = null;
        if (!audio) return;
        try {
            audio.pause();
            audio.currentTime = 0;
        } catch (_error) { /* the element may not be seekable yet */ }
    }

    function resetVoiceFilterPreview() {
        voiceFilterPreviewGeneration += 1;
        stopVoiceFilterAudio();
        voiceFilterPreviewCache = null;
        byId('voice-filter-preview').disabled = false;
        setVoiceFilterStatus('', false);
    }

    function voiceFilterPreviewKey() {
        const profileId = String((voiceFilterControl('profile_id') || {}).value || '');
        const voiceId = String((voiceFilterControl('voiceid') || {}).value || '').trim();
        const preset = String((voiceFilterControl('tts_filter_preset') || {}).value || '');
        return `${profileId}|${voiceId}|${preset}`;
    }

    function renderVoiceFilterDescription() {
        const select = voiceFilterControl('tts_filter_preset');
        const preset = voiceFilterPresets.find((entry) => entry.id === select.value);
        byId('voice-filter-description').textContent = (preset && preset.description) || '';
    }

    function renderVoiceFilterPresets(detail) {
        const select = voiceFilterControl('tts_filter_preset');
        const catalog = Array.isArray(detail && detail.tts_filter_presets) ? detail.tts_filter_presets : [];
        const fields = (detail && detail.fields) || {};
        const saved = fields.tts_filter_preset === null || fields.tts_filter_preset === undefined
            ? ''
            : String(fields.tts_filter_preset);
        voiceFilterPresets = catalog
            .map((preset) => ({
                id: preset && preset.id !== null && preset.id !== undefined ? String(preset.id) : '',
                label: String((preset && preset.label) || ''),
                description: String((preset && preset.description) || '')
            }))
            .filter((preset) => preset.id !== '');
        if (!voiceFilterPresets.some((preset) => preset.id === VOICE_FILTER_NONE_ID)) {
            voiceFilterPresets.unshift({
                id: VOICE_FILTER_NONE_ID,
                label: 'None',
                description: 'Play this NPC with their unfiltered voice.'
            });
        }
        if (saved && !voiceFilterPresets.some((preset) => preset.id === saved)) {
            voiceFilterPresets.push({
                id: saved,
                label: `${saved} (unavailable)`,
                description: 'The server no longer offers this saved filter. Pick another to replace it.'
            });
        }
        select.replaceChildren();
        voiceFilterPresets.forEach((preset) => select.appendChild(new Option(preset.label || preset.id, preset.id)));
        select.value = saved || VOICE_FILTER_NONE_ID;
        if (!select.value) select.value = VOICE_FILTER_NONE_ID;
        renderVoiceFilterDescription();
    }

    function invalidateVoiceFilterPreview() {
        if (voiceFilterPreviewCache && voiceFilterPreviewCache.key === voiceFilterPreviewKey()) return;
        voiceFilterPreviewGeneration += 1;
        stopVoiceFilterAudio();
        voiceFilterPreviewCache = null;
        byId('voice-filter-preview').disabled = false;
        setVoiceFilterStatus('', false);
    }

    function playVoiceFilterPreview(url) {
        stopVoiceFilterAudio();
        const audio = new Audio(url);
        voiceFilterPreviewAudio = audio;
        audio.addEventListener('ended', () => {
            if (voiceFilterPreviewAudio !== audio) return;
            voiceFilterPreviewAudio = null;
            setVoiceFilterStatus('Preview finished.', false);
        });
        audio.addEventListener('error', () => {
            if (voiceFilterPreviewAudio !== audio) return;
            voiceFilterPreviewAudio = null;
            setVoiceFilterStatus('Preview audio could not be played.', true);
        });
        setVoiceFilterStatus('Playing voice filter preview...', false);
        const started = audio.play();
        if (started && typeof started.catch === 'function') {
            started.catch((error) => {
                if (voiceFilterPreviewAudio !== audio) return;
                voiceFilterPreviewAudio = null;
                setVoiceFilterStatus(`Preview could not start: ${error.message || error}`, true);
            });
        }
    }

    async function requestVoiceFilterPreview() {
        if (!currentDetail) return;
        const button = byId('voice-filter-preview');
        const voiceId = String(voiceFilterControl('voiceid').value || '').trim();
        if (!voiceId) {
            setVoiceFilterStatus('Enter a Voice Type before previewing.', true);
            return;
        }
        const key = voiceFilterPreviewKey();
        if (voiceFilterPreviewCache && voiceFilterPreviewCache.key === key) {
            playVoiceFilterPreview(voiceFilterPreviewCache.url);
            return;
        }

        const generation = ++voiceFilterPreviewGeneration;
        stopVoiceFilterAudio();
        button.disabled = true;
        setVoiceFilterStatus('Generating voice filter preview...', false);
        try {
            const body = new FormData();
            body.append('profile_id', String(voiceFilterControl('profile_id').value || ''));
            body.append('voiceid', voiceId);
            body.append('tts_filter_preset', String(voiceFilterControl('tts_filter_preset').value || ''));
            const response = await fetch(`${serverBaseUrl}/ui/api/npc_voice_filter_preview.php`, {
                method: 'POST',
                body,
                cache: 'no-store'
            });
            let payload;
            try { payload = await response.json(); } catch (_error) {
                throw new Error(`Server returned invalid JSON (HTTP ${response.status})`);
            }
            if (!response.ok || !payload || payload.ok !== true) {
                throw new Error((payload && payload.error) || `HTTP ${response.status}`);
            }
            const url = resolveAssetUrl(payload.audio_url);
            if (!url) throw new Error('Server did not return preview audio.');
            if (generation !== voiceFilterPreviewGeneration) return;
            voiceFilterPreviewCache = { key, url };
            playVoiceFilterPreview(url);
        } catch (error) {
            if (generation !== voiceFilterPreviewGeneration) return;
            setVoiceFilterStatus(`Preview failed: ${error.message || error}`, true);
        } finally {
            if (generation === voiceFilterPreviewGeneration) button.disabled = false;
        }
    }

    function setHistoryStatus(message, error) {
        const status = byId('history-status');
        status.textContent = message || '';
        status.classList.toggle('error', !!error);
    }

    function resetNpcHistory(card) {
        historyRecipients.clear();
        historyRecipients.set(Number(card.id), String(card.name || 'NPC'));
        historySearchGeneration += 1;
        clearTimeout(historyRecipientSearchTimer);
        byId('history-recipient-search').value = '';
        byId('history-recipient-results').hidden = true;
        byId('history-recipient-results').replaceChildren();
        byId('history-event-text').value = '';
        historyEventType = '';
        byId('history-event-type').value = '';
        byId('history-filter-note').textContent = 'Using Event Log visibility filters.';
        byId('history-list').replaceChildren(historyEmpty('Open this tab to load recent events.'));
        setHistoryStatus('', false);
        renderHistoryRecipients();
    }

    function historyEmpty(message, error) {
        const empty = document.createElement('p');
        empty.className = `history-empty${error ? ' error' : ''}`;
        empty.textContent = message;
        return empty;
    }

    function renderHistoryRecipients() {
        const container = byId('history-recipients');
        const currentNpcId = Number(byId('npc-id').value || 0);
        container.replaceChildren();
        historyRecipients.forEach((name, id) => {
            const chip = document.createElement('span');
            chip.className = 'history-recipient-chip';
            const label = document.createElement('span');
            label.textContent = name;
            chip.appendChild(label);
            if (Number(id) !== currentNpcId) {
                const remove = document.createElement('button');
                remove.type = 'button';
                remove.textContent = 'x';
                remove.setAttribute('aria-label', `Remove ${name}`);
                remove.addEventListener('click', () => {
                    historyRecipients.delete(id);
                    renderHistoryRecipients();
                });
                chip.appendChild(remove);
            }
            container.appendChild(chip);
        });
    }

    function renderHistorySearchResults(npcs) {
        const container = byId('history-recipient-results');
        container.replaceChildren();
        const available = npcs.filter((npc) => !historyRecipients.has(Number(npc.id)));
        if (!available.length) {
            container.hidden = true;
            return;
        }
        available.forEach((npc) => {
            const button = document.createElement('button');
            button.type = 'button';
            button.className = 'history-search-result';
            button.textContent = npc.name || 'Unknown NPC';
            button.addEventListener('click', () => {
                historyRecipients.set(Number(npc.id), String(npc.name || 'Unknown NPC'));
                byId('history-recipient-search').value = '';
                container.hidden = true;
                renderHistoryRecipients();
            });
            container.appendChild(button);
        });
        container.hidden = false;
    }

    async function searchHistoryRecipients() {
        const search = byId('history-recipient-search').value.trim();
        if (search.length < 2) {
            byId('history-recipient-results').hidden = true;
            return;
        }
        const generation = ++historySearchGeneration;
        const query = new URLSearchParams({ operation: 'list', search, page: '1', limit: '10' });
        try {
            const data = await parseResponse(await fetch(
                `${serverBaseUrl}/ui/api/chim_npc_manager.php?${query.toString()}`,
                { cache: 'no-store' }
            ));
            if (generation === historySearchGeneration) {
                renderHistorySearchResults(Array.isArray(data.npcs) ? data.npcs : []);
            }
        } catch (_error) {
            if (generation === historySearchGeneration) byId('history-recipient-results').hidden = true;
        }
    }

    function renderNpcHistoryFilters(filters) {
        const select = byId('history-event-type');
        const types = Array.isArray(filters.event_types) ? filters.event_types : [];
        const hiddenTypes = Array.isArray(filters.hidden_event_types) ? filters.hidden_event_types : [];
        const selected = String(filters.selected_event_type || historyEventType);
        select.replaceChildren(new Option('All visible events', ''));
        types.forEach((entry) => {
            const type = String(entry.type || '');
            if (!type) return;
            select.appendChild(new Option(`${type} (${Number(entry.total || 0)})`, type));
        });
        select.value = selected;
        historyEventType = select.value;
        byId('history-filter-note').textContent = hiddenTypes.length
            ? `Hidden by Event Log: ${hiddenTypes.join(', ')}`
            : 'Using Event Log visibility filters.';
    }

    function renderNpcHistory(events) {
        const container = byId('history-list');
        container.replaceChildren();
        if (!events.length) {
            container.appendChild(historyEmpty('No events are recorded for this NPC yet.'));
            return;
        }
        const tableWrap = document.createElement('div');
        tableWrap.className = 'history-table-wrap';
        const table = document.createElement('table');
        table.className = 'history-table';
        const thead = document.createElement('thead');
        const headerRow = document.createElement('tr');
        ['Event', 'Events', 'People Present', 'Tamrielic Time', 'Time (UTC)', ''].forEach((label) => {
            const heading = document.createElement('th');
            heading.textContent = label;
            headerRow.appendChild(heading);
        });
        thead.appendChild(headerRow);
        const tbody = document.createElement('tbody');
        events.forEach((historyEvent) => {
            const row = document.createElement('tr');
            const values = [
                historyEvent.type || 'Event',
                historyEvent.data || '',
                Array.isArray(historyEvent.recipients) ? historyEvent.recipients.join(', ') : '',
                historyEvent.tamrielic_time || '',
                historyEvent.local_time || ''
            ];
            values.forEach((value, index) => {
                const cell = document.createElement('td');
                if (index === 0) cell.className = 'history-event-type';
                if (index === 1) cell.className = 'history-data';
                if (index === 2) cell.className = 'history-audience';
                cell.textContent = value;
                row.appendChild(cell);
            });
            const deleteButton = document.createElement('button');
            deleteButton.type = 'button';
            deleteButton.className = 'history-delete';
            deleteButton.textContent = 'Delete';
            deleteButton.addEventListener('click', async () => {
                const shared = Array.isArray(historyEvent.recipients) && historyEvent.recipients.length > 1;
                const warning = shared ? ' This removes it from every listed NPC history.' : '';
                if (!window.confirm(`Delete this event?${warning}`)) return;
                deleteButton.disabled = true;
                try {
                    await parseResponse(await fetch(`${serverBaseUrl}/ui/api/chim_npc_manager.php`, {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({
                            operation: 'delete_event',
                            id: Number(byId('npc-id').value),
                            rowid: Number(historyEvent.rowid)
                        })
                    }));
                    setHistoryStatus('Event deleted.', false);
                    await loadNpcHistory();
                } catch (error) {
                    setHistoryStatus(`Delete failed: ${error.message || error}`, true);
                    deleteButton.disabled = false;
                }
            });
            const actions = document.createElement('td');
            actions.appendChild(deleteButton);
            row.appendChild(actions);
            tbody.appendChild(row);
        });
        table.append(thead, tbody);
        tableWrap.appendChild(table);
        container.appendChild(tableWrap);
    }

    async function loadNpcHistory() {
        const npcId = Number(byId('npc-id').value || 0);
        if (!npcId || !currentDetail) return;
        const refreshButton = byId('history-refresh');
        refreshButton.disabled = true;
        byId('history-list').replaceChildren(historyEmpty('Loading recent events...'));
        try {
            const query = new URLSearchParams({ operation: 'history', id: String(npcId), limit: '100' });
            if (historyEventType) query.set('event_type', historyEventType);
            const data = await parseResponse(await fetch(
                `${serverBaseUrl}/ui/api/chim_npc_manager.php?${query.toString()}`,
                { cache: 'no-store' }
            ));
            if (Number(byId('npc-id').value || 0) === npcId) {
                renderNpcHistoryFilters(data.filters || {});
                renderNpcHistory(Array.isArray(data.events) ? data.events : []);
            }
        } catch (error) {
            byId('history-list').replaceChildren(historyEmpty(`History failed to load: ${error.message || error}`, true));
        } finally {
            refreshButton.disabled = false;
        }
    }

    async function injectNpcHistoryEvent() {
        const text = byId('history-event-text').value.trim();
        if (!text) {
            setHistoryStatus('Enter an event before injecting it.', true);
            byId('history-event-text').focus();
            return;
        }
        const injectButton = byId('history-inject');
        injectButton.disabled = true;
        setHistoryStatus('Injecting event...', false);
        try {
            const data = await parseResponse(await fetch(`${serverBaseUrl}/ui/api/chim_npc_manager.php`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    operation: 'inject_event',
                    id: Number(byId('npc-id').value),
                    event: text,
                    recipient_ids: Array.from(historyRecipients.keys())
                })
            }));
            byId('history-event-text').value = '';
            setHistoryStatus(data.message || 'Event injected.', false);
            await loadNpcHistory();
        } catch (error) {
            setHistoryStatus(`Injection failed: ${error.message || error}`, true);
        } finally {
            injectButton.disabled = false;
        }
    }

    // Keep the reversible teleport control aligned with the return point stored by HerikaServer.
    function renderTeleportAction(returnLocation) {
        const hasReturnLocation = !!returnLocation && typeof returnLocation === 'object';
        const locationName = hasReturnLocation ? String(returnLocation.name || '').trim() : '';
        const button = byId('teleport-action');
        button.dataset.action = hasReturnLocation ? 'return' : 'teleport';
        button.textContent = hasReturnLocation ? 'Return NPC' : 'Teleport';
        byId('teleport-action-title').textContent = hasReturnLocation ? 'Return NPC' : 'Teleport';
        byId('teleport-action-description').textContent = hasReturnLocation
            ? `Send this NPC back to ${locationName || 'their previous location'}.`
            : "Move this NPC to the player's current position and save their previous location.";
    }

    function renderFeatureToggles(toggles) {
        const container = byId('feature-toggles');
        container.replaceChildren();
        featureDefinitions.forEach(([key, label]) => {
            const state = toggles[key] || { value: false, source: 'default', profile_default: false };
            const card = document.createElement('div');
            card.className = 'toggle-card';
            card.dataset.key = key;
            card.dataset.profileDefault = state.profile_default ? '1' : '0';
            card.dataset.override = state.source === 'npc' ? (state.value ? '1' : '0') : 'inherit';
            const checkbox = document.createElement('input');
            checkbox.type = 'checkbox';
            checkbox.checked = !!state.value;
            checkbox.setAttribute('aria-label', label);
            const copy = document.createElement('span');
            copy.className = 'toggle-label';
            copy.textContent = label;
            const inherit = document.createElement('button');
            inherit.type = 'button';
            inherit.className = `inherit-button${card.dataset.override === 'inherit' ? ' inherited' : ''}`;
            inherit.textContent = card.dataset.override === 'inherit' ? 'Inherited' : 'NPC override';
            checkbox.addEventListener('change', () => {
                card.dataset.override = checkbox.checked ? '1' : '0';
                inherit.classList.remove('inherited');
                inherit.textContent = 'NPC override';
            });
            inherit.addEventListener('click', () => {
                card.dataset.override = 'inherit';
                checkbox.checked = card.dataset.profileDefault === '1';
                inherit.classList.add('inherited');
                inherit.textContent = 'Inherited';
            });
            card.append(checkbox, copy, inherit);
            container.appendChild(card);
        });
    }

    function addRelationshipRow(target, relationship) {
        const row = document.createElement('div');
        row.className = 'relationship-row';
        row.relationshipData = relationship && typeof relationship === 'object' ? { ...relationship } : {};
        const targetInput = document.createElement('input');
        targetInput.className = 'relationship-target';
        targetInput.placeholder = 'NPC or Player';
        targetInput.value = target || '';
        const affinity = document.createElement('input');
        affinity.className = 'relationship-affinity';
        affinity.type = 'number';
        affinity.min = '-100';
        affinity.max = '100';
        affinity.value = String(Number(relationship && relationship.aff || 0));
        const type = document.createElement('select');
        type.className = 'relationship-type';
        relationshipTypes.forEach((entry) => type.appendChild(new Option(entry, entry)));
        const selectedType = String(relationship && relationship.type || 'neutral').toLowerCase();
        if (!relationshipTypes.includes(selectedType)) type.appendChild(new Option(selectedType, selectedType));
        type.value = selectedType;
        const note = document.createElement('input');
        note.className = 'relationship-note';
        note.placeholder = 'Relationship note';
        note.value = String(relationship && (relationship.note || relationship.relation) || '');
        const customInfoField = document.createElement('label');
        customInfoField.className = 'relationship-custom-info-field';
        const customInfoLabel = document.createElement('span');
        customInfoLabel.textContent = 'Custom Info';
        const customInfo = document.createElement('textarea');
        customInfo.className = 'relationship-custom-info';
        customInfo.rows = 3;
        customInfo.placeholder = 'Player-only notes (not used by AI)';
        customInfo.value = String(relationship && relationship.custom_info || '');
        customInfoField.append(customInfoLabel, customInfo);
        const remove = document.createElement('button');
        remove.type = 'button';
        remove.className = 'remove-relationship';
        remove.textContent = '×';
        remove.title = 'Remove relationship';
        remove.addEventListener('click', () => row.remove());
        row.append(targetInput, affinity, type, note, remove, customInfoField);
        byId('relationship-list').appendChild(row);
    }

    function renderRelationships(relationships) {
        byId('relationship-list').replaceChildren();
        Object.entries(relationships).forEach(([target, relationship]) => addRelationshipRow(target, relationship));
    }

    function collectRelationships() {
        const relationships = {};
        byId('relationship-list').querySelectorAll('.relationship-row').forEach((row) => {
            const target = row.querySelector('.relationship-target').value.trim();
            if (!target) return;
            relationships[target] = {
                ...(row.relationshipData || {}),
                aff: Number(row.querySelector('.relationship-affinity').value || 0),
                type: row.querySelector('.relationship-type').value,
                note: row.querySelector('.relationship-note').value.trim(),
                custom_info: row.querySelector('.relationship-custom-info').value.trim()
            };
        });
        return relationships;
    }

    function collectFields() {
        const names = [
            'npc_name', 'profile_id', 'lock_profile', 'npc_favorite', 'gender', 'race', 'base',
            'refid', 'voiceid', 'oghma_knowledge_tags', 'tags', 'prompt_head', 'core',
            'npc_static_bio', 'appearance', 'personality', 'occupation', 'skills', 'speechstyle',
            'goals', 'emote_moods', 'middle_term_latest', 'tts_filter_preset'
        ];
        const fields = {};
        names.forEach((name) => {
            const control = form.elements.namedItem(name);
            fields[name] = control.type === 'checkbox' ? control.checked : control.value;
        });
        fields.profile_id = Number(fields.profile_id);
        return fields;
    }

    function collectOverrides() {
        const overrides = {};
        byId('feature-toggles').querySelectorAll('.toggle-card').forEach((card) => {
            const value = card.dataset.override;
            overrides[card.dataset.key] = value === 'inherit' ? null : value === '1';
        });
        return overrides;
    }

    async function saveNpc(event) {
        event.preventDefault();
        if (!currentDetail) return;
        const saveButton = byId('save-button');
        saveButton.disabled = true;
        byId('save-status').textContent = 'Saving NPC profile...';
        byId('save-status').classList.remove('error');
        try {
            currentDetail = await parseResponse(await fetch(`${serverBaseUrl}/ui/api/chim_npc_manager.php`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    id: Number(byId('npc-id').value),
                    fields: collectFields(),
                    overrides: collectOverrides(),
                    relationships: collectRelationships(),
                    relationships_locked: byId('relationships-locked').checked
                })
            }));
            populateEditor(currentDetail);
            byId('save-status').textContent = 'NPC profile saved.';
            await loadNpcs();
        } catch (error) {
            byId('save-status').textContent = `Save failed: ${error.message || error}`;
            byId('save-status').classList.add('error');
        } finally {
            saveButton.disabled = false;
        }
    }

    async function runNpcAction(action, button) {
        if (!currentDetail) return;
        const idea = action === 'bgl_inception' ? byId('bgl-inception-idea').value.trim() : '';
        const status = byId(action === 'bgl_inception' ? 'bgl-action-status' : 'action-status');
        if (action === 'bgl_inception' && !idea) {
            status.textContent = 'Enter a thought before setting Background Life inception.';
            status.classList.add('error');
            return;
        }

        button.disabled = true;
        status.textContent = 'Sending action...';
        status.classList.remove('error');
        try {
            const result = await parseResponse(await fetch(`${serverBaseUrl}/ui/api/chim_npc_manager.php`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    operation: 'action',
                    action,
                    id: Number(byId('npc-id').value),
                    idea
                })
            }));
            status.textContent = result.message || 'Action sent.';
            if (action === 'bgl_inception') byId('bgl-inception-idea').value = '';
            if (action === 'teleport' || action === 'return') {
                const returnLocation = result.next_action === 'return'
                    ? { name: result.return_location || '' }
                    : null;
                if (!currentDetail.metadata || typeof currentDetail.metadata !== 'object') {
                    currentDetail.metadata = {};
                }
                if (returnLocation) currentDetail.metadata.npc_manager_return_location = returnLocation;
                else delete currentDetail.metadata.npc_manager_return_location;
                byId('metadata-output').textContent = JSON.stringify(currentDetail.metadata, null, 2);
                renderTeleportAction(returnLocation);
            }
        } catch (error) {
            status.textContent = `Action failed: ${error.message || error}`;
            status.classList.add('error');
        } finally {
            button.disabled = false;
        }
    }

    function closeEditor() {
        byId('editor-backdrop').classList.add('hidden');
        resetVoiceFilterPreview();
        currentDetail = null;
        sendCommand('input_capture|off');
    }

    function switchEditorTab(tabName) {
        document.querySelectorAll('.editor-tab').forEach((tab) => tab.classList.toggle('active', tab.dataset.tab === tabName));
        document.querySelectorAll('.tab-panel').forEach((panel) => {
            const active = panel.dataset.panel === tabName;
            panel.classList.toggle('active', active);
            panel.hidden = !active;
        });
        if (tabName === 'history') loadNpcHistory();
    }

    window.setNpcManagerServerUrl = function (value) {
        serverBaseUrl = normalizeBaseUrl(value);
    };

    window.updateNpcManagerTargets = function (payloadText) {
        try {
            const payload = typeof payloadText === 'string' ? JSON.parse(payloadText) : payloadText;
            nearbyTargets = Array.isArray(payload && payload.targets) ? payload.targets : [];
            if (scope === 'nearby') {
                page = 1;
                loadNpcs();
            }
        } catch (error) {
            setStatus(`Could not read nearby NPCs: ${error.message || error}`, true);
        }
    };

    window.onNpcManagerShown = function () {
        page = 1;
        sendCommand('targets_refresh');
    };

    document.querySelectorAll('.scope-button').forEach((button) => button.addEventListener('click', () => {
        scope = button.dataset.scope;
        document.querySelectorAll('.scope-button').forEach((entry) => entry.classList.toggle('active', entry === button));
        page = 1;
        loadNpcs();
    }));
    document.querySelectorAll('.editor-tab').forEach((button) => button.addEventListener('click', () => switchEditorTab(button.dataset.tab)));
    byId('search-input').addEventListener('input', () => {
        clearTimeout(searchTimer);
        searchTimer = setTimeout(() => { page = 1; loadNpcs(); }, 250);
    });
    byId('profile-filter').addEventListener('change', () => { page = 1; loadNpcs(); });
    byId('refresh-button').addEventListener('click', () => sendCommand('targets_refresh'));
    byId('previous-page').addEventListener('click', () => { if (page > 1) { page -= 1; loadNpcs(); } });
    byId('next-page').addEventListener('click', () => { if (page < pages) { page += 1; loadNpcs(); } });
    if (!embeddedInSettings) byId('close-button').addEventListener('click', () => sendCommand('close'));
    byId('editor-close').addEventListener('click', closeEditor);
    byId('cancel-button').addEventListener('click', closeEditor);
    byId('visit-action').addEventListener('click', (event) => runNpcAction('visit', event.currentTarget));
    byId('teleport-action').addEventListener('click', (event) => {
        runNpcAction(event.currentTarget.dataset.action || 'teleport', event.currentTarget);
    });
    byId('bgl-inception-action').addEventListener('click', (event) => runNpcAction('bgl_inception', event.currentTarget));
    byId('history-refresh').addEventListener('click', loadNpcHistory);
    byId('history-event-type').addEventListener('change', (event) => {
        historyEventType = event.currentTarget.value;
        loadNpcHistory();
    });
    byId('history-inject').addEventListener('click', injectNpcHistoryEvent);
    byId('history-recipient-search').addEventListener('input', () => {
        clearTimeout(historyRecipientSearchTimer);
        historyRecipientSearchTimer = setTimeout(searchHistoryRecipients, 250);
    });
    byId('add-relationship').addEventListener('click', () => addRelationshipRow('', { aff: 0, type: 'neutral' }));
    byId('voice-filter-preview').addEventListener('click', requestVoiceFilterPreview);
    voiceFilterControl('tts_filter_preset').addEventListener('change', () => {
        renderVoiceFilterDescription();
        invalidateVoiceFilterPreview();
    });
    voiceFilterControl('voiceid').addEventListener('input', invalidateVoiceFilterPreview);
    voiceFilterControl('profile_id').addEventListener('change', invalidateVoiceFilterPreview);
    form.addEventListener('submit', saveNpc);
    document.addEventListener('keydown', (event) => {
        if (event.key !== 'Escape') return;
        if (!byId('editor-backdrop').classList.contains('hidden')) {
            event.stopImmediatePropagation();
            closeEditor();
        } else if (!embeddedInSettings) {
            sendCommand('close');
        }
    });
    if (!embeddedInSettings) {
        document.addEventListener('focusin', (event) => {
            if (event.target.matches('input, textarea, select')) sendCommand('input_capture|on');
        });
        document.addEventListener('focusout', () => {
            window.setTimeout(() => {
                if (!document.activeElement || !document.activeElement.matches('input, textarea, select')) {
                    sendCommand('input_capture|off');
                }
            }, 0);
        });
    }
    window.addEventListener('DOMContentLoaded', () => sendCommand('dom_ready'));
}());
