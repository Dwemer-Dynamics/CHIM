(function () {
    'use strict';

    const byId = (id) => document.getElementById(id);
    const form = byId('npc-form');
    const featureDefinitions = [
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
    let listedCards = [];
    let stopSchedules = null;
    let editorLoadGeneration = 0;
    let loadingGeneration = 0;
    let searchTimer = null;
    let historyRecipientSearchTimer = null;
    let historySearchGeneration = 0;
    let historyEventType = '';
    const historyRecipients = new Map();
    let bglStatus = null;
    let bglRequestGeneration = 0;
    const VOICE_FILTER_NONE_ID = 'none';
    let voiceFilterPresets = [];
    let voiceFilterPreviewAudio = null;
    let voiceFilterPreviewCache = null;
    let voiceFilterPreviewGeneration = 0;
    const embeddedInSettings = !!byId('npcs-page');

    // Same-named actors are told apart by RefID and source mod, never by the visible name.
    function normalizeRefid(value) {
        const raw = String(value == null ? '' : value).trim().replace(/^0x/i, '').toUpperCase();
        return /^[0-9A-F]{1,8}$/.test(raw) ? raw.padStart(8, '0') : '';
    }

    // Canonical actor keys are opaque server-issued values. The UI only checks their shape and
    // never builds one from a name, a runtime FormID or a list position.
    // Same shape as the server's chimIsActorKey(): a ref: plugin is lowercase, ends in .esm/.esp/.esl, has no
    // | / \ @ # : control or DEL character and does not start with a space; the local id is 00XXXXXX.
    const ACTOR_KEY_PATTERN = /^(?:ref:(?! )[^A-Z|/\\@#:\x00-\x1F\x7F]+\.(?:esm|esp|esl)\|00[0-9A-F]{6}|dyn:(?!0{8}-0{4}-0{4}-0{4}-0{12}$)[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}|player|narrator)$/;
    function actorKey(value) {
        const key = String(value == null ? '' : value);
        return ACTOR_KEY_PATTERN.test(key) ? key : '';
    }

    // A readable identity for a key without exposing the raw "plugin|local" pair.
    function actorKeyLabel(key) {
        if (key === 'player') return 'Player';
        if (key === 'narrator') return 'Narrator';
        const ref = /^ref:([^|]+)\|([0-9A-F]{8})$/.exec(key || '');
        if (ref) return `${ref[2]} · ${ref[1]}`;
        if (/^dyn:/.test(key || '')) return `Dynamic actor ${key.slice(4, 12)}`;
        return '';
    }

    // Event participants arrive as legacy name strings or {name, id} objects (identity format 2).
    function participantLabel(entry) {
        if (entry && typeof entry === 'object') {
            const name = String(entry.name || entry.label || '').trim() || 'Unknown';
            const key = actorKey(entry.id || entry.key);
            return key && key !== 'player' && key !== 'narrator' ? `${name} (${actorKeyLabel(key)})` : name;
        }
        return String(entry == null ? '' : entry).trim();
    }

    function cardActorKey(card) {
        return actorKey(card && card.actor_key);
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
        const explicitKey = card.actor_key != null && String(card.actor_key) !== '';
        const key = cardActorKey(card);
        return { id: Number(byId('npc-id').value || 0), refid: normalizeRefid(card.refid), actorKey: key,
            invalidKey: explicitKey && !key };
    }

    // Additive guard for writes: the server refuses when the row id no longer holds this key.
    // Legacy rows have no key and send nothing, so ordinary edits keep working. A row whose key is
    // present but malformed is refused here rather than written unguarded or under a rewritten key.
    function withExpectedKey(payload, actor) {
        if (actor && actor.invalidKey) throw new Error('this NPC has an invalid actor key; reload the list');
        if (actor && actor.actorKey) payload.expected_actor_key = actor.actorKey;
        return payload;
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
            listedCards = Array.isArray(data.npcs) ? data.npcs : [];
            renderCards(listedCards);
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
        if (stopSchedules) stopSchedules();
        const editorGeneration = ++editorLoadGeneration;
        byId('editor-backdrop').classList.remove('hidden');
        byId('editor-title').textContent = 'Loading NPC...';
        resetVoiceFilterPreview();
        byId('save-status').textContent = '';
        try {
            const loadedDetail = await parseResponse(await fetch(
                `${serverBaseUrl}/ui/api/chim_npc_manager.php?operation=detail&id=${encodeURIComponent(id)}`,
                { cache: 'no-store' }
            ));
            if (editorGeneration !== editorLoadGeneration) return;
            currentDetail = loadedDetail;
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
        renderVoiceFilterPresets(detail);
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
        loadBglSettings(detail.card);
        resetNpcHistory(detail.card);
        if (stopSchedules) stopSchedules();
        stopSchedules = window.chimSchedules(byId('npc-schedules'), `${serverBaseUrl}/ui/api/npc_schedules.php`, detail.card.id, cardActorKey(detail.card));
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
        byId('sharing-explainer').textContent = `${automaticLine}${lands}`;

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

    function recipientEntry(card) {
        const refid = refidDisplay(card && card.refid);
        return {
            name: String((card && card.name) || 'NPC'),
            refid: refid.text,
            actorKey: cardActorKey(card),
            source: definingMod(card) || UNKNOWN_SOURCE_LABEL
        };
    }

    function renderBglSettings(status) {
        bglStatus = status || { background_life_enabled: false };
        const enabled = bglStatus.background_life_enabled === true;
        const enrollment = byId('bgl-enrollment-action');
        enrollment.disabled = false;
        enrollment.textContent = enabled ? 'Disable Background Life' : 'Enable Background Life';
        document.querySelectorAll('[data-bgl-setting]').forEach((control) => {
            control.checked = bglStatus[control.dataset.bglSetting] === true;
            control.disabled = !enabled;
        });
    }

    async function loadBglSettings(card) {
        const generation = ++bglRequestGeneration;
        bglStatus = null;
        document.querySelectorAll('[data-bgl-setting]').forEach((control) => {
            control.checked = false;
            control.disabled = true;
        });
        byId('bgl-enrollment-action').disabled = true;
        byId('bgl-enrollment-action').textContent = 'Loading...';
        try {
            const params = new URLSearchParams({ npc_name: card.name || '', refid: card.refid || '' });
            const status = await parseResponse(await fetch(
                `${serverBaseUrl}/ui/api/background_life_npc.php?${params.toString()}`,
                { cache: 'no-store' }
            ));
            if (generation !== bglRequestGeneration || currentDetail?.card !== card) return;
            renderBglSettings(status);
        } catch (error) {
            if (generation !== bglRequestGeneration || currentDetail?.card !== card) return;
            bglStatus = null;
            byId('bgl-enrollment-action').disabled = true;
            byId('bgl-enrollment-action').textContent = 'Unavailable';
            byId('bgl-action-status').textContent = `Could not load Background Life settings: ${error.message || error}`;
            byId('bgl-action-status').classList.add('error');
        }
    }

    async function updateBglSettings(operation, setting, value) {
        if (!currentDetail) return;
        const card = currentDetail.card;
        const generation = ++bglRequestGeneration;
        byId('bgl-enrollment-action').disabled = true;
        document.querySelectorAll('[data-bgl-setting]').forEach((control) => { control.disabled = true; });
        const body = new URLSearchParams({
            operation,
            npc_name: card.name || '',
            refid: card.refid || ''
        });
        if (setting) body.set('setting', setting);
        if (value !== undefined) body.set('value', value ? '1' : '0');
        const statusLine = byId('bgl-action-status');
        statusLine.textContent = 'Saving Background Life settings...';
        statusLine.classList.remove('error');
        try {
            const status = await parseResponse(await fetch(`${serverBaseUrl}/ui/api/background_life_npc.php`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8' },
                body: body.toString()
            }));
            if (generation !== bglRequestGeneration || currentDetail?.card !== card) return;
            renderBglSettings(status);
            statusLine.textContent = 'Background Life settings saved.';
        } catch (error) {
            if (generation !== bglRequestGeneration || currentDetail?.card !== card) return;
            statusLine.textContent = `Could not save Background Life settings: ${error.message || error}`;
            statusLine.classList.add('error');
            if (bglStatus) renderBglSettings(bglStatus);
        }
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

    function historyRecipientKeys() {
        const keys = {};
        historyRecipients.forEach((entry, id) => {
            if (entry && entry.actorKey) keys[String(id)] = entry.actorKey;
        });
        return keys;
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
                Array.isArray(historyEvent.recipients)
                    ? historyEvent.recipients.map(participantLabel).filter(Boolean).join(', ')
                    : '',
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
                        body: JSON.stringify(withExpectedKey({
                            operation: 'delete_event',
                            id: Number(byId('npc-id').value),
                            rowid: Number(historyEvent.rowid)
                        }, selectedActor()))
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
                body: JSON.stringify(withExpectedKey({
                    operation: 'inject_event',
                    id: actor.id,
                    refid: actor.refid,
                    event: text,
                    recipient_ids: Array.from(historyRecipients.keys()),
                    // Row id -> key the operator saw; keyless legacy recipients are omitted, never guessed.
                    recipient_expected_keys: historyRecipientKeys()
                }, actor))
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

    // Classifies a stored edge without guessing: only a canonical actor key or an explicit typed
    // target is an actor. Name-keyed edges stay unattributed legacy data, shown but never retargeted.
    function relationshipTarget(mapKey, relationship) {
        const typed = relationship && relationship.target && typeof relationship.target === 'object'
            ? relationship.target : null;
        const typedKind = String(typed && typed.kind || '').toLowerCase();
        const key = actorKey(typed && typed.key) || actorKey(mapKey);
        // Relationship storage keys the player edge as "Player" (RelationshipManager::playerTargetIdentity);
        // this is separate from the reserved event/actor key "player".
        if (typedKind === 'player' || mapKey === 'Player') {
            return { kind: 'player', key: 'Player', label: 'Player', editable: true };
        }
        if ((typedKind === 'actor' || !typedKind) && key && key !== 'player' && key !== 'narrator') {
            const label = String(typed && typed.label || '').trim() || 'Unnamed actor';
            return { kind: 'actor', key, label, editable: true };
        }
        if (typedKind === 'concept') {
            return { kind: 'concept', key: String(typed.key || mapKey), label: String(typed.label || mapKey), editable: true };
        }
        return { kind: 'legacy', key: '', label: String(mapKey), editable: false };
    }

    // A keyed NPC search result becomes an actor choice; cards without a server key are not offered.
    function relationshipActorChoice(card) {
        const key = cardActorKey(card);
        const selfId = Number(byId('npc-id').value || 0);
        if (!key || key === 'player' || key === 'narrator' || Number(card.id) === selfId) return null;
        const entry = recipientEntry(card);
        return { kind: 'actor', key, name: entry.name, npcId: Number(card.id), identity: `${entry.refid} · ${entry.source}` };
    }

    function relationshipTargetElement(row, target, isNew) {
        if (!isNew) {
            const display = document.createElement('div');
            display.className = `relationship-target relationship-target-fixed ${target.kind}`;
            const name = document.createElement('span');
            name.className = 'relationship-target-name';
            name.textContent = target.label;
            display.appendChild(name);
            const detail = document.createElement('span');
            detail.className = 'relationship-target-identity';
            detail.textContent = target.kind === 'actor' ? actorKeyLabel(target.key)
                : target.kind === 'legacy' ? 'Unattributed legacy entry (read-only)'
                : target.kind === 'concept' ? 'Concept' : '';
            if (detail.textContent) display.appendChild(detail);
            return display;
        }
        // The target type is always explicit: an NPC is picked by exact key from the server search
        // (any page or filter), and a concept/faction is typed text that is never resolved to an actor.
        const picker = document.createElement('div');
        picker.className = 'relationship-target-picker';
        const kind = document.createElement('select');
        kind.className = 'relationship-target';
        kind.setAttribute('aria-label', 'Relationship target type');
        [['', 'Choose target type'], ['player', 'Player'], ['actor', 'NPC (search)'], ['concept', 'Concept / faction']]
            .forEach(([value, text]) => kind.appendChild(new Option(text, value)));
        const search = document.createElement('input');
        search.type = 'search';
        search.className = 'relationship-target-search';
        search.placeholder = 'Search NPC profiles';
        search.setAttribute('aria-label', 'Search NPC for relationship');
        const concept = document.createElement('input');
        concept.className = 'relationship-target-concept';
        concept.placeholder = 'Concept or faction name';
        concept.setAttribute('aria-label', 'Concept or faction name');
        const results = document.createElement('div');
        results.className = 'history-search-results relationship-target-results';
        const chosen = document.createElement('div');
        chosen.className = 'relationship-target-identity relationship-target-chosen';
        chosen.setAttribute('aria-live', 'polite');
        let generation = 0;
        let timer = null;
        const sync = () => {
            search.hidden = kind.value !== 'actor';
            concept.hidden = kind.value !== 'concept';
            results.hidden = true;
            if (kind.value === 'player') row.targetChoice = { kind: 'player', key: 'Player', name: 'Player' };
            else if (kind.value === 'concept') {
                const label = concept.value.trim();
                row.targetChoice = label ? { kind: 'concept', key: label, name: label } : null;
            } else row.targetChoice = null;
            chosen.textContent = '';
        };
        const pick = (choice) => {
            generation += 1;
            row.targetChoice = choice;
            search.value = choice.name;
            chosen.textContent = `Selected: ${choice.name} · ${choice.identity}`;
            results.hidden = true;
            search.focus();
        };
        const runSearch = async () => {
            const term = search.value.trim();
            const current = ++generation;
            if (term.length < 2) { results.hidden = true; return; }
            const query = new URLSearchParams({ operation: 'list', search: term, page: '1', limit: '10' });
            try {
                const data = await parseResponse(await fetch(
                    `${serverBaseUrl}/ui/api/chim_npc_manager.php?${query.toString()}`, { cache: 'no-store' }));
                if (current !== generation) return;
                const choices = (Array.isArray(data.npcs) ? data.npcs : []).map(relationshipActorChoice).filter(Boolean);
                results.replaceChildren();
                if (!choices.length) {
                    const empty = document.createElement('div');
                    empty.className = 'history-search-result-identity';
                    empty.textContent = 'No keyed NPC matches.';
                    results.appendChild(empty);
                }
                choices.forEach((choice) => {
                    const button = document.createElement('button');
                    button.type = 'button';
                    button.className = 'history-search-result';
                    const name = document.createElement('span');
                    name.className = 'history-search-result-name';
                    name.textContent = choice.name;
                    const identity = document.createElement('span');
                    identity.className = 'history-search-result-identity';
                    identity.textContent = choice.identity;
                    button.append(name, identity);
                    button.addEventListener('click', () => pick(choice));
                    results.appendChild(button);
                });
                results.hidden = false;
            } catch (_error) {
                if (current === generation) results.hidden = true;
            }
        };
        kind.addEventListener('change', () => {
            sync();
            if (kind.value === 'actor') search.focus();
            if (kind.value === 'concept') concept.focus();
        });
        concept.addEventListener('input', sync);
        search.addEventListener('input', () => {
            row.targetChoice = null;
            chosen.textContent = '';
            generation += 1;
            clearTimeout(timer);
            timer = setTimeout(runSearch, 250);
        });
        search.addEventListener('keydown', (event) => {
            const first = results.hidden ? null : results.querySelector('button');
            if (event.key === 'ArrowDown' && first) { event.preventDefault(); first.focus(); }
            // Escape first closes the open result list; only a second Escape reaches the editor.
            if (event.key === 'Escape' && !results.hidden) { event.stopPropagation(); results.hidden = true; }
        });
        results.addEventListener('keydown', (event) => {
            const buttons = Array.from(results.querySelectorAll('button'));
            const index = buttons.indexOf(document.activeElement);
            if (event.key === 'ArrowDown' && index < buttons.length - 1) { event.preventDefault(); buttons[index + 1].focus(); }
            if (event.key === 'ArrowUp') { event.preventDefault(); (index > 0 ? buttons[index - 1] : search).focus(); }
            if (event.key === 'Escape') { event.stopPropagation(); results.hidden = true; search.focus(); }
        });
        picker.append(kind, search, concept, results, chosen);
        sync();
        return picker;
    }

    function addRelationshipRow(mapKey, relationship, isNew) {
        const row = document.createElement('div');
        row.className = 'relationship-row';
        row.relationshipData = relationship && typeof relationship === 'object' ? { ...relationship } : {};
        row.mapKey = isNew ? '' : String(mapKey);
        row.isNew = !!isNew;
        const target = isNew ? { kind: 'new', label: '', editable: true } : relationshipTarget(row.mapKey, row.relationshipData);
        row.targetInfo = target;
        if (target.kind === 'legacy') row.classList.add('legacy');
        const targetElement = relationshipTargetElement(row, target, isNew);
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
        const label = target.label || 'new relationship';
        affinity.setAttribute('aria-label', `Affinity with ${label}`);
        type.setAttribute('aria-label', `Relationship type with ${label}`);
        note.setAttribute('aria-label', `Relationship note for ${label}`);
        // Legacy edges are preserved exactly as stored until the server offers explicit attribution.
        if (!target.editable) [affinity, type, note, customInfo].forEach((control) => { control.disabled = true; });
        const remove = document.createElement('button');
        remove.type = 'button';
        remove.className = 'remove-relationship';
        remove.textContent = '×';
        remove.title = 'Remove relationship';
        remove.setAttribute('aria-label', `Remove relationship with ${label}`);
        remove.addEventListener('click', () => row.remove());
        row.append(targetElement, affinity, type, note, remove, customInfoField);
        byId('relationship-list').appendChild(row);
        return row;
    }

    function renderRelationships(relationships) {
        byId('relationship-list').replaceChildren();
        Object.entries(relationships).forEach(([target, relationship]) => addRelationshipRow(target, relationship, false));
    }

    // Existing edges keep their stored map key and typed target; new edges use the chosen key.
    // Throws instead of saving when a new row has no target or collides with an existing edge.
    function collectRelationships() {
        const relationships = {};
        byId('relationship-list').querySelectorAll('.relationship-row').forEach((row) => {
            const info = row.targetInfo || {};
            if (!row.isNew && !info.editable) {
                relationships[row.mapKey] = { ...(row.relationshipData || {}) };
                return;
            }
            let mapKey = row.mapKey;
            const data = { ...(row.relationshipData || {}) };
            const choice = row.targetChoice;
            if (row.isNew) {
                if (!choice) throw new Error('Choose a target for each new relationship.');
                if (choice.kind === 'concept' && (actorKey(choice.key) || choice.key.toLowerCase() === 'player')) {
                    throw new Error(`"${choice.key}" is reserved and cannot be used as a concept name.`);
                }
                mapKey = choice.kind === 'player' ? 'Player' : choice.key;
                data.target = { kind: choice.kind, key: choice.key, label: choice.name };
                if (choice.npcId) data.target_npc_id = choice.npcId;
            }
            if (Object.prototype.hasOwnProperty.call(relationships, mapKey)) {
                throw new Error(`A relationship with ${info.label || (choice && choice.name) || mapKey} already exists.`);
            }
            relationships[mapKey] = {
                ...data,
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
        const actor = selectedActor();
        const saveButton = byId('save-button');
        saveButton.disabled = true;
        byId('save-status').textContent = 'Saving NPC profile...';
        byId('save-status').classList.remove('error');
        try {
            currentDetail = await parseResponse(await fetch(`${serverBaseUrl}/ui/api/chim_npc_manager.php`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(withExpectedKey({
                    id: actor.id,
                    refid: actor.refid,
                    profile_revision: String((currentDetail && currentDetail.profile_revision) || ''),
                    fields: collectFields(),
                    overrides: collectOverrides(),
                    relationships: collectRelationships(),
                    relationships_locked: byId('relationships-locked').checked
                }, actor))
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
                body: JSON.stringify(withExpectedKey({
                    operation: 'action',
                    action,
                    id: actor.id,
                    refid: actor.refid,
                    idea
                }, actor))
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
        ++editorLoadGeneration;
        if (stopSchedules) stopSchedules();
        byId('editor-backdrop').classList.add('hidden');
        resetVoiceFilterPreview();
        currentDetail = null;
        sendCommand('input_capture|off');
    }

    function switchEditorTab(tabName) {
        if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
        sendCommand('input_capture|off');
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
            <p id="reference-intro" class="reference-intro">A group lists the exact placed references that are the same character, so they share one profile. Leave FormIDs empty to link every placed actor with that exact name from every plugin, sharing one profile, memories and relationships. Changing or disabling a rule releases its automatic links. Matching actors share again when they next register. Manual links and original profile data are preserved.</p>
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
                    <span id="reference-plugin-label">Plugin file</span>
                    <input id="reference-plugin-name" type="text" maxlength="120" spellcheck="false" placeholder="Skyrim.esm"
                           aria-describedby="reference-formids-hint">
                </label>
            </div>
            <label class="form-field">
                <span>Reference FormIDs</span>
                <textarea id="reference-formids" class="reference-formids" rows="3" spellcheck="false"
                          aria-describedby="reference-formids-hint" placeholder="0001A66C"></textarea>
            </label>
            <p id="reference-formids-hint" class="reference-hint" aria-live="polite">Local FormIDs from the plugin above, without the load order prefix. One per line, or separated by spaces or commas.</p>
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

    const REFERENCE_EXACT_HINT = 'Local FormIDs from the plugin above, without the load order prefix. One per line, or separated by spaces or commas.';
    const REFERENCE_ALL_HINT = 'No FormIDs: every placed actor with this exact name, from every plugin, shares one profile, memories and relationships. The plugin file is not used.';

    // A catch-all row matches by name alone, which the server marks with catch_all and plugin '*'.
    function referenceRow(row, isDefault) {
        const source = row && typeof row === 'object' ? row : {};
        const plugin = String(source.plugin_name == null ? '' : source.plugin_name).trim();
        const catchAll = toBoolean(source.catch_all, false) || plugin === '*';
        return {
            key: String(source.group_key == null ? '' : source.group_key).trim(),
            name: String(source.display_name == null ? '' : source.display_name).trim(),
            plugin: catchAll ? '' : plugin,
            catchAll,
            formids: catchAll ? [] : normalizeFormidList(source.local_formids),
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

    function referencePluginCell(group) {
        return group.catchAll
            ? referenceTextCell('Plugin', 'All plugins', 'reference-plugin reference-scope-all')
            : referenceTextCell('Plugin', group.plugin || 'Unknown plugin', 'reference-plugin');
    }

    function referenceFormidCell(group) {
        if (group.catchAll) {
            return referenceTextCell('Reference FormIDs', 'All references with this name', 'reference-formid-list reference-scope-all');
        }
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
                referencePluginCell(group),
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
                referencePluginCell(group),
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
            note.textContent = 'List every placed reference that is the same character, or leave FormIDs empty to match the name in all plugins.';
            note.hidden = false;
        } else {
            note.textContent = '';
            note.hidden = true;
        }
        byId('reference-display-name').value = source.name || '';
        const pluginField = byId('reference-plugin-name');
        pluginField.disabled = false;
        pluginField.dataset.retained = '';
        pluginField.value = source.plugin || '';
        byId('reference-formids').value = (source.formids || []).join('\n');
        byId('reference-enabled').checked = source.enabled !== false;
        updateReferenceScope();
        setReferenceEditorStatus('', false);
        editor.hidden = false;
        byId('reference-add').setAttribute('aria-expanded', 'true');
        if (typeof editor.scrollIntoView === 'function') editor.scrollIntoView({ block: 'nearest' });
        const first = byId('reference-display-name');
        first.focus();
        if (typeof first.select === 'function') first.select();
    }

    // An empty FormID list is the all-plugins choice on its own, so the plugin field is set aside
    // (its value kept for when FormIDs return) and the hint and save label follow as the operator types.
    function updateReferenceScope() {
        const parsed = readFormidField(byId('reference-formids').value);
        const empty = !parsed.ids.length && !parsed.invalid.length;
        const plugin = byId('reference-plugin-name');
        if (empty && !plugin.disabled) {
            plugin.dataset.retained = plugin.value;
            plugin.value = '';
            plugin.disabled = true;
        } else if (!empty && plugin.disabled) {
            plugin.disabled = false;
            plugin.value = plugin.dataset.retained || '';
            plugin.dataset.retained = '';
        }
        const hint = byId('reference-formids-hint');
        const text = empty ? REFERENCE_ALL_HINT : REFERENCE_EXACT_HINT;
        // Rewriting identical text would re-announce the live hint on every keystroke.
        if (hint.textContent !== text) hint.textContent = text;
        hint.classList.toggle('reference-scope-all', empty);
        byId('reference-plugin-label').textContent = empty ? 'Plugin file (not used: all plugins)' : 'Plugin file';
        plugin.placeholder = empty ? 'All plugins' : 'Skyrim.esm';
        byId('reference-save').textContent = empty ? 'Save for all plugins' : 'Save group';
    }

    // The server refuses a second, name-only group beside an enabled exact one, so the operator
    // is pointed at the group to edit instead of a failed save.
    function referenceNameConflict(name, key) {
        const effective = referenceState.defaults
            .filter((group) => !customGroupFor(group.key))
            .map((group) => ({ group, builtin: true }))
            .concat(referenceState.custom.map((group) => ({ group, builtin: false })));
        return effective.find(({ group }) => group.enabled && !group.catchAll
            && group.key !== key && group.name === name) || null;
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

    // Exact groups name 2-32 references from one plugin; a name-only group skips these checks.
    function exactReferenceProblem(plugin, parsed) {
        if (!plugin) return { field: 'reference-plugin-name', message: 'Enter the plugin file the references come from.' };
        if (!/^[^\\/:*?"<>|\x00-\x1F]+\.es[mpl]$/i.test(plugin)) {
            return { field: 'reference-plugin-name', message: 'Enter a plugin filename ending in .esp, .esm or .esl.' };
        }
        if (parsed.invalid.length) return { field: 'reference-formids', message: `Not valid FormIDs: ${parsed.invalid.join(', ')}` };
        if (parsed.ids.length < 2) {
            return { field: 'reference-formids', message: 'Add at least two reference FormIDs, or remove them all to match the name in all plugins.' };
        }
        if (parsed.ids.length > 32) return { field: 'reference-formids', message: 'A group can contain up to 32 reference FormIDs.' };
        return null;
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
        const catchAll = !parsed.ids.length && !parsed.invalid.length;
        const conflict = catchAll ? referenceNameConflict(name, referenceState.editing.key) : null;
        if (conflict) {
            setReferenceEditorStatus(conflict.builtin
                ? `Customize the built-in "${name}" group to match it in all plugins.`
                : `Edit your existing "${name}" group to match it in all plugins.`, true);
            byId('reference-display-name').focus();
            return;
        }
        const problem = catchAll ? null : exactReferenceProblem(plugin, parsed);
        if (problem) {
            setReferenceEditorStatus(problem.message, true);
            byId(problem.field).focus();
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
                    plugin_name: catchAll ? '*' : plugin,
                    local_formids: catchAll ? [] : parsed.ids,
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
            byId('reference-formids').addEventListener('input', updateReferenceScope);
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
    byId('bgl-enrollment-action').addEventListener('click', () => {
        updateBglSettings(bglStatus && bglStatus.background_life_enabled ? 'disable' : 'enable');
    });
    document.querySelectorAll('[data-bgl-setting]').forEach((control) => {
        control.addEventListener('change', () => updateBglSettings('toggle', control.dataset.bglSetting, control.checked));
    });
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
    byId('add-relationship').addEventListener('click', () => {
        const row = addRelationshipRow('', { aff: 0, type: 'neutral' }, true);
        row.querySelector('.relationship-target').focus();
    });
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
