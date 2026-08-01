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

    function sendCommand(command) {
        if (window.chimNpcManagerCommand) window.chimNpcManagerCommand(command);
    }

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
        byId('save-status').textContent = '';
        sendCommand('input_capture|on');
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
        renderFeatureToggles(detail.toggles || {});
        renderRelationships(detail.relationships || {});
        byId('relationships-locked').checked = !!detail.relationships_locked;
        byId('metadata-output').textContent = JSON.stringify(detail.metadata || {}, null, 2);
        byId('bgl-inception-idea').value = '';
        byId('action-status').textContent = '';
        byId('action-status').classList.remove('error');
        switchEditorTab('general');
        byId('save-status').textContent = '';
        byId('save-status').classList.remove('error');
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
        const remove = document.createElement('button');
        remove.type = 'button';
        remove.className = 'remove-relationship';
        remove.textContent = '×';
        remove.title = 'Remove relationship';
        remove.addEventListener('click', () => row.remove());
        row.append(targetInput, affinity, type, note, remove);
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
                aff: Number(row.querySelector('.relationship-affinity').value || 0),
                type: row.querySelector('.relationship-type').value,
                note: row.querySelector('.relationship-note').value.trim()
            };
        });
        return relationships;
    }

    function collectFields() {
        const names = [
            'npc_name', 'profile_id', 'lock_profile', 'npc_favorite', 'gender', 'race', 'base',
            'refid', 'voiceid', 'oghma_knowledge_tags', 'tags', 'prompt_head', 'core',
            'npc_static_bio', 'appearance', 'personality', 'occupation', 'skills', 'speechstyle',
            'goals', 'emote_moods', 'middle_term_latest'
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
        const status = byId('action-status');
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
        } catch (error) {
            status.textContent = `Action failed: ${error.message || error}`;
            status.classList.add('error');
        } finally {
            button.disabled = false;
        }
    }

    function closeEditor() {
        byId('editor-backdrop').classList.add('hidden');
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
    byId('close-button').addEventListener('click', () => sendCommand('close'));
    byId('editor-close').addEventListener('click', closeEditor);
    byId('cancel-button').addEventListener('click', closeEditor);
    byId('visit-action').addEventListener('click', (event) => runNpcAction('visit', event.currentTarget));
    byId('teleport-action').addEventListener('click', (event) => runNpcAction('teleport', event.currentTarget));
    byId('bgl-inception-action').addEventListener('click', (event) => runNpcAction('bgl_inception', event.currentTarget));
    byId('add-relationship').addEventListener('click', () => addRelationshipRow('', { aff: 0, type: 'neutral' }));
    form.addEventListener('submit', saveNpc);
    document.addEventListener('keydown', (event) => {
        if (event.key !== 'Escape') return;
        if (!byId('editor-backdrop').classList.contains('hidden')) closeEditor();
        else sendCommand('close');
    });
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
    window.addEventListener('DOMContentLoaded', () => sendCommand('dom_ready'));
}());
