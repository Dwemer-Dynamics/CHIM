(function () {
    'use strict';

    const byId = (id) => document.getElementById(id);
    let serverBaseUrl = 'http://127.0.0.1:8081/HerikaServer';
    let globalData = null;
    let profileData = null;
    let selectedProfileId = 0;
    let activeGlobalTab = 'prompt-rechat';
    let selectSequence = 0;

    function command(value) { if (window.chimConfigManagerCommand) window.chimConfigManagerCommand(value); }
    function asBool(value) {
        if (typeof value === 'boolean') return value;
        return ['1', 'true', 'yes', 'on'].includes(String(value || '').trim().toLowerCase());
    }
    function normalizeBaseUrl(value) {
        let base = String(value || '').trim().replace(/\/+$/, '');
        if (!/\/HerikaServer$/i.test(base)) base += '/HerikaServer';
        return base;
    }
    function closeTileSelectors(except) {
        document.querySelectorAll('.settings-tile-selector.expanded').forEach((selector) => {
            if (selector === except) return;
            selector.classList.remove('expanded');
            selector.querySelector('.settings-tile-trigger').setAttribute('aria-expanded', 'false');
        });
    }
    function syncTileSelect(select) {
        const selector = select.closest('.settings-tile-selector');
        if (!selector) return;
        const selected = select.options[select.selectedIndex];
        selector.querySelector('.settings-tile-value').textContent = selected ? selected.textContent : 'Not assigned';
        selector.querySelector('.settings-tile-trigger').disabled = select.disabled;
        selector.querySelectorAll('.settings-tile-option').forEach((option) => {
            const active = option.dataset.value === select.value;
            option.classList.toggle('selected', active);
            option.setAttribute('aria-selected', active ? 'true' : 'false');
        });
    }
    function enhanceSelect(select) {
        if (select.closest('.settings-tile-selector')) return select.closest('.settings-tile-selector');
        const selector = document.createElement('div');
        const trigger = document.createElement('button');
        const value = document.createElement('span');
        const arrow = document.createElement('span');
        const options = document.createElement('div');
        const listId = `settings-select-options-${++selectSequence}`;
        selector.className = 'settings-tile-selector';
        trigger.type = 'button'; trigger.className = 'settings-tile-trigger';
        trigger.setAttribute('aria-haspopup', 'listbox'); trigger.setAttribute('aria-expanded', 'false'); trigger.setAttribute('aria-controls', listId);
        value.className = 'settings-tile-value'; arrow.className = 'settings-tile-arrow'; arrow.setAttribute('aria-hidden', 'true');
        options.className = 'settings-tile-options'; options.id = listId; options.setAttribute('role', 'listbox');
        Array.from(select.options).forEach((nativeOption) => {
            const option = document.createElement('button');
            option.type = 'button'; option.className = 'settings-tile-option'; option.dataset.value = nativeOption.value;
            option.setAttribute('role', 'option'); option.textContent = nativeOption.textContent;
            option.addEventListener('click', () => {
                select.value = nativeOption.value;
                select.dispatchEvent(new Event('change', { bubbles: true }));
                syncTileSelect(select); closeTileSelectors(); trigger.focus();
            });
            options.appendChild(option);
        });
        trigger.append(value, arrow);
        trigger.addEventListener('click', () => {
            const opening = !selector.classList.contains('expanded');
            closeTileSelectors(opening ? selector : null);
            selector.classList.toggle('expanded', opening);
            trigger.setAttribute('aria-expanded', opening ? 'true' : 'false');
        });
        selector.addEventListener('keydown', (event) => {
            if (event.key !== 'Escape') return;
            event.stopPropagation(); closeTileSelectors(); trigger.focus();
        });
        select.classList.add('settings-native-select');
        select.parentNode.insertBefore(selector, select);
        selector.append(trigger, options, select);
        select.addEventListener('change', () => syncTileSelect(select));
        syncTileSelect(select);
        return selector;
    }
    function setControlDisabled(control, disabled) {
        control.disabled = disabled;
        if (control.matches('select')) syncTileSelect(control);
    }
    async function responseData(response) {
        let payload;
        try { payload = await response.json(); } catch (_error) { throw new Error(`Invalid server response (HTTP ${response.status})`); }
        if (!response.ok || !payload || !payload.success) throw new Error((payload && payload.error) || `HTTP ${response.status}`);
        return payload.data || payload;
    }
    function status(message, error) {
        byId('status-line').textContent = message || '';
        byId('status-line').classList.toggle('error', !!error);
    }
    function fieldControl(field, value, namePrefix) {
        const wrapper = document.createElement('label');
        const type = String(field.type || 'string');
        const name = `${namePrefix || ''}${field.name}`;
        wrapper.className = type === 'boolean' ? 'toggle-field' : 'field';
        let control;
        if (type === 'boolean') {
            control = document.createElement('input'); control.type = 'checkbox'; control.checked = asBool(value); control.name = name;
            const label = document.createElement('span'); label.textContent = field.label || field.name; wrapper.append(control, label);
        } else {
            const label = document.createElement('span'); label.textContent = field.label || field.name; wrapper.appendChild(label);
            if (type === 'longstring') { control = document.createElement('textarea'); control.rows = 5; }
            else if (type === 'select' || type.startsWith('foreign:')) {
                control = document.createElement('select');
                const options = field.options || (field.values || []).map((item) => ({ value: item, label: item }));
                if (type.startsWith('foreign:')) control.appendChild(new Option('Not assigned', ''));
                options.forEach((option) => control.appendChild(new Option(option.label, String(option.value))));
            } else { control = document.createElement('input'); control.type = ['integer', 'number'].includes(type) ? 'number' : (type === 'url' ? 'url' : 'text'); }
            control.name = name; control.value = value === null || value === undefined ? '' : String(value);
            if (field.min !== undefined) control.min = field.min; if (field.max !== undefined) control.max = field.max; if (field.step !== undefined) control.step = field.step;
            wrapper.appendChild(control);
            if (control.matches('select')) enhanceSelect(control);
            if (field.description) { const help = document.createElement('small'); help.textContent = field.description; wrapper.appendChild(help); }
        }
        return wrapper;
    }

    async function loadGlobals() {
        status('Loading Global Settings...', false);
        globalData = await responseData(await fetch(`${serverBaseUrl}/ui/api/chim_global_settings.php`, { cache: 'no-store' }));
        renderGlobals(); status('Global Settings loaded.', false);
    }
    function renderGlobals() {
        const tabs = byId('global-tabs'); tabs.replaceChildren();
        Object.entries(globalData.tabs || {}).forEach(([id, label]) => {
            const button = document.createElement('button'); button.type = 'button'; button.className = `sub-tab${id === activeGlobalTab ? ' active' : ''}`; button.textContent = label;
            button.addEventListener('click', () => { activeGlobalTab = id; renderGlobals(); }); tabs.appendChild(button);
        });
        const sections = byId('global-sections'); sections.replaceChildren();
        (globalData.sections || []).forEach((section) => {
            const card = document.createElement('section'); card.className = 'settings-section'; card.dataset.hidden = section.tab !== activeGlobalTab ? 'true' : 'false';
            const title = document.createElement('h2'); title.textContent = section.name; card.appendChild(title);
            section.fields.forEach((field) => card.appendChild(fieldControl(field, field.value, 'global:')));
            sections.appendChild(card);
        });
        const context = byId('prompt-context-options'); context.replaceChildren();
        Object.entries(globalData.prompt_context_catalog || {}).forEach(([bucket, options]) => {
            const box = document.createElement('section'); box.className = 'context-bucket';
            const title = document.createElement('h3'); title.textContent = bucket.replaceAll('_', ' '); box.appendChild(title);
            Object.entries(options || {}).forEach(([id, definition]) => {
                const field = { name: `${bucket}:${id}`, label: definition.label || id, type: 'boolean' };
                const enabled = (globalData.prompt_context_options[bucket] || []).includes(id);
                box.appendChild(fieldControl(field, enabled, 'context:'));
            }); context.appendChild(box);
        });
        byId('prompt-context-options').closest('.settings-section').hidden = activeGlobalTab !== 'context-knowledge';
    }
    async function saveGlobals(event) {
        event.preventDefault();
        const settings = {}; const promptContext = {};
        byId('globals-form').querySelectorAll('[name]').forEach((control) => {
            if (control.name.startsWith('global:')) settings[control.name.slice(7)] = control.type === 'checkbox' ? control.checked : control.value;
            if (control.name.startsWith('context:')) {
                const [bucket, id] = control.name.slice(8).split(':');
                if (!promptContext[bucket]) promptContext[bucket] = [];
                if (control.checked) promptContext[bucket].push(id);
            }
        });
        status('Saving Global Settings...', false);
        await responseData(await fetch(`${serverBaseUrl}/ui/api/chim_global_settings.php`, { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ settings, prompt_context_options: promptContext }) }));
        await loadGlobals(); status('Global Settings saved.', false);
    }

    async function loadProfiles(preferredId) {
        status('Loading Profiles...', false);
        profileData = await responseData(await fetch(`${serverBaseUrl}/ui/api/chim_profile_manager.php`, { cache: 'no-store' }));
        renderProfileList();
        const id = Number(preferredId || selectedProfileId || (profileData.profiles[0] && profileData.profiles[0].id));
        if (id) await loadProfile(id); else status('No profiles found.', true);
    }
    function renderProfileList() {
        const list = byId('profile-list'); list.replaceChildren();
        const slots = byId('profile-slot-summary'); slots.replaceChildren();
        for (let slot = 1; slot <= 4; slot += 1) {
            const profile = (profileData.profiles || []).find((item) => Number(item.slot) === slot);
            const row = document.createElement('div'); row.className = 'slot-summary-row';
            const label = document.createElement('strong'); label.textContent = `Slot ${slot}`;
            const value = document.createElement('span'); value.textContent = profile ? profile.label : 'Empty';
            row.append(label, value); slots.appendChild(row);
        }
        (profileData.profiles || []).forEach((profile) => {
            const button = document.createElement('button'); button.type = 'button'; button.className = `profile-card${profile.id === selectedProfileId ? ' active' : ''}`;
            const titleRow = document.createElement('div'); titleRow.className = 'profile-card-title';
            const title = document.createElement('strong'); title.textContent = profile.label;
            const badges = document.createElement('span'); badges.className = 'profile-badges';
            if (profile.default_npc) { const badge = document.createElement('span'); badge.className = 'profile-badge'; badge.textContent = 'Default'; badges.appendChild(badge); }
            if (profile.slot) { const badge = document.createElement('span'); badge.className = 'profile-badge'; badge.textContent = `Slot ${profile.slot}`; badges.appendChild(badge); }
            const meta = document.createElement('small'); meta.textContent = `${profile.npc_count} NPCs${profile.dynamic_profile ? ' · Dynamic' : ''}${profile.auto_diary ? ' · Diary' : ''}`;
            titleRow.append(title, badges); button.append(titleRow, meta); button.addEventListener('click', () => loadProfile(profile.id)); list.appendChild(button);
        });
    }
    async function loadProfile(id) {
        selectedProfileId = Number(id); renderProfileList(); status('Loading profile...', false);
        const payload = await responseData(await fetch(`${serverBaseUrl}/ui/api/chim_profile_manager.php?id=${selectedProfileId}`, { cache: 'no-store' }));
        profileData.profiles = payload.profiles; profileData.connector_options = payload.connector_options;
        renderProfile(payload.detail); status('Profile loaded.', false);
    }
    function renderProfile(detail) {
        const form = byId('profile-form');
        form.elements.label.value = detail.core.label || ''; form.elements.slot.value = detail.core.slot || '';
        syncTileSelect(form.elements.slot);
        form.elements.default_npc.checked = asBool(detail.core.default_npc); form.elements.prompt.value = detail.core.prompt || '';
        byId('profile-editor-name').textContent = detail.core.label || 'Profile';
        const connectorRoot = byId('profile-connectors'); connectorRoot.replaceChildren();
        Object.entries(detail.connector_catalog || {}).forEach(([group, fields]) => {
            const section = document.createElement('section'); section.className = 'settings-section'; const title = document.createElement('h2'); title.textContent = group; section.appendChild(title);
            fields.forEach((field) => { field.type = 'select'; field.options = profileData.connector_options[field.source] || []; field.value = detail.connectors[field.name] || ''; section.appendChild(fieldControl(field, field.value, 'connector:')); });
            connectorRoot.appendChild(section);
        });
        const metadataRoot = byId('profile-metadata-sections'); metadataRoot.replaceChildren();
        (detail.metadata_sections || []).forEach((group) => {
            const section = document.createElement('section'); section.className = 'settings-section'; const title = document.createElement('h2'); title.textContent = group.name; section.appendChild(title);
            group.fields.forEach((field) => {
                if (field.type === 'multiselect') {
                    const block = document.createElement('div'); block.className = 'field'; const label = document.createElement('span'); label.textContent = field.label; block.appendChild(label);
                    (field.options || []).forEach((option) => block.appendChild(fieldControl({name:`${field.name}:${option}`,label:option,type:'boolean'}, (field.value || []).includes(option), 'metadata_multi:'))); section.appendChild(block);
                } else {
                    const control = fieldControl(field, field.value, 'metadata:');
                    if (field.copyable) { const button = document.createElement('button'); button.type='button'; button.className='button copy-button'; button.textContent='Copy to all'; button.addEventListener('click', () => copySetting(field.name)); control.prepend(button); }
                    section.appendChild(control);
                }
            }); metadataRoot.appendChild(section);
        });
        const overrideRoot = byId('profile-override-sections'); overrideRoot.replaceChildren();
        (detail.override_sections || []).forEach((group) => {
            const section = document.createElement('section'); section.className = 'settings-section';
            const title = document.createElement('h2'); title.textContent = group.name; section.appendChild(title);
            group.fields.forEach((field) => {
                const row = document.createElement('div'); row.className = `override-field${field.enabled ? '' : ' disabled'}`;
                const enable = document.createElement('label'); enable.className = 'override-enable';
                const checkbox = document.createElement('input'); checkbox.type = 'checkbox'; checkbox.name = `override_enabled:${field.name}`; checkbox.checked = asBool(field.enabled);
                const label = document.createElement('span'); label.textContent = field.label || field.name; enable.append(checkbox, label);
                const valueField = Object.assign({}, field, { label: 'Override Value' });
                const control = fieldControl(valueField, field.value, 'override:');
                control.querySelectorAll('input, textarea, select').forEach((element) => setControlDisabled(element, !checkbox.checked));
                checkbox.addEventListener('change', () => {
                    row.classList.toggle('disabled', !checkbox.checked);
                    control.querySelectorAll('input, textarea, select').forEach((element) => setControlDisabled(element, !checkbox.checked));
                });
                row.append(enable, control); section.appendChild(row);
            });
            overrideRoot.appendChild(section);
        });
        byId('profile-metadata-json').value = JSON.stringify(detail.metadata || {}, null, 2);
    }
    async function saveProfile(event) {
        event.preventDefault();
        let metadata; try { metadata = JSON.parse(byId('profile-metadata-json').value || '{}'); } catch (_error) { throw new Error('Advanced Metadata JSON is invalid.'); }
        const multi = {};
        byId('profile-form').querySelectorAll('[name^="metadata:"]').forEach((control) => { metadata[control.name.slice(9)] = control.type === 'checkbox' ? control.checked : (control.type === 'number' && control.value !== '' ? Number(control.value) : control.value); });
        byId('profile-form').querySelectorAll('[name^="metadata_multi:"]').forEach((control) => { const [key, value] = control.name.slice(15).split(':'); if (!multi[key]) multi[key]=[]; if (control.checked) multi[key].push(value); });
        Object.assign(metadata, multi);
        byId('profile-form').querySelectorAll('[name^="override_enabled:"]').forEach((enabledControl) => {
            const key = enabledControl.name.slice(17); delete metadata[key];
            if (!enabledControl.checked) return;
            const control = byId('profile-form').querySelector(`[name="override:${CSS.escape(key)}"]`);
            if (!control) return;
            metadata[key] = control.type === 'checkbox' ? control.checked : (control.type === 'number' && control.value !== '' ? Number(control.value) : control.value);
        });
        const connectors = {}; byId('profile-form').querySelectorAll('[name^="connector:"]').forEach((control) => { connectors[control.name.slice(10)] = control.value; });
        const core = { label: event.currentTarget.elements.label.value, slot: event.currentTarget.elements.slot.value, default_npc: event.currentTarget.elements.default_npc.checked, prompt: event.currentTarget.elements.prompt.value };
        status('Saving profile...', false);
        await responseData(await fetch(`${serverBaseUrl}/ui/api/chim_profile_manager.php`, { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({operation:'save',id:selectedProfileId,core,connectors,metadata}) }));
        await loadProfiles(selectedProfileId); status('Profile saved.', false);
    }
    async function copySetting(key) {
        if (!confirm(`Copy ${key.replaceAll('_',' ')} to every profile?`)) return;
        await responseData(await fetch(`${serverBaseUrl}/ui/api/chim_profile_manager.php`, { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({operation:'copy_setting',id:selectedProfileId,key}) }));
        status('Setting copied to all profiles.', false);
    }
    async function createProfile() {
        const label = prompt('New profile name:', 'New Profile'); if (label === null) return;
        const detail = await responseData(await fetch(`${serverBaseUrl}/ui/api/chim_profile_manager.php`, { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({operation:'create',label}) }));
        await loadProfiles(detail.core.id);
    }
    async function deleteProfile() {
        if (!selectedProfileId || !confirm('Delete this profile? This cannot be undone.')) return;
        await responseData(await fetch(`${serverBaseUrl}/ui/api/chim_profile_manager.php`, { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({operation:'delete',id:selectedProfileId}) }));
        selectedProfileId = 0; await loadProfiles();
    }
    function switchPage(page) {
        if (page === 'npcs') { command('tab_npcs'); return; }
        document.querySelectorAll('.top-tab').forEach((button) => button.classList.toggle('active', button.dataset.page === page));
        document.querySelectorAll('.page').forEach((element) => { const active = element.id === `${page}-page`; element.hidden = !active; element.classList.toggle('active', active); });
        if (page === 'globals' && !globalData) loadGlobals().catch(showError);
        if (page === 'profiles' && !profileData) loadProfiles().catch(showError);
    }
    function showError(error) { status(error.message || String(error), true); }
    function init() {
        enhanceSelect(byId('profile-form').elements.slot);
        byId('close-button').addEventListener('click', () => command('close'));
        document.querySelectorAll('.top-tab').forEach((button) => button.addEventListener('click', () => switchPage(button.dataset.page)));
        byId('globals-form').addEventListener('submit', (event) => saveGlobals(event).catch(showError));
        byId('profile-form').addEventListener('submit', (event) => saveProfile(event).catch(showError));
        byId('new-profile').addEventListener('click', () => createProfile().catch(showError));
        byId('delete-profile').addEventListener('click', () => deleteProfile().catch(showError));
        document.addEventListener('focusin', (event) => { if (event.target.matches('input, textarea, select')) command('input_capture|on'); });
        document.addEventListener('focusout', () => command('input_capture|off'));
        document.addEventListener('click', (event) => { if (!event.target.closest('.settings-tile-selector')) closeTileSelectors(); });
        document.addEventListener('keydown', (event) => { if (event.key === 'Escape') command('close'); });
        command('dom_ready');
    }
    window.setConfigManagerServerUrl = (value) => {
        serverBaseUrl = normalizeBaseUrl(value);
        if (!globalData && document.querySelector('.top-tab.active').dataset.page === 'globals') {
            loadGlobals().catch(showError);
        }
    };
    window.setConfigManagerTab = (tab) => switchPage(tab === 'profiles' ? 'profiles' : 'globals');
    window.onConfigManagerShown = () => { if (document.querySelector('.top-tab.active').dataset.page === 'profiles') loadProfiles(selectedProfileId).catch(showError); else loadGlobals().catch(showError); };
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init); else init();
})();
