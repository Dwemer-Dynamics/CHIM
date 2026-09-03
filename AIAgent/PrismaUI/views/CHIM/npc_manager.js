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

    // Sharing state rides along with both list cards and the detail payload. A group can be linked
    // automatically by the server, and auto_link_disabled outlives the link itself, so it is read
    // even when the row is no longer shared.
    function sharingState(source) {
        const sharing = source && source.profile_sharing;
        if (!sharing || typeof sharing !== 'object') {
            return { linked: false, ownerId: 0, members: [], automatic: false, autoLinkDisabled: false };
        }
        const autoLinkDisabled = !!sharing.auto_link_disabled;
        if (!sharing.linked) {
            return { linked: false, ownerId: 0, members: [], automatic: false, autoLinkDisabled };
        }
        return {
            linked: true,
            ownerId: Number(sharing.owner_id || 0),
            members: Array.isArray(sharing.members) ? sharing.members : [],
            automatic: !!sharing.automatic,
            autoLinkDisabled
        };
    }

    // The reference origin is the plugin recorded in refid_source, not the first entry of
    // metadata.mods: a later plugin can override an actor without owning its reference.
    function referenceOrigin(value) {
        const raw = String(value == null ? '' : value).trim();
        if (!raw) return '';
        // refid_source is "<plugin>|<local form id>"; the RefID is already shown beside it, so
        // the origin reads as the plugin that owns the reference.
        const match = raw.match(/^([^/\\|@#:]+\.es[mpl])(?:[/|][0-9A-Fa-f]{1,8})?$/);
        return match ? match[1] : raw;
    }

    // Every action, injected event and speech target stays bound to the physical actor the
    // operator opened, even when that actor only borrows a shared profile.
    function selectedActor() {
        const card = (currentDetail && currentDetail.card) || {};
        return { id: Number(byId('npc-id').value || 0), refid: normalizeRefid(card.refid) };
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
            if (sharingState(npc).linked) flags.appendChild(pill('Shared profile', 'shared'));
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
        renderSharingPanel(detail);
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

    // RefID and source metadata are identity, not editable profile data.
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

    // Prisma v1 cannot merge or unlink. It states that the profile is shared and, more
    // importantly, which profile an edit saved here actually lands in.
    function renderSharingPanel(detail) {
        const panel = byId('sharing-panel');
        const banner = byId('editor-shared');
        const autoBadge = byId('sharing-auto');
        const autoOff = byId('sharing-auto-off');
        if (!panel) return;
        const card = (detail && detail.card) || {};
        const sharing = sharingState(detail);
        if (banner) banner.hidden = !sharing.linked;
        if (autoBadge) autoBadge.hidden = !(sharing.linked && sharing.automatic);
        if (autoOff) autoOff.hidden = !sharing.autoLinkDisabled;
        panel.hidden = !sharing.linked;
        // Renaming one linked actor would invalidate its stored identity, so the server refuses it.
        // Say so on the control instead of letting the save fail.
        const nameField = form.elements.namedItem('npc_name');
        if (nameField) {
            nameField.readOnly = sharing.linked;
            if (sharing.linked) nameField.title = 'Locked while this profile is shared. Unlink the profiles to rename this actor.';
            else nameField.removeAttribute('title');
        }
        if (!sharing.linked) return;

        const isOwner = sharing.ownerId === Number(card.id || 0);
        const owner = sharing.members.find((member) => Number(member.id) === sharing.ownerId);
        const ownerName = String((owner && owner.name) || card.name || '').trim() || 'another actor';
        const ownerRefid = refidDisplay(owner && owner.refid).text;
        const lands = isOwner
            ? 'Biography, personality, goals, voice, relationships and personal memory are shared. Physical details, RefID, favorite and lock stay with this actor.'
            : `Biography, personality, goals, voice, relationships and personal memory use ${ownerName}'s kept profile (${ownerRefid}). Physical details, RefID, favorite and lock stay with this actor.`;
        // Members of an automatic group can be recorded under different names, so each row below is
        // labelled with the name the server reported for it.
        const automaticLine = sharing.automatic
            ? 'These references are known to be one character, so CHIM linked them automatically to the kept profile. '
            : '';
        byId('sharing-explainer').textContent = `${automaticLine}${lands} The name is locked while the profile is shared.`;

        const list = byId('sharing-members');
        list.replaceChildren();
        if (!sharing.members.length) {
            const empty = document.createElement('li');
            empty.className = 'sharing-empty';
            empty.textContent = 'No actors are listed for this shared profile.';
            list.appendChild(empty);
            return;
        }
        sharing.members.forEach((member) => {
            const item = document.createElement('li');
            item.className = 'sharing-member';
            const name = document.createElement('span');
            name.className = 'sharing-member-name';
            name.textContent = String(member.name || 'Unknown NPC');
            item.appendChild(name);
            if (Number(member.id) === sharing.ownerId) item.appendChild(pill('Kept profile', 'good'));
            if (Number(member.id) === Number(card.id || 0)) item.appendChild(pill('This actor', 'nearby'));
            const identity = document.createElement('span');
            identity.className = 'sharing-member-identity';
            const refid = refidDisplay(member.refid);
            const refidNode = document.createElement('span');
            refidNode.className = `sharing-member-refid${refid.known ? '' : ' unknown'}`;
            refidNode.append(srOnly('Ref ID '), document.createTextNode(refid.text));
            const origin = referenceOrigin(member.refid_source);
            const originNode = document.createElement('span');
            originNode.className = `sharing-member-origin${origin ? '' : ' unknown'}`;
            originNode.append(srOnly('Reference origin '), document.createTextNode(origin || 'Unknown plugin'));
            identity.append(refidNode, originNode);
            item.appendChild(identity);
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
        const actor = selectedActor();
        const injectButton = byId('history-inject');
        injectButton.disabled = true;
        setHistoryStatus('Injecting event...', false);
        try {
            const data = await parseResponse(await fetch(`${serverBaseUrl}/ui/api/chim_npc_manager.php`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    operation: 'inject_event',
                    id: actor.id,
                    refid: actor.refid,
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
        const actor = selectedActor();
        const saveButton = byId('save-button');
        saveButton.disabled = true;
        byId('save-status').textContent = 'Saving NPC profile...';
        byId('save-status').classList.remove('error');
        try {
            currentDetail = await parseResponse(await fetch(`${serverBaseUrl}/ui/api/chim_npc_manager.php`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    id: actor.id,
                    refid: actor.refid,
                    profile_revision: String((currentDetail && currentDetail.profile_revision) || ''),
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

        const actor = selectedActor();
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
                    id: actor.id,
                    refid: actor.refid,
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

    // ---------------------------------------------------------------------------
    // Reference groups
    //
    // A group names the exact placed references that are one character, so those actors share a
    // single profile. CHIM ships a built-in list; a custom row carrying the same group key replaces
    // the built-in entry until it is reset. The shell below is built here rather than in the page
    // markup because the settings view hosts this same NPC page from its own document.
    // ---------------------------------------------------------------------------
    const referenceState = {
        defaults: [],
        custom: [],
        loading: false,
        saving: false,
        failed: false,
        editing: null,
        returnFocus: null,
        editorReturnFocus: null,
        editorReturnKey: ''
    };

    const REFERENCE_MODAL_MARKUP = `
<section id="reference-modal" class="editor-modal reference-modal" role="dialog" aria-modal="true"
         aria-labelledby="reference-title" aria-describedby="reference-intro">
    <header class="editor-header">
        <div class="reference-heading">
            <h2 id="reference-title">Reference Groups</h2>
            <p id="reference-intro" class="reference-intro">A group lists the exact placed references that are the same character, so they share one profile. Changes take effect the next time those actors register. Existing links remain until you unlink them in the web NPC Manager.</p>
        </div>
        <button id="reference-close" class="icon-button" type="button" aria-label="Close reference groups">&times;</button>
    </header>
    <div class="reference-body">
        <div class="reference-status-line">
            <span id="reference-status" class="reference-status" role="status" aria-live="polite"></span>
            <span id="reference-count" class="reference-count"></span>
        </div>

        <form id="reference-editor" class="reference-editor" autocomplete="off" aria-labelledby="reference-editor-title" hidden>
            <h3 id="reference-editor-title">Add a group</h3>
            <p id="reference-editor-note" class="reference-editor-note" hidden></p>
            <div class="reference-editor-grid">
                <label class="form-field">
                    <span>Character or group name</span>
                    <input id="reference-display-name" type="text" maxlength="120" placeholder="Sigrid">
                </label>
                <label class="form-field">
                    <span>Plugin file</span>
                    <input id="reference-plugin-name" type="text" maxlength="120" spellcheck="false" placeholder="Skyrim.esm">
                </label>
            </div>
            <label class="form-field">
                <span>Reference FormIDs</span>
                <textarea id="reference-formids" class="reference-formids" rows="3" spellcheck="false"
                          aria-describedby="reference-formids-hint" placeholder="0001A66C"></textarea>
            </label>
            <p id="reference-formids-hint" class="reference-hint">Local FormIDs from the plugin above, without the load order prefix. One per line, or separated by spaces or commas.</p>
            <label class="check-field"><input id="reference-enabled" type="checkbox"><span>Enabled</span></label>
            <div class="reference-editor-actions">
                <span id="reference-editor-status" class="reference-editor-status" role="status" aria-live="polite"></span>
                <button id="reference-cancel" class="button secondary compact" type="button">Cancel</button>
                <button id="reference-save" class="button primary compact" type="submit">Save group</button>
            </div>
        </form>

        <section class="reference-section" aria-labelledby="reference-defaults-heading">
            <div class="reference-section-heading">
                <div>
                    <h3 id="reference-defaults-heading">Built-in defaults</h3>
                    <p>Shipped with CHIM. Customize one to keep your own version of it.</p>
                </div>
            </div>
            <div class="reference-table-wrap">
                <table class="reference-table">
                    <thead>
                        <tr>
                            <th scope="col">Character or group</th>
                            <th scope="col">Plugin</th>
                            <th scope="col">Reference FormIDs</th>
                            <th scope="col">Status</th>
                            <th scope="col">Actions</th>
                        </tr>
                    </thead>
                    <tbody id="reference-defaults-body"></tbody>
                </table>
            </div>
        </section>

        <section class="reference-section" aria-labelledby="reference-custom-heading">
            <div class="reference-section-heading">
                <div>
                    <h3 id="reference-custom-heading">Your groups</h3>
                    <p>Groups you added, plus your versions of built-in ones. Resetting a version brings the built-in group back.</p>
                </div>
                <button id="reference-add" class="button secondary compact" type="button"
                        aria-controls="reference-editor" aria-expanded="false">Add group</button>
            </div>
            <div class="reference-table-wrap">
                <table class="reference-table">
                    <thead>
                        <tr>
                            <th scope="col">Character or group</th>
                            <th scope="col">Plugin</th>
                            <th scope="col">Reference FormIDs</th>
                            <th scope="col">Kind</th>
                            <th scope="col">State</th>
                            <th scope="col">Actions</th>
                        </tr>
                    </thead>
                    <tbody id="reference-custom-body"></tbody>
                </table>
            </div>
        </section>
    </div>
    <footer class="editor-actions reference-footer">
        <button id="reference-refresh" class="button secondary compact" type="button">Refresh</button>
        <button id="reference-done" class="button secondary" type="button">Close</button>
    </footer>
</section>`;

    function toBoolean(value, fallback) {
        if (value === undefined || value === null || value === '') return !!fallback;
        if (typeof value === 'string') return !/^(0|false|no|off)$/i.test(value.trim());
        return !!value;
    }

    // Local FormIDs are stored without the load order prefix, so only the plugin-local digits are kept.
    function normalizeLocalFormid(value) {
        const raw = String(value == null ? '' : value).trim().replace(/^0x/i, '').toUpperCase();
        return /^[0-9A-F]{1,8}$/.test(raw) ? raw.padStart(8, '0') : '';
    }

    // Leading zeros are cosmetic, so short input and canonical eight-digit input deduplicate.
    function formidKey(id) {
        return id.replace(/^0+/, '') || '0';
    }

    function normalizeFormidList(value) {
        let raw = value;
        if (typeof raw === 'string') raw = raw.split(/[\s,;]+/);
        if (!Array.isArray(raw)) return [];
        const ids = [];
        const seen = new Set();
        raw.forEach((entry) => {
            const id = normalizeLocalFormid(entry);
            if (!id || seen.has(formidKey(id))) return;
            seen.add(formidKey(id));
            ids.push(id);
        });
        return ids;
    }

    // Operators paste FormIDs in every shape, so any run of whitespace, comma or semicolon separates
    // them. Unreadable entries are reported instead of being dropped in silence.
    function readFormidField(text) {
        const ids = [];
        const invalid = [];
        const seen = new Set();
        String(text == null ? '' : text).split(/[\s,;]+/).filter(Boolean).forEach((token) => {
            const id = normalizeLocalFormid(token);
            if (!id) {
                invalid.push(token);
                return;
            }
            if (seen.has(formidKey(id))) return;
            seen.add(formidKey(id));
            ids.push(id);
        });
        return { ids, invalid };
    }

    function referenceRow(row, isDefault) {
        const source = row && typeof row === 'object' ? row : {};
        return {
            key: String(source.group_key == null ? '' : source.group_key).trim(),
            name: String(source.display_name == null ? '' : source.display_name).trim(),
            plugin: String(source.plugin_name == null ? '' : source.plugin_name).trim(),
            formids: normalizeFormidList(source.local_formids),
            enabled: toBoolean(source.enabled, true),
            overridesDefault: !isDefault && toBoolean(source.overrides_default, false)
        };
    }

    function referenceModalOpen() {
        const backdrop = byId('reference-backdrop');
        return !!backdrop && !backdrop.classList.contains('hidden');
    }

    function referenceEditorOpen() {
        const editor = byId('reference-editor');
        return !!editor && !editor.hidden;
    }

    function setReferenceStatus(message, error, good) {
        const status = byId('reference-status');
        if (!status) return;
        status.textContent = message || '';
        status.classList.toggle('error', !!error);
        status.classList.toggle('good', !error && !!good);
    }

    function setReferenceEditorStatus(message, error) {
        const status = byId('reference-editor-status');
        if (!status) return;
        status.textContent = message || '';
        status.classList.toggle('error', !!error);
    }

    function referenceCell(label, className) {
        const cell = document.createElement('td');
        cell.dataset.label = label;
        if (className) cell.className = className;
        return cell;
    }

    function referenceTextCell(label, text, className, muted) {
        const cell = referenceCell(label, className);
        if (muted) cell.classList.add('reference-muted');
        cell.textContent = text;
        return cell;
    }

    function referenceFormidCell(group) {
        return group.formids.length
            ? referenceTextCell('Reference FormIDs', group.formids.join(', '), 'reference-formid-list')
            : referenceTextCell('Reference FormIDs', 'None listed', 'reference-formid-list', true);
    }

    function referenceMessageRow(text, columns, error) {
        const row = document.createElement('tr');
        const cell = document.createElement('td');
        cell.className = `reference-cell-message${error ? ' error' : ''}`;
        cell.colSpan = columns;
        cell.textContent = text;
        row.appendChild(cell);
        return row;
    }

    function referenceActionButton(label, action, group, danger) {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = `reference-action${danger ? ' danger' : ''}`;
        button.textContent = label;
        button.dataset.action = action;
        button.dataset.groupKey = group.key;
        button.disabled = referenceState.saving;
        return button;
    }

    // Row buttons are rebuilt on every refresh, so focus is restored by group key, not by element.
    function referenceActionFor(key) {
        const modal = byId('reference-modal');
        if (!modal || !key) return null;
        return Array.from(modal.querySelectorAll('.reference-action'))
            .find((button) => button.dataset.groupKey === key) || null;
    }

    function customGroupFor(key) {
        return referenceState.custom.find((entry) => entry.key && entry.key === key) || null;
    }

    function renderReferenceDefaults() {
        const body = byId('reference-defaults-body');
        if (!body) return;
        body.replaceChildren();
        if (referenceState.loading) {
            body.appendChild(referenceMessageRow('Loading groups...', 5));
            return;
        }
        if (referenceState.failed) {
            body.appendChild(referenceMessageRow('Groups could not be loaded.', 5, true));
            return;
        }
        if (!referenceState.defaults.length) {
            body.appendChild(referenceMessageRow('No built-in groups are available.', 5));
            return;
        }
        referenceState.defaults.forEach((group) => {
            const override = customGroupFor(group.key);
            const row = document.createElement('tr');
            row.className = `reference-row${override ? ' overridden' : ''}`;
            row.append(
                referenceTextCell('Character or group', group.name || 'Unnamed group', 'reference-name'),
                referenceTextCell('Plugin', group.plugin || 'Unknown plugin', 'reference-plugin'),
                referenceFormidCell(group)
            );
            const status = referenceCell('Status');
            status.appendChild(override
                ? pill('Overridden')
                : (group.enabled ? pill('In use', 'good') : pill('Disabled')));
            row.appendChild(status);

            const actions = referenceCell('Actions');
            const wrap = document.createElement('div');
            wrap.className = 'reference-row-actions';
            const label = override ? 'Edit override' : 'Customize';
            const button = referenceActionButton(label, override ? 'edit' : 'customize', group);
            button.setAttribute('aria-label', `${label} ${group.name || 'group'}`);
            button.addEventListener('click', () => {
                const current = customGroupFor(group.key);
                openReferenceEditor(current ? 'edit' : 'customize', current || group, button);
            });
            wrap.appendChild(button);
            actions.appendChild(wrap);
            row.appendChild(actions);
            body.appendChild(row);
        });
    }

    function renderReferenceCustom() {
        const body = byId('reference-custom-body');
        if (!body) return;
        body.replaceChildren();
        if (referenceState.loading) {
            body.appendChild(referenceMessageRow('Loading groups...', 6));
            return;
        }
        if (referenceState.failed) {
            body.appendChild(referenceMessageRow('Groups could not be loaded.', 6, true));
            return;
        }
        if (!referenceState.custom.length) {
            body.appendChild(referenceMessageRow('You have not added or customized any groups yet.', 6));
            return;
        }
        const defaultKeys = new Set(referenceState.defaults.map((group) => group.key).filter(Boolean));
        referenceState.custom.forEach((group) => {
            const isOverride = group.overridesDefault || defaultKeys.has(group.key);
            const row = document.createElement('tr');
            row.className = 'reference-row';
            row.append(
                referenceTextCell('Character or group', group.name || 'Unnamed group', 'reference-name'),
                referenceTextCell('Plugin', group.plugin || 'Unknown plugin', 'reference-plugin'),
                referenceFormidCell(group)
            );
            const kind = referenceCell('Kind');
            kind.appendChild(isOverride ? pill('Replaces a built-in') : pill('Custom'));
            row.appendChild(kind);
            const state = referenceCell('State');
            state.appendChild(group.enabled ? pill('Enabled', 'good') : pill('Disabled'));
            row.appendChild(state);

            const actions = referenceCell('Actions');
            const wrap = document.createElement('div');
            wrap.className = 'reference-row-actions';
            const edit = referenceActionButton('Edit', 'edit', group);
            edit.setAttribute('aria-label', `Edit ${group.name || 'group'}`);
            edit.addEventListener('click', () => openReferenceEditor('edit', group, edit));
            const removeLabel = isOverride ? 'Reset' : 'Delete';
            const remove = referenceActionButton(removeLabel, isOverride ? 'reset' : 'delete', group, true);
            remove.setAttribute('aria-label', isOverride
                ? `Reset ${group.name || 'group'} to the built-in group`
                : `Delete ${group.name || 'group'}`);
            remove.addEventListener('click', () => deleteReferenceGroup(group, isOverride));
            wrap.append(edit, remove);
            actions.appendChild(wrap);
            row.appendChild(actions);
            body.appendChild(row);
        });
    }

    function renderReferenceTables() {
        renderReferenceDefaults();
        renderReferenceCustom();
        const count = byId('reference-count');
        if (!count) return;
        if (referenceState.loading || referenceState.failed) {
            count.textContent = '';
            return;
        }
        const builtin = referenceState.defaults.length;
        const custom = referenceState.custom.length;
        count.textContent = `${builtin} built-in · ${custom} custom`;
    }

    function setReferenceBusy(saving) {
        referenceState.saving = !!saving;
        const modal = byId('reference-modal');
        if (!modal) return;
        modal.querySelectorAll('.reference-action').forEach((button) => { button.disabled = !!saving; });
        byId('reference-save').disabled = !!saving;
        byId('reference-cancel').disabled = !!saving;
        byId('reference-add').disabled = !!saving;
        byId('reference-refresh').disabled = !!saving;
    }

    function openReferenceEditor(mode, group, invoker) {
        const editor = byId('reference-editor');
        if (!editor) return;
        const source = group || { key: '', name: '', plugin: '', formids: [], enabled: true };
        // A customized built-in keeps the built-in key so the server knows which entry it replaces.
        referenceState.editing = { mode, key: mode === 'add' ? '' : source.key };
        referenceState.editorReturnFocus = invoker || byId('reference-add');
        referenceState.editorReturnKey = mode === 'add' ? '' : source.key;
        byId('reference-editor-title').textContent = mode === 'add'
            ? 'Add a group'
            : (mode === 'customize' ? 'Customize a built-in group' : 'Edit a group');
        const note = byId('reference-editor-note');
        if (mode === 'customize') {
            note.textContent = 'Your version is used instead of the built-in one. Reset it later to go back.';
            note.hidden = false;
        } else if (mode === 'add') {
            note.textContent = 'List every placed reference that is the same character.';
            note.hidden = false;
        } else {
            note.textContent = '';
            note.hidden = true;
        }
        byId('reference-display-name').value = source.name || '';
        byId('reference-plugin-name').value = source.plugin || '';
        byId('reference-formids').value = (source.formids || []).join('\n');
        byId('reference-enabled').checked = source.enabled !== false;
        setReferenceEditorStatus('', false);
        editor.hidden = false;
        byId('reference-add').setAttribute('aria-expanded', 'true');
        if (typeof editor.scrollIntoView === 'function') editor.scrollIntoView({ block: 'nearest' });
        const first = byId('reference-display-name');
        first.focus();
        if (typeof first.select === 'function') first.select();
    }

    function closeReferenceEditor(focusTarget) {
        const editor = byId('reference-editor');
        if (!editor || editor.hidden) return;
        const key = referenceState.editorReturnKey;
        const invoker = referenceState.editorReturnFocus;
        editor.hidden = true;
        referenceState.editing = null;
        referenceState.editorReturnFocus = null;
        referenceState.editorReturnKey = '';
        setReferenceEditorStatus('', false);
        byId('reference-add').setAttribute('aria-expanded', 'false');
        // Pass false when the whole modal is closing, so focus is not moved twice.
        if (focusTarget === false) return;
        // The invoking row button survives a cancel; after a save its row was rebuilt, so the
        // replacement is found by group key instead.
        const usable = (element) => element && document.contains(element) && !element.disabled;
        const target = (usable(focusTarget) && focusTarget)
            || (usable(invoker) && invoker)
            || referenceActionFor(key)
            || byId('reference-add');
        if (usable(target)) target.focus();
    }

    async function loadReferenceGroups(message, good) {
        referenceState.loading = true;
        referenceState.failed = false;
        setReferenceStatus(message || 'Loading reference groups...', false, good);
        renderReferenceTables();
        try {
            const data = await parseResponse(await fetch(
                `${serverBaseUrl}/ui/api/chim_npc_manager.php?operation=reference_groups`,
                { cache: 'no-store' }
            ));
            referenceState.defaults = (Array.isArray(data && data.defaults) ? data.defaults : [])
                .map((row) => referenceRow(row, true));
            referenceState.custom = (Array.isArray(data && data.custom) ? data.custom : [])
                .map((row) => referenceRow(row, false));
            referenceState.loading = false;
            renderReferenceTables();
            setReferenceStatus(message || 'Changes apply the next time these actors register.', false, good);
        } catch (error) {
            referenceState.loading = false;
            referenceState.failed = true;
            referenceState.defaults = [];
            referenceState.custom = [];
            renderReferenceTables();
            setReferenceStatus(`Could not load reference groups: ${error.message || error}`, true);
        }
    }

    async function saveReferenceGroup(event) {
        event.preventDefault();
        if (referenceState.saving || !referenceState.editing) return;
        const name = byId('reference-display-name').value.trim();
        const plugin = byId('reference-plugin-name').value.trim();
        const parsed = readFormidField(byId('reference-formids').value);
        if (!name) {
            setReferenceEditorStatus('Enter a name for this character or group.', true);
            byId('reference-display-name').focus();
            return;
        }
        if (!plugin) {
            setReferenceEditorStatus('Enter the plugin file the references come from.', true);
            byId('reference-plugin-name').focus();
            return;
        }
        if (!/^[^\\/:*?"<>|\x00-\x1F]+\.es[mpl]$/i.test(plugin)) {
            setReferenceEditorStatus('Enter a plugin filename ending in .esp, .esm or .esl.', true);
            byId('reference-plugin-name').focus();
            return;
        }
        if (parsed.invalid.length) {
            setReferenceEditorStatus(`Not valid FormIDs: ${parsed.invalid.join(', ')}`, true);
            byId('reference-formids').focus();
            return;
        }
        if (parsed.ids.length < 2) {
            setReferenceEditorStatus('Add at least two reference FormIDs.', true);
            byId('reference-formids').focus();
            return;
        }
        if (parsed.ids.length > 32) {
            setReferenceEditorStatus('A group can contain up to 32 reference FormIDs.', true);
            byId('reference-formids').focus();
            return;
        }

        const key = referenceState.editing.key;
        const added = !key;
        setReferenceBusy(true);
        setReferenceEditorStatus('Saving...', false);
        try {
            await parseResponse(await fetch(`${serverBaseUrl}/ui/api/chim_npc_manager.php`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    operation: 'reference_group_save',
                    group_key: key,
                    display_name: name,
                    plugin_name: plugin,
                    local_formids: parsed.ids,
                    enabled: byId('reference-enabled').checked
                })
            }));
            // The tables are refreshed before the editor closes so focus lands on the rebuilt row
            // button rather than moving twice.
            await loadReferenceGroups(added ? 'Group added.' : 'Group saved.', true);
            setReferenceBusy(false);
            closeReferenceEditor();
        } catch (error) {
            setReferenceBusy(false);
            setReferenceEditorStatus(`Save failed: ${error.message || error}`, true);
        }
    }

    async function deleteReferenceGroup(group, isOverride) {
        if (referenceState.saving) return;
        const label = group.name || 'this group';
        const question = isOverride
            ? `Reset "${label}" to the built-in group? Your version is removed.`
            : `Delete the group "${label}"?`;
        if (!window.confirm(question)) return;
        setReferenceBusy(true);
        setReferenceStatus(isOverride ? 'Resetting group...' : 'Deleting group...', false);
        try {
            await parseResponse(await fetch(`${serverBaseUrl}/ui/api/chim_npc_manager.php`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ operation: 'reference_group_delete', group_key: group.key })
            }));
            const wasEditing = !!(referenceState.editing && referenceState.editing.key === group.key);
            await loadReferenceGroups(isOverride ? 'Built-in group restored.' : 'Group deleted.', true);
            setReferenceBusy(false);
            if (wasEditing) {
                closeReferenceEditor();
                return;
            }
            // Resetting an override leaves the built-in row's Customize button under the same key.
            const target = referenceActionFor(group.key) || byId('reference-add');
            if (target && document.contains(target) && !target.disabled) target.focus();
        } catch (error) {
            setReferenceBusy(false);
            setReferenceStatus(`${isOverride ? 'Reset' : 'Delete'} failed: ${error.message || error}`, true);
        }
    }

    function openReferenceModal(invoker) {
        const backdrop = byId('reference-backdrop');
        if (!backdrop || !backdrop.classList.contains('hidden')) return;
        referenceState.returnFocus = invoker || byId('reference-groups-button');
        backdrop.classList.remove('hidden');
        setReferenceBusy(false);
        byId('reference-close').focus();
        loadReferenceGroups();
    }

    function closeReferenceModal() {
        const backdrop = byId('reference-backdrop');
        if (!backdrop || backdrop.classList.contains('hidden')) return;
        closeReferenceEditor(false);
        backdrop.classList.add('hidden');
        const target = referenceState.returnFocus;
        referenceState.returnFocus = null;
        if (target && document.contains(target)) target.focus();
        sendCommand('input_capture|off');
    }

    // The NPC list behind the modal stays in the tab order otherwise, which strands keyboard users.
    function trapReferenceFocus(event) {
        if (event.key !== 'Tab') return;
        const modal = byId('reference-modal');
        if (!modal) return;
        const focusable = Array.from(modal.querySelectorAll(
            'button:not([disabled]), input:not([disabled]), textarea:not([disabled]), select:not([disabled])'
        )).filter((element) => !element.closest('[hidden]'));
        if (!focusable.length) return;
        const first = focusable[0];
        const last = focusable[focusable.length - 1];
        if (event.shiftKey && document.activeElement === first) {
            event.preventDefault();
            last.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
            event.preventDefault();
            first.focus();
        }
    }

    function ensureReferenceGroupsUi() {
        // The settings view renders this NPC page from its own markup, so the trigger is adopted
        // when it is already there and created next to Refresh when it is not.
        let trigger = byId('reference-groups-button');
        if (!trigger) {
            const refresh = byId('refresh-button');
            if (!refresh || !refresh.parentElement) return;
            trigger = document.createElement('button');
            trigger.id = 'reference-groups-button';
            trigger.type = 'button';
            trigger.className = 'button secondary compact';
            trigger.setAttribute('aria-haspopup', 'dialog');
            trigger.textContent = 'Reference Groups';
            refresh.insertAdjacentElement('afterend', trigger);
        }

        if (!byId('reference-backdrop')) {
            const backdrop = document.createElement('div');
            backdrop.id = 'reference-backdrop';
            backdrop.className = 'editor-backdrop reference-backdrop hidden';
            backdrop.setAttribute('role', 'presentation');
            backdrop.innerHTML = REFERENCE_MODAL_MARKUP;
            document.body.appendChild(backdrop);
            byId('reference-modal').addEventListener('keydown', trapReferenceFocus);
            byId('reference-close').addEventListener('click', closeReferenceModal);
            byId('reference-done').addEventListener('click', closeReferenceModal);
            byId('reference-refresh').addEventListener('click', () => loadReferenceGroups());
            byId('reference-add').addEventListener('click', () => openReferenceEditor('add', null));
            byId('reference-cancel').addEventListener('click', () => closeReferenceEditor());
            byId('reference-editor').addEventListener('submit', saveReferenceGroup);
        }

        trigger.addEventListener('click', (event) => openReferenceModal(event.currentTarget));
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
    ensureReferenceGroupsUi();
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
        // Reference groups sit above the NPC editor: the inline group editor closes first, then
        // the modal, and only then does Escape reach the page behind it.
        if (referenceModalOpen()) {
            event.stopImmediatePropagation();
            if (referenceEditorOpen()) closeReferenceEditor();
            else closeReferenceModal();
        } else if (!byId('editor-backdrop').classList.contains('hidden')) {
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
