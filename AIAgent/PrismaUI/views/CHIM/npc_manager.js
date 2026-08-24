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

    const RUNTIME_REFID_PREFIX = 'FF';
    const NO_REFID_LABEL = 'No RefID';
    const UNKNOWN_SOURCE_LABEL = 'Unknown source';

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
    const embeddedInSettings = !!byId('npcs-page');

    // Same-named actors are told apart by RefID and source mod, never by the visible name.
    function normalizeRefid(value) {
        const raw = String(value == null ? '' : value).trim().replace(/^0x/i, '').toUpperCase();
        return /^[0-9A-F]{1,8}$/.test(raw) ? raw.padStart(8, '0') : '';
    }

    function refidDisplay(value) {
        const normalized = normalizeRefid(value);
        if (!normalized) return { text: NO_REFID_LABEL, runtime: false, known: false };
        return { text: normalized, runtime: normalized.startsWith(RUNTIME_REFID_PREFIX), known: true };
    }

    // Herika stores metadata.mods ordered: first entry defines the actor, last may override it.
    function modChain(source) {
        let raw = null;
        if (source && typeof source === 'object') {
            if (Array.isArray(source.mod_chain)) raw = source.mod_chain;
            else if (Array.isArray(source.mods) || typeof source.mods === 'string') raw = source.mods;
            else if (source.metadata && typeof source.metadata === 'object') raw = source.metadata.mods;
        }
        if (typeof raw === 'string') raw = raw.split(/[#,\r\n]+/);
        if (!Array.isArray(raw)) return [];
        return raw
            .map((entry) => String(entry == null ? '' : entry).trim())
            .filter(Boolean);
    }

    function definingMod(source) {
        const explicit = String((source && source.source_mod) || '').trim();
        if (explicit) return explicit;
        const chain = modChain(source);
        return chain.length ? chain[0] : '';
    }

    function duplicateCount(npc) {
        const count = Number(npc && npc.duplicate_count);
        return Number.isFinite(count) && count > 1 ? Math.floor(count) : 1;
    }

    function actorKeyLabel(npc) {
        const key = String((npc && npc.actor_key) || '').trim();
        return key || 'Not recorded';
    }

    // Users type RefIDs either way; the stored column has no 0x prefix.
    function normalizeSearchTerm(value) {
        const term = String(value == null ? '' : value).trim();
        const stripped = term.replace(/^0x/i, '');
        if (stripped.length !== term.length && /^[0-9A-Fa-f]{1,8}$/.test(stripped)) return stripped;
        return term;
    }

    function srOnly(text) {
        const element = document.createElement('span');
        element.className = 'sr-only';
        element.textContent = text;
        return element;
    }

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
        const byRefid = new Map();
        const nameCounts = new Map();
        nearbyTargets.forEach((target) => {
            const refid = normalizeRefid(target.refid);
            if (refid) byRefid.set(refid, target);
            const name = String(target.name || '').trim().toLowerCase();
            if (name) nameCounts.set(name, (nameCounts.get(name) || 0) + 1);
        });
        // Only fall back to a name match when that name is unambiguous among nearby actors.
        const byUniqueName = new Map();
        nearbyTargets.forEach((target) => {
            const name = String(target.name || '').trim().toLowerCase();
            if (name && nameCounts.get(name) === 1) byUniqueName.set(name, target);
        });
        return { byRefid, byUniqueName };
    }

    function findNearbyTarget(lookup, npc) {
        const refid = normalizeRefid(npc && npc.refid);
        if (refid && lookup.byRefid.has(refid)) return lookup.byRefid.get(refid);
        if (duplicateCount(npc) > 1) return null;
        const name = String((npc && npc.name) || '').trim().toLowerCase();
        return (name && lookup.byUniqueName.get(name)) || null;
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
        const search = normalizeSearchTerm(byId('search-input').value);
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
        const targets = new Map();
        ordered.forEach((npc) => targets.set(npc, findNearbyTarget(lookup, npc)));
        const distanceOf = (npc) => {
            const target = targets.get(npc);
            return Number((target && target.distance) || 99999);
        };
        ordered.sort((left, right) => {
            const byDistance = distanceOf(left) - distanceOf(right);
            if (byDistance !== 0) return byDistance;
            // Same-named profiles must keep a stable, identity-based order.
            const byName = String(left.name || '').localeCompare(String(right.name || ''));
            if (byName !== 0) return byName;
            const byRefid = normalizeRefid(left.refid).localeCompare(normalizeRefid(right.refid));
            if (byRefid !== 0) return byRefid;
            return Number(left.id || 0) - Number(right.id || 0);
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
            const target = targets.get(npc);
            const duplicates = duplicateCount(npc);
            const card = document.createElement('button');
            card.type = 'button';
            card.className = 'npc-card';
            // Every row action is keyed by the database row id, never by the visible name.
            card.dataset.id = String(npc.id);
            const portrait = document.createElement('img');
            portrait.src = resolveAssetUrl(npc.portrait_url);
            portrait.alt = '';
            const copy = document.createElement('div');
            copy.className = 'npc-card-copy';
            const nameRow = document.createElement('div');
            nameRow.className = 'npc-card-name-row';
            const name = document.createElement('div');
            name.className = 'npc-card-name';
            name.textContent = npc.name || 'Unknown NPC';
            nameRow.appendChild(name);
            if (duplicates > 1) nameRow.appendChild(duplicateBadge(duplicates));
            copy.appendChild(nameRow);
            const meta = document.createElement('div');
            meta.className = 'npc-card-meta';
            meta.textContent = [npc.race, npc.gender].filter(Boolean).join(' · ') || 'Unknown race';
            copy.appendChild(meta);
            copy.appendChild(buildIdentityLine(npc));
            const profile = document.createElement('div');
            profile.className = 'npc-card-profile';
            profile.textContent = npc.profile_label || 'No Profile';
            copy.appendChild(profile);
            const flags = document.createElement('div');
            flags.className = 'npc-card-flags';
            if (target) flags.appendChild(pill(`${Number(target.distance || 0).toFixed(1)}m`, 'nearby'));
            if (npc.favorite) flags.appendChild(pill('Favorite', 'good'));
            if (npc.locked) flags.appendChild(pill('Locked'));
            copy.appendChild(flags);
            card.append(portrait, copy);
            const chain = modChain(npc);
            if (chain.length > 1) card.appendChild(buildChainTooltip(chain));
            card.addEventListener('click', () => openEditor(npc.id));
            grid.appendChild(card);
        });
    }

    function duplicateBadge(count) {
        const badge = document.createElement('span');
        badge.className = 'npc-card-dup';
        const symbol = document.createElement('span');
        symbol.setAttribute('aria-hidden', 'true');
        symbol.textContent = `×${count}`;
        badge.append(symbol, srOnly(`${count} profiles share this name`));
        return badge;
    }

    // Compact identity line: RefID plus the mod that defines this actor.
    function buildIdentityLine(npc) {
        const line = document.createElement('div');
        line.className = 'npc-card-identity';
        const refid = refidDisplay(npc && npc.refid);
        const refidElement = document.createElement('span');
        refidElement.className = `npc-card-refid${refid.known ? '' : ' unknown'}`;
        refidElement.append(srOnly('Ref ID '), document.createTextNode(refid.text));
        line.appendChild(refidElement);
        if (refid.runtime) {
            const runtime = document.createElement('span');
            runtime.className = 'npc-card-runtime';
            runtime.textContent = 'Runtime';
            runtime.title = 'FF RefIDs are assigned at runtime and can change between saves.';
            line.appendChild(runtime);
        }
        const separator = document.createElement('span');
        separator.className = 'npc-card-sep';
        separator.setAttribute('aria-hidden', 'true');
        separator.textContent = '·';
        line.appendChild(separator);
        const chain = modChain(npc);
        const source = definingMod(npc);
        const sourceElement = document.createElement('span');
        sourceElement.className = `npc-card-source${source ? '' : ' unknown'}`;
        sourceElement.append(srOnly('Source mod '), document.createTextNode(source || UNKNOWN_SOURCE_LABEL));
        // A single-entry chain gets no tooltip, so keep a native title for truncated names.
        if (chain.length === 1) sourceElement.title = chain[0];
        line.appendChild(sourceElement);
        return line;
    }

    // Held outside the card body so the full chain stays reachable on hover and focus
    // without lengthening the card itself.
    function buildChainTooltip(chain) {
        const tooltip = document.createElement('span');
        tooltip.className = 'npc-card-chain';
        const heading = document.createElement('span');
        heading.className = 'npc-card-chain-title';
        heading.textContent = 'Mod chain';
        tooltip.appendChild(heading);
        chain.forEach((mod, index) => {
            const entry = document.createElement('span');
            entry.className = 'npc-card-chain-entry';
            const label = document.createElement('span');
            label.className = 'npc-card-chain-mod';
            label.textContent = mod;
            const role = document.createElement('span');
            role.className = 'npc-card-chain-role';
            role.textContent = index === 0
                ? 'defining'
                : (index === chain.length - 1 ? 'final override' : 'override');
            entry.append(label, role);
            tooltip.appendChild(entry);
        });
        return tooltip;
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
        byId('editor-subtitle').textContent = editorSubtitle(detail.card);
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
        renderIdentityPanel(detail);
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

    // The title stays the visible NPC name, so the subtitle carries the disambiguators.
    function editorSubtitle(card) {
        const refid = refidDisplay(card && card.refid);
        const duplicates = duplicateCount(card);
        const parts = [card && card.race, card && card.profile_label].filter(Boolean);
        parts.push(refid.runtime ? `${refid.text} (Runtime)` : refid.text);
        parts.push(definingMod(card) || UNKNOWN_SOURCE_LABEL);
        if (duplicates > 1) parts.push(`${duplicates} profiles share this name`);
        return parts.join(' · ');
    }

    // RefID, actor key and source metadata are identity, not editable profile data.
    function renderIdentityPanel(detail) {
        if (!byId('identity-refid')) return;
        const card = (detail && detail.card) || {};
        const metadata = (detail && detail.metadata && typeof detail.metadata === 'object') ? detail.metadata : {};
        const source = Object.assign({ metadata }, card);
        if (!source.metadata) source.metadata = metadata;
        const refid = refidDisplay(card.refid);
        const duplicates = duplicateCount(card);
        const chain = modChain(source);

        byId('identity-refid').textContent = refid.text;
        const runtimeNote = byId('identity-refid-runtime');
        runtimeNote.hidden = !refid.runtime;
        byId('identity-actor-key').textContent = actorKeyLabel(card);
        byId('identity-source').textContent = (String(card.source_mod || '').trim() || chain[0] || '') || UNKNOWN_SOURCE_LABEL;
        byId('identity-duplicates').textContent = duplicates > 1
            ? `${duplicates} profiles share the name "${card.name || 'Unknown NPC'}"`
            : 'This name is unique';

        const list = byId('identity-chain');
        list.replaceChildren();
        if (!chain.length) {
            const empty = document.createElement('li');
            empty.className = 'identity-chain-empty';
            empty.textContent = 'No source mod chain recorded for this actor.';
            list.appendChild(empty);
            return;
        }
        chain.forEach((mod, index) => {
            const item = document.createElement('li');
            const label = document.createElement('span');
            label.className = 'identity-chain-mod';
            label.textContent = mod;
            const role = index === 0
                ? 'Defining'
                : (index === chain.length - 1 ? 'Final override' : 'Override');
            item.append(label, pill(role, index === 0 ? 'good' : ''));
            list.appendChild(item);
        });
    }

    function setHistoryStatus(message, error) {
        const status = byId('history-status');
        status.textContent = message || '';
        status.classList.toggle('error', !!error);
    }

    function recipientEntry(card) {
        const refid = refidDisplay(card && card.refid);
        return {
            name: String((card && card.name) || 'NPC'),
            refid: refid.text,
            source: definingMod(card) || UNKNOWN_SOURCE_LABEL
        };
    }

    function resetNpcHistory(card) {
        historyRecipients.clear();
        historyRecipients.set(Number(card.id), recipientEntry(card));
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
        historyRecipients.forEach((entry, id) => {
            const name = entry && entry.name ? entry.name : String(entry || 'NPC');
            const refid = (entry && entry.refid) || NO_REFID_LABEL;
            const chip = document.createElement('span');
            chip.className = 'history-recipient-chip';
            const label = document.createElement('span');
            label.textContent = name;
            const identity = document.createElement('span');
            identity.className = 'history-recipient-refid';
            identity.textContent = refid;
            chip.append(label, identity);
            if (Number(id) !== currentNpcId) {
                const remove = document.createElement('button');
                remove.type = 'button';
                remove.textContent = 'x';
                remove.setAttribute('aria-label', `Remove ${name} (${refid})`);
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
            const entry = recipientEntry(npc);
            const button = document.createElement('button');
            button.type = 'button';
            button.className = 'history-search-result';
            const label = document.createElement('span');
            label.className = 'history-search-result-name';
            label.textContent = entry.name;
            const identity = document.createElement('span');
            identity.className = 'history-search-result-identity';
            identity.textContent = `${entry.refid} · ${entry.source}`;
            button.append(label, identity);
            button.addEventListener('click', () => {
                historyRecipients.set(Number(npc.id), entry);
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
