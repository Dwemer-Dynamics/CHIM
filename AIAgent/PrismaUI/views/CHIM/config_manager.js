(function () {
    'use strict';

    const byId = (id) => document.getElementById(id);
    let serverBaseUrl = 'http://127.0.0.1:8081/HerikaServer';
    let globalData = null;
    let profileData = null;
    let selectedProfileId = 0;
    let activeGlobalTab = 'prompt-rechat';
    const SETTINGS_PAGES = ['globals', 'profiles', 'npcs', 'mcm'];
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
    /* ----- CHIM MCM (mirrors the six SkyUI MCM pages, rendered from native state) ----- */
    const MCM_PAGES = [
        { id: 'hotkeys', label: 'Hotkeys' },
        { id: 'auto_activate', label: 'Auto Activate' },
        { id: 'behavior', label: 'Behavior' },
        { id: 'sound', label: 'Sound' },
        { id: 'ai_agents', label: 'AI Agents' },
        { id: 'tools', label: 'Tools' }
    ];
    const MCM_PAGE_ALIASES = { hotkey: 'hotkeys', keys: 'hotkeys', keymaps: 'hotkeys', auto: 'auto_activate', autoactivate: 'auto_activate', agents: 'ai_agents', aiagents: 'ai_agents', audio: 'sound', tool: 'tools' };
    /* DirectInput (DIK) scan codes used by Skyrim and SkyUI keymaps.
       Rows are [scan code, KeyboardEvent.code ('' when the device cannot be captured in-view), display name]. */
    const MCM_KEY_CODES = [
        [1, 'Escape', 'Esc'], [2, 'Digit1', '1'], [3, 'Digit2', '2'], [4, 'Digit3', '3'], [5, 'Digit4', '4'],
        [6, 'Digit5', '5'], [7, 'Digit6', '6'], [8, 'Digit7', '7'], [9, 'Digit8', '8'], [10, 'Digit9', '9'],
        [11, 'Digit0', '0'], [12, 'Minus', 'Minus'], [13, 'Equal', 'Equals'], [14, 'Backspace', 'Backspace'],
        [15, 'Tab', 'Tab'], [16, 'KeyQ', 'Q'], [17, 'KeyW', 'W'], [18, 'KeyE', 'E'], [19, 'KeyR', 'R'],
        [20, 'KeyT', 'T'], [21, 'KeyY', 'Y'], [22, 'KeyU', 'U'], [23, 'KeyI', 'I'], [24, 'KeyO', 'O'],
        [25, 'KeyP', 'P'], [26, 'BracketLeft', 'Left Bracket'], [27, 'BracketRight', 'Right Bracket'],
        [28, 'Enter', 'Enter'], [29, 'ControlLeft', 'Left Ctrl'], [30, 'KeyA', 'A'], [31, 'KeyS', 'S'],
        [32, 'KeyD', 'D'], [33, 'KeyF', 'F'], [34, 'KeyG', 'G'], [35, 'KeyH', 'H'], [36, 'KeyJ', 'J'],
        [37, 'KeyK', 'K'], [38, 'KeyL', 'L'], [39, 'Semicolon', 'Semicolon'], [40, 'Quote', 'Apostrophe'],
        [41, 'Backquote', 'Grave'], [42, 'ShiftLeft', 'Left Shift'], [43, 'Backslash', 'Backslash'],
        [44, 'KeyZ', 'Z'], [45, 'KeyX', 'X'], [46, 'KeyC', 'C'], [47, 'KeyV', 'V'], [48, 'KeyB', 'B'],
        [49, 'KeyN', 'N'], [50, 'KeyM', 'M'], [51, 'Comma', 'Comma'], [52, 'Period', 'Period'],
        [53, 'Slash', 'Slash'], [54, 'ShiftRight', 'Right Shift'], [55, 'NumpadMultiply', 'Num *'],
        [56, 'AltLeft', 'Left Alt'], [57, 'Space', 'Space'], [58, 'CapsLock', 'Caps Lock'],
        [59, 'F1', 'F1'], [60, 'F2', 'F2'], [61, 'F3', 'F3'], [62, 'F4', 'F4'], [63, 'F5', 'F5'],
        [64, 'F6', 'F6'], [65, 'F7', 'F7'], [66, 'F8', 'F8'], [67, 'F9', 'F9'], [68, 'F10', 'F10'],
        [69, 'NumLock', 'Num Lock'], [70, 'ScrollLock', 'Scroll Lock'], [71, 'Numpad7', 'Num 7'],
        [72, 'Numpad8', 'Num 8'], [73, 'Numpad9', 'Num 9'], [74, 'NumpadSubtract', 'Num -'],
        [75, 'Numpad4', 'Num 4'], [76, 'Numpad5', 'Num 5'], [77, 'Numpad6', 'Num 6'], [78, 'NumpadAdd', 'Num +'],
        [79, 'Numpad1', 'Num 1'], [80, 'Numpad2', 'Num 2'], [81, 'Numpad3', 'Num 3'], [82, 'Numpad0', 'Num 0'],
        [83, 'NumpadDecimal', 'Num .'], [87, 'F11', 'F11'], [88, 'F12', 'F12'], [100, 'F13', 'F13'],
        [101, 'F14', 'F14'], [102, 'F15', 'F15'], [141, 'NumpadEqual', 'Num ='], [156, 'NumpadEnter', 'Num Enter'],
        [157, 'ControlRight', 'Right Ctrl'], [179, 'NumpadComma', 'Num ,'], [181, 'NumpadDivide', 'Num /'],
        [183, 'PrintScreen', 'Print Screen'], [184, 'AltRight', 'Right Alt'], [197, 'Pause', 'Pause'],
        [199, 'Home', 'Home'], [200, 'ArrowUp', 'Up Arrow'], [201, 'PageUp', 'Page Up'],
        [203, 'ArrowLeft', 'Left Arrow'], [205, 'ArrowRight', 'Right Arrow'], [207, 'End', 'End'],
        [208, 'ArrowDown', 'Down Arrow'], [209, 'PageDown', 'Page Down'], [210, 'Insert', 'Insert'],
        [211, 'Delete', 'Delete'], [219, 'MetaLeft', 'Left Win'], [220, 'MetaRight', 'Right Win'],
        [221, 'ContextMenu', 'Menu'], [256, '', 'Mouse 1'], [257, '', 'Mouse 2'], [258, '', 'Mouse 3'],
        [259, '', 'Mouse 4'], [260, '', 'Mouse 5'], [261, '', 'Mouse 6'], [262, '', 'Mouse 7'],
        [263, '', 'Mouse 8'], [264, '', 'Mouse Wheel Up'], [265, '', 'Mouse Wheel Down'],
        [266, '', 'Gamepad D-Pad Up'], [267, '', 'Gamepad D-Pad Down'], [268, '', 'Gamepad D-Pad Left'],
        [269, '', 'Gamepad D-Pad Right'], [270, '', 'Gamepad Start'], [271, '', 'Gamepad Back'],
        [272, '', 'Gamepad Left Stick'], [273, '', 'Gamepad Right Stick'], [274, '', 'Gamepad Left Shoulder'],
        [275, '', 'Gamepad Right Shoulder'], [276, '', 'Gamepad A'], [277, '', 'Gamepad B'],
        [278, '', 'Gamepad X'], [279, '', 'Gamepad Y'], [280, '', 'Gamepad Left Trigger'], [281, '', 'Gamepad Right Trigger']
    ];
    /* Printable KeyboardEvent.key values for the rows whose code is not the key itself. */
    const MCM_KEY_CHARS = {
        Minus: '-', Equal: '=', BracketLeft: '[', BracketRight: ']', Semicolon: ';', Quote: "'",
        Backquote: '`', Backslash: '\\', Comma: ',', Period: '.', Slash: '/', Space: ' '
    };
    const MCM_KEY_NAMES = new Map();
    const MCM_KEY_FROM_BROWSER = new Map();
    /* Prisma Ultralight can deliver an empty or unusable event.code, so keep an event.key map too. */
    const MCM_KEY_FROM_KEY = new Map();
    MCM_KEY_CODES.forEach((row) => {
        MCM_KEY_NAMES.set(row[0], row[2]);
        const code = row[1];
        if (!code) return;
        MCM_KEY_FROM_BROWSER.set(code, row[0]);
        const letter = /^Key([A-Z])$/.exec(code);
        const digit = /^Digit([0-9])$/.exec(code);
        /* Named keys and function keys report event.key === event.code; modifiers drop the side suffix. */
        const alias = letter ? letter[1].toLowerCase()
            : digit ? digit[1]
            : MCM_KEY_CHARS[code] || code.replace(/^(Shift|Control|Alt|Meta)(Left|Right)$/, '$1');
        /* First row wins so main-row keys keep priority over their numpad twins. */
        if (!MCM_KEY_FROM_KEY.has(alias)) MCM_KEY_FROM_KEY.set(alias, row[0]);
    });
    const MCM_SAVE_TIMEOUT_MS = 12000;
    let mcmState = null;
    let mcmAgents = null;
    let mcmRequested = false;
    let activeMcmPage = MCM_PAGES[0].id;
    let mcmSequence = 0;
    let mcmConfirmInvoker = null;
    let mcmConfirmAction = null;
    const mcmPending = new Map();
    /* Edits are staged in the view and only pushed to the game by Save. */
    const mcmStaged = new Map();
    const mcmSaveErrors = new Map();
    let mcmSaving = false;
    let mcmCapture = null;

    function mcmStatus(message, error) {
        const line = byId('mcm-status');
        if (!line) return;
        line.textContent = message || '';
        line.classList.toggle('error', !!error);
    }
    function parseMcmPayload(payload) {
        if (payload === null || payload === undefined) return null;
        if (typeof payload === 'string') { try { return JSON.parse(payload); } catch (_error) { return null; } }
        return typeof payload === 'object' ? payload : null;
    }
    function normalizeMcmPage(value) {
        const slug = String(value === null || value === undefined ? '' : value).trim().toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '');
        if (MCM_PAGES.some((page) => page.id === slug)) return slug;
        return MCM_PAGE_ALIASES[slug.replace(/_/g, '')] || MCM_PAGE_ALIASES[slug] || slug;
    }
    function decimalsFor(step) {
        const text = String(step);
        const dot = text.indexOf('.');
        return dot < 0 ? 0 : Math.min(4, text.length - dot - 1);
    }
    function clampToStep(value, min, max, step) {
        let numeric = Number(value);
        if (!Number.isFinite(numeric)) numeric = min;
        numeric = Math.min(max, Math.max(min, numeric));
        numeric = min + Math.round((numeric - min) / step) * step;
        return Number(Math.min(max, Math.max(min, numeric)).toFixed(decimalsFor(step)));
    }
    function setMcmBusy(button, busy) {
        if (!button) return;
        if (busy) {
            if (!button.dataset.idleLabel) button.dataset.idleLabel = button.textContent;
            button.textContent = button.dataset.busyLabel || 'Working...';
        } else if (button.dataset.idleLabel) {
            button.textContent = button.dataset.idleLabel;
        }
        button.disabled = !!busy;
        button.setAttribute('aria-busy', busy ? 'true' : 'false');
    }
    function mcmResultNode(button) {
        const card = button && button.closest('.mcm-tool-card, .mcm-agent-toolbar, .mcm-agent-row');
        return card ? card.querySelector('.mcm-result') : null;
    }
    function sendMcmCommand(request, options) {
        const settings = options || {};
        const button = settings.button || null;
        if (button) {
            if (settings.busyLabel) button.dataset.busyLabel = settings.busyLabel;
            setMcmBusy(button, true);
        }
        const result = mcmResultNode(button);
        if (result) { result.textContent = settings.pendingMessage || 'Working...'; result.classList.remove('error'); }
        mcmStatus(settings.pendingMessage || 'Sending command...', false);
        const record = { buttonId: button ? button.id : '', onResult: settings.onResult, timeoutId: 0 };
        mcmPending.set(request, record);
        if (Number(settings.timeoutMs) > 0) {
            record.timeoutId = setTimeout(() => {
                if (mcmPending.get(request) !== record) return;
                mcmPending.delete(request);
                setMcmBusy(record.buttonId ? byId(record.buttonId) : null, false);
                const message = 'The game did not respond in time.';
                mcmStatus(message, true);
                if (typeof record.onResult === 'function') record.onResult(false, message);
            }, Number(settings.timeoutMs));
        }
        command(request);
    }
    function matchingMcmPendingRequest(request) {
        const exact = `mcm|${request}`;
        if (mcmPending.has(exact)) return exact;
        const withValue = Array.from(mcmPending.keys()).find((key) => key.startsWith(`${exact}|`));
        if (withValue) return withValue;
        const family = request.split('|', 1)[0];
        if (family === 'agent_add' || family === 'agent_remove') {
            return Array.from(mcmPending.keys()).find((key) => key.startsWith(`mcm|${family}|`)) || '';
        }
        return '';
    }

    function mcmHelp(row, entry) {
        const text = String(entry.description === null || entry.description === undefined ? '' : entry.description).trim();
        if (!text) return '';
        const id = `mcm-help-${++mcmSequence}`;
        const help = document.createElement('span');
        help.className = 'mcm-help';
        help.id = id;
        help.setAttribute('role', 'tooltip');
        help.textContent = text;
        row.appendChild(help);
        row.classList.add('has-help');
        return id;
    }
    function mcmRowHead(entry) {
        const head = document.createElement('div');
        head.className = 'mcm-row-head';
        const label = document.createElement('span');
        label.className = 'mcm-label';
        label.id = `mcm-label-${++mcmSequence}`;
        label.textContent = String(entry.label || entry.key || '');
        head.appendChild(label);
        const unit = String(entry.unit === null || entry.unit === undefined ? '' : entry.unit).trim();
        if (unit) {
            const badge = document.createElement('span');
            badge.className = 'mcm-unit';
            badge.textContent = unit;
            head.appendChild(badge);
        }
        return { head: head, labelId: label.id };
    }
    function mcmControlId(kind, key) {
        return `mcm-${kind}-${String(key === null || key === undefined ? '' : key).replace(/[^A-Za-z0-9_-]/g, '')}`;
    }
    function mcmEntryList() { return mcmState && Array.isArray(mcmState.entries) ? mcmState.entries : []; }
    function mcmBaseValue(entry) {
        const numeric = Number(entry.value);
        return Number.isFinite(numeric) ? numeric : 0;
    }
    function mcmValue(entry) {
        const key = String(entry.key || '');
        return mcmStaged.has(key) ? mcmStaged.get(key) : mcmBaseValue(entry);
    }
    function mcmNumberText(value) {
        const numeric = Number(value);
        return Number.isFinite(numeric) ? String(Number(numeric.toFixed(4))) : '0';
    }
    function mcmKeyName(value) {
        const code = Number(value);
        if (!Number.isFinite(code) || code <= 0) return 'Unbound';
        const known = MCM_KEY_NAMES.get(code);
        if (known) return known;
        return 'Unknown key';
    }
    function mcmDescribe(control, ids) {
        const list = ids.filter(Boolean);
        if (list.length) control.setAttribute('aria-describedby', list.join(' '));
    }
    /* Every editable row carries a hidden "Unsaved" pill and an error line, both wired into aria-describedby. */
    function mcmRowState(row, entry, host) {
        const key = String(entry.key || '');
        const pill = document.createElement('span');
        pill.className = 'mcm-staged-pill';
        pill.id = `mcm-staged-${++mcmSequence}`;
        pill.textContent = 'Unsaved';
        pill.hidden = !mcmStaged.has(key);
        (host || row).appendChild(pill);
        const error = document.createElement('p');
        error.className = 'mcm-row-error';
        error.id = `mcm-error-${++mcmSequence}`;
        const message = mcmSaveErrors.get(key) || '';
        error.textContent = message;
        error.hidden = !message;
        row.appendChild(error);
        row.classList.toggle('is-staged', mcmStaged.has(key));
        row.classList.toggle('has-error', !!message);
        return { pillId: pill.id, errorId: error.id };
    }
    function mcmSyncRowState(row, entry) {
        if (!row) return;
        const key = String(entry.key || '');
        const staged = mcmStaged.has(key);
        const message = mcmSaveErrors.get(key) || '';
        row.classList.toggle('is-staged', staged);
        row.classList.toggle('has-error', !!message);
        const pill = row.querySelector('.mcm-staged-pill');
        if (pill) pill.hidden = !staged;
        const error = row.querySelector('.mcm-row-error');
        if (error) { error.textContent = message; error.hidden = !message; }
    }
    function stageMcmValue(entry, value, row) {
        const key = String(entry.key || '');
        if (!key) return;
        if (Number(value) === mcmBaseValue(entry)) mcmStaged.delete(key);
        else mcmStaged.set(key, Number(value));
        mcmSaveErrors.delete(key);
        mcmSyncRowState(row, entry);
        refreshMcmSaveBar();
    }

    /* ----- Staged edits: dirty state, ordered save, explicit discard ----- */
    function refreshMcmSaveBar() {
        const bar = byId('mcm-save-bar');
        if (!bar) return;
        const count = mcmStaged.size;
        bar.dataset.staged = count ? 'true' : 'false';
        bar.dataset.saving = mcmSaving ? 'true' : 'false';
        const summary = byId('mcm-staged-summary');
        if (summary) {
            summary.textContent = mcmSaving
                ? 'Applying staged settings...'
                : (count ? `${count} setting${count === 1 ? '' : 's'} staged, not applied yet` : 'No unsaved changes');
        }
        const save = byId('mcm-save');
        if (save) {
            save.disabled = mcmSaving || !count;
            save.setAttribute('aria-busy', mcmSaving ? 'true' : 'false');
        }
        const discard = byId('mcm-discard');
        if (discard) discard.disabled = mcmSaving || !count;
        const refresh = byId('mcm-refresh');
        if (refresh) refresh.disabled = mcmSaving;
    }
    function mcmSaveNote(message, state) {
        const line = byId('mcm-save-note');
        if (!line) return;
        line.textContent = message || '';
        line.dataset.state = state || 'idle';
    }
    function mcmStagedQueue() {
        const queue = [];
        const seen = new Set();
        mcmEntryList().forEach((entry) => {
            const key = String(entry.key || '');
            if (!mcmStaged.has(key) || seen.has(key)) return;
            seen.add(key);
            queue.push({ key: key, label: String(entry.label || key), value: mcmStaged.get(key) });
        });
        mcmStaged.forEach((value, key) => {
            if (seen.has(key)) return;
            seen.add(key);
            queue.push({ key: key, label: key, value: value });
        });
        return queue;
    }
    function mcmFocusEntryControl(key) {
        const candidates = ['toggle', 'range', 'keycap'].map((kind) => byId(mcmControlId(kind, key))).filter(Boolean);
        const target = candidates.find((node) => !node.disabled) || candidates[0];
        if (target) target.focus();
    }
    function saveMcmChanges() {
        if (mcmSaving || !mcmStaged.size) return;
        stopMcmCapture();
        const queue = mcmStagedQueue();
        const failures = [];
        mcmSaveErrors.clear();
        mcmSaving = true;
        refreshMcmSaveBar();
        renderMcmPanel(activeMcmPage);
        let index = 0;
        const finish = () => {
            mcmSaving = false;
            failures.forEach((failure) => mcmSaveErrors.set(failure.key, failure.message));
            refreshMcmSaveBar();
            renderMcmPanel(activeMcmPage);
            const applied = queue.length - failures.length;
            if (!failures.length) {
                const done = `Applied ${applied} setting${applied === 1 ? '' : 's'}.`;
                mcmSaveNote(done, 'ok');
                mcmStatus(done, false);
                const save = byId('mcm-save');
                if (save) save.focus();
                return;
            }
            const note = `${applied} of ${queue.length} applied. Still staged: ${failures.map((failure) => failure.label).join(', ')}.`;
            mcmSaveNote(note, 'error');
            mcmStatus(note, true);
            mcmFocusEntryControl(failures[0].key);
        };
        const step = () => {
            if (index >= queue.length) { finish(); return; }
            const item = queue[index];
            const position = `${index + 1} of ${queue.length}`;
            mcmSaveNote(`Applying ${position}: ${item.label}...`, 'busy');
            sendMcmCommand(`mcm|set|${item.key}|${mcmNumberText(item.value)}`, {
                pendingMessage: `Applying ${item.label} (${position})...`,
                timeoutMs: MCM_SAVE_TIMEOUT_MS,
                onResult: (ok, message) => {
                    if (ok) mcmStaged.delete(item.key);
                    else failures.push({ key: item.key, label: item.label, message: message || 'The game rejected this value.' });
                    index += 1;
                    step();
                }
            });
        };
        step();
    }
    function discardMcmChanges(note) {
        stopMcmCapture();
        mcmStaged.clear();
        mcmSaveErrors.clear();
        refreshMcmSaveBar();
        renderMcmPanel(activeMcmPage);
        mcmSaveNote(note === undefined ? 'Staged changes discarded.' : note, 'idle');
    }
    function confirmDiscardMcmChanges(options) {
        const count = mcmStaged.size;
        openMcmConfirm({
            title: options.title,
            body: `${options.body} ${count} staged change${count === 1 ? '' : 's'} will be lost. Use Save first to keep them.`,
            confirmLabel: options.confirmLabel,
            danger: true,
            onConfirm: options.onConfirm
        });
    }

    /* ----- Hotkey capture: only runs while the keycap button owns focus ----- */
    function mcmSyncKeycap(button, entry) {
        if (!button) return;
        const value = Number(mcmValue(entry));
        const name = mcmKeyName(value);
        const label = String(entry.label || entry.key || 'Hotkey');
        button.textContent = name;
        button.dataset.unbound = value > 0 ? 'false' : 'true';
        button.setAttribute('aria-label', `${label} hotkey, currently ${name}. Activate to capture a new key.`);
        const row = button.closest('.mcm-row');
        const clear = row ? row.querySelector('.mcm-keycap-clear') : null;
        if (clear) clear.disabled = mcmSaving || value <= 0;
    }
    function stopMcmCapture(message, state) {
        const capture = mcmCapture;
        if (!capture) {
            if (message) mcmSaveNote(message, state);
            return;
        }
        mcmCapture = null;
        document.removeEventListener('keydown', mcmCaptureKeydown, true);
        const button = byId(capture.buttonId);
        if (button) {
            button.dataset.capturing = 'false';
            mcmSyncKeycap(button, capture.entry);
        }
        command('input_capture|off');
        if (message) mcmSaveNote(message, state);
    }
    function startMcmCapture(entry, button) {
        if (mcmSaving || button.disabled) return;
        if (mcmCapture && mcmCapture.buttonId === button.id) { stopMcmCapture('Rebind cancelled.', 'idle'); return; }
        stopMcmCapture();
        mcmCapture = { entry: entry, buttonId: button.id };
        button.dataset.capturing = 'true';
        button.textContent = 'Press a key';
        button.setAttribute('aria-label', `${entry.label || entry.key} hotkey: press a key to stage it, Escape to cancel, Backspace to unbind, Tab to leave.`);
        if (document.activeElement !== button) button.focus();
        command('input_capture|on');
        document.addEventListener('keydown', mcmCaptureKeydown, true);
        mcmSaveNote('Press a key to stage a new binding. Escape cancels, Backspace unbinds, Tab leaves.', 'busy');
    }
    function stageMcmCapturedKey(code) {
        const capture = mcmCapture;
        if (!capture) return;
        const entry = capture.entry;
        const button = byId(capture.buttonId);
        const row = button ? button.closest('.mcm-row') : null;
        const conflict = code > 0 ? mcmEntryList().find((candidate) => {
            return String(candidate.type || '').toLowerCase() === 'keymap' &&
                String(candidate.key || '') !== String(entry.key || '') &&
                Number(mcmValue(candidate)) === Number(code);
        }) : null;
        if (conflict) {
            stopMcmCapture(`${mcmKeyName(code)} is already assigned to ${conflict.label || conflict.key}. Nothing was changed.`, 'error');
            return;
        }
        stopMcmCapture();
        stageMcmValue(entry, code, row);
        mcmSyncKeycap(button, entry);
        mcmSaveNote(`${entry.label || entry.key} staged as ${mcmKeyName(code)}. Use Save to apply.`, 'idle');
    }
    /* Either field can be the usable one in-game, so a named key matches on whichever arrives. */
    function mcmEventIs(event, name) {
        return event.key === name || event.code === name;
    }
    function mcmResolveKeyCode(event) {
        const byCode = MCM_KEY_FROM_BROWSER.get(event.code);
        if (byCode) return byCode;
        const key = String(event.key || '');
        if (!key) return 0;
        /* Single characters are folded to their unshifted form so Shift+X still resolves to X. */
        return MCM_KEY_FROM_KEY.get(key.length === 1 ? key.toLowerCase() : key) || 0;
    }
    function mcmCaptureKeydown(event) {
        if (!mcmCapture) return;
        const button = byId(mcmCapture.buttonId);
        /* Capture is released as soon as the keycap button stops owning focus. */
        if (!button || document.activeElement !== button || mcmEventIs(event, 'Tab')) {
            stopMcmCapture('Rebind cancelled.', 'idle');
            return;
        }
        /* Swallow the key so it never reaches the global Escape-to-close handler or the game. */
        event.preventDefault();
        event.stopPropagation();
        if (event.repeat) return;
        if (mcmEventIs(event, 'Escape')) { stopMcmCapture('Rebind cancelled.', 'idle'); return; }
        if (mcmEventIs(event, 'Backspace')) { stageMcmCapturedKey(-1); return; }
        const code = mcmResolveKeyCode(event);
        if (!code) { stopMcmCapture('That key is not in the Skyrim key map, so nothing was staged.', 'error'); return; }
        stageMcmCapturedKey(code);
    }

    function mcmToggleRow(entry, readonly) {
        const row = document.createElement('div');
        row.className = 'mcm-row mcm-row-toggle';
        const wrapper = document.createElement('label');
        wrapper.className = 'toggle-field mcm-toggle';
        const input = document.createElement('input');
        input.type = 'checkbox';
        input.id = mcmControlId('toggle', entry.key);
        input.checked = Number(mcmValue(entry)) > 0.5;
        input.disabled = readonly || mcmSaving;
        const label = document.createElement('span');
        label.textContent = String(entry.label || entry.key || '');
        wrapper.append(input, label);
        row.appendChild(wrapper);
        const state = mcmRowState(row, entry);
        const helpId = mcmHelp(row, entry);
        mcmDescribe(input, [helpId, state.pillId, state.errorId]);
        input.addEventListener('change', () => stageMcmValue(entry, input.checked ? 1 : 0, row));
        return row;
    }
    function mcmSliderRow(entry, readonly) {
        const row = document.createElement('div');
        row.className = 'mcm-row mcm-row-slider';
        const min = Number.isFinite(Number(entry.min)) ? Number(entry.min) : 0;
        const max = Number.isFinite(Number(entry.max)) && Number(entry.max) > min ? Number(entry.max) : min + 100;
        const step = Number(entry.step) > 0 ? Number(entry.step) : 1;
        const heading = mcmRowHead(entry);
        const controls = document.createElement('div');
        controls.className = 'mcm-slider-controls';
        const range = document.createElement('input');
        const number = document.createElement('input');
        const current = clampToStep(mcmValue(entry), min, max, step);
        [range, number].forEach((control) => {
            control.min = String(min);
            control.max = String(max);
            control.step = String(step);
            control.disabled = readonly || mcmSaving;
            control.value = String(current);
        });
        range.type = 'range';
        range.id = mcmControlId('range', entry.key);
        range.className = 'mcm-range';
        range.setAttribute('aria-labelledby', heading.labelId);
        number.type = 'number';
        number.id = mcmControlId('number', entry.key);
        number.className = 'mcm-number';
        number.setAttribute('aria-label', `${entry.label || entry.key} exact value`);
        controls.append(range, number);
        row.append(heading.head, controls);
        const state = mcmRowState(row, entry, heading.head);
        const helpId = mcmHelp(row, entry);
        if (helpId) heading.head.setAttribute('aria-describedby', helpId);
        mcmDescribe(range, [helpId, state.pillId, state.errorId]);
        mcmDescribe(number, [helpId, state.pillId, state.errorId]);
        const commit = (raw) => {
            const value = clampToStep(raw, min, max, step);
            range.value = String(value);
            number.value = String(value);
            stageMcmValue(entry, value, row);
        };
        range.addEventListener('input', () => { number.value = range.value; });
        range.addEventListener('change', () => commit(range.value));
        number.addEventListener('change', () => commit(number.value));
        return row;
    }
    function mcmKeymapRow(entry) {
        const row = document.createElement('div');
        row.className = 'mcm-row mcm-row-keymap';
        const heading = mcmRowHead(entry);
        const controls = document.createElement('div');
        controls.className = 'mcm-keymap-controls';
        const button = document.createElement('button');
        button.type = 'button';
        button.id = mcmControlId('keycap', entry.key);
        button.className = 'mcm-keycap';
        button.dataset.capturing = 'false';
        button.disabled = mcmSaving;
        const clear = document.createElement('button');
        clear.type = 'button';
        clear.id = mcmControlId('keyclear', entry.key);
        clear.className = 'button secondary compact mcm-keycap-clear';
        clear.textContent = 'Unbind';
        clear.setAttribute('aria-label', `Unbind ${entry.label || entry.key} hotkey`);
        const hint = document.createElement('span');
        hint.className = 'mcm-keycap-hint';
        hint.textContent = 'Enter to rebind';
        controls.append(button, clear, hint);
        row.append(heading.head, controls);
        const state = mcmRowState(row, entry, heading.head);
        const helpId = mcmHelp(row, entry);
        mcmDescribe(button, [helpId, state.pillId, state.errorId]);
        button.addEventListener('click', () => startMcmCapture(entry, button));
        button.addEventListener('blur', () => {
            if (mcmCapture && mcmCapture.buttonId === button.id) stopMcmCapture('Rebind cancelled.', 'idle');
        });
        clear.addEventListener('click', () => {
            stopMcmCapture();
            stageMcmValue(entry, -1, row);
            mcmSyncKeycap(button, entry);
            mcmSaveNote(`${entry.label || entry.key} staged as Unbound. Use Save to apply.`, 'idle');
        });
        mcmSyncKeycap(button, entry);
        return row;
    }
    function mcmReadonlyRow(entry, type) {
        const row = document.createElement('div');
        row.className = `mcm-row mcm-row-${type} mcm-row-readonly`;
        const heading = mcmRowHead(entry);
        const badge = document.createElement('span');
        badge.className = 'mcm-readonly-badge';
        badge.textContent = 'Read-only';
        heading.head.appendChild(badge);
        const text = String(entry.value === null || entry.value === undefined ? '' : entry.value).trim();
        const value = document.createElement('span');
        value.className = 'mcm-readonly-value';
        if (type === 'keymap') value.textContent = mcmKeyName(text);
        else value.textContent = text ? text : 'Not reported';
        value.setAttribute('aria-labelledby', heading.labelId);
        row.append(heading.head, value);
        const helpId = mcmHelp(row, entry);
        if (helpId) {
            value.setAttribute('aria-describedby', helpId);
            value.tabIndex = 0;
        }
        return row;
    }
    function mcmRow(entry) {
        const type = String(entry.type || 'text').toLowerCase();
        const readonly = entry.readonly === true;
        if (type === 'toggle') return mcmToggleRow(entry, readonly);
        if (type === 'slider') return mcmSliderRow(entry, readonly);
        /* Keymaps stay editable through key capture; the payload readonly flag only mirrors the legacy MCM display. */
        if (type === 'keymap') return mcmKeymapRow(entry);
        return mcmReadonlyRow(entry, 'text');
    }
    function mcmNote(text) {
        const note = document.createElement('p');
        note.className = 'mcm-note';
        note.textContent = text;
        return note;
    }
    function renderMcmSettingsPanel(pageId, panel) {
        if (!mcmState) { panel.appendChild(mcmNote('Waiting for CHIM MCM data from the game.')); return; }
        const entries = (mcmState.entries || []).filter((entry) => normalizeMcmPage(entry.page) === pageId);
        if (!entries.length) { panel.appendChild(mcmNote('No settings were reported for this page.')); return; }
        const grid = document.createElement('div');
        grid.className = 'settings-grid';
        const sections = new Map();
        entries.forEach((entry) => {
            const name = String(entry.section || 'General');
            if (!sections.has(name)) sections.set(name, []);
            sections.get(name).push(entry);
        });
        sections.forEach((items, name) => {
            const card = document.createElement('section');
            card.className = 'settings-section mcm-section';
            const title = document.createElement('h2');
            title.textContent = name;
            card.appendChild(title);
            items.filter((item) => item.deprecated !== true).forEach((item) => card.appendChild(mcmRow(item)));
            const deprecated = items.filter((item) => item.deprecated === true);
            if (deprecated.length) {
                const details = document.createElement('details');
                details.className = 'mcm-deprecated';
                const summary = document.createElement('summary');
                summary.textContent = `Deprecated (${deprecated.length})`;
                details.appendChild(summary);
                deprecated.forEach((item) => details.appendChild(mcmRow(item)));
                card.appendChild(details);
            }
            grid.appendChild(card);
        });
        panel.appendChild(grid);
    }
    function mcmAgentRow(agent, mode) {
        const row = document.createElement('div');
        row.className = 'mcm-agent-row';
        const identity = document.createElement('div');
        identity.className = 'mcm-agent-identity';
        const name = document.createElement('strong');
        name.textContent = String(agent.name || agent.refid || 'Unknown');
        const refid = document.createElement('small');
        refid.textContent = String(agent.refid || '');
        identity.append(name, refid);
        const button = document.createElement('button');
        button.type = 'button';
        button.id = `mcm-agent-${mode}-${String(agent.refid || ++mcmSequence).replace(/[^A-Za-z0-9_-]/g, '')}`;
        button.className = `button compact${mode === 'remove' ? ' danger' : ' secondary'}`;
        button.textContent = mode === 'remove' ? 'Remove' : 'Add';
        button.addEventListener('click', () => openMcmConfirm({
            title: mode === 'remove' ? 'Remove AI Agent' : 'Add AI Agent',
            body: mode === 'remove'
                ? `Remove ${name.textContent} from the active AI Agents? Their stored CHIM NPC profile is not deleted.`
                : `Add ${name.textContent} as an active AI Agent for this session?`,
            confirmLabel: mode === 'remove' ? 'Remove' : 'Add',
            danger: mode === 'remove',
            onConfirm: () => sendMcmCommand(`mcm|agent_${mode}|${agent.refid}`, {
                button: button,
                busyLabel: mode === 'remove' ? 'Removing...' : 'Adding...',
                pendingMessage: `${mode === 'remove' ? 'Removing' : 'Adding'} ${name.textContent}...`
            })
        }));
        const result = document.createElement('p');
        result.className = 'mcm-result';
        result.setAttribute('role', 'status');
        result.setAttribute('aria-live', 'polite');
        row.append(identity, button, result);
        return row;
    }
    function renderMcmAgentsPanel(panel) {
        const banner = document.createElement('div');
        banner.className = 'mcm-runtime-banner';
        const badge = document.createElement('span');
        badge.className = 'mcm-runtime-badge';
        badge.textContent = 'Runtime';
        const copy = document.createElement('p');
        copy.textContent = 'Live session state from the running game. These are not the stored CHIM NPC profiles on the CHIM NPCs tab.';
        banner.append(badge, copy);
        panel.appendChild(banner);

        const toolbar = document.createElement('div');
        toolbar.className = 'mcm-agent-toolbar';
        const stamp = document.createElement('span');
        stamp.className = 'mcm-agent-stamp';
        stamp.textContent = mcmAgents && mcmAgents.updated_at ? `Updated ${mcmAgents.updated_at}` : 'Not refreshed yet';
        const refresh = document.createElement('button');
        refresh.type = 'button';
        refresh.id = 'mcm-agents-refresh';
        refresh.className = 'button secondary compact';
        refresh.textContent = 'Refresh';
        refresh.addEventListener('click', () => sendMcmCommand('mcm|agents_refresh', { button: refresh, busyLabel: 'Refreshing...', pendingMessage: 'Refreshing AI Agents...' }));
        const addAll = document.createElement('button');
        addAll.type = 'button';
        addAll.id = 'mcm-agents-add-all';
        addAll.className = 'button secondary compact';
        addAll.textContent = 'Add All Available';
        addAll.addEventListener('click', () => sendMcmCommand('mcm|agents_add_all', { button: addAll, busyLabel: 'Adding...', pendingMessage: 'Adding all available NPCs as AI Agents...' }));
        const removeAll = document.createElement('button');
        removeAll.type = 'button';
        removeAll.id = 'mcm-agents-remove-all';
        removeAll.className = 'button danger compact';
        removeAll.textContent = 'Remove All';
        removeAll.addEventListener('click', () => openMcmConfirm({
            title: 'Remove all AI Agents',
            body: 'Remove every active AI Agent from this session? Stored CHIM NPC profiles are not deleted.',
            confirmLabel: 'Remove All',
            danger: true,
            onConfirm: () => sendMcmCommand('mcm|agents_remove_all', { button: removeAll, busyLabel: 'Removing...', pendingMessage: 'Removing all AI Agents...' })
        }));
        const result = document.createElement('p');
        result.className = 'mcm-result';
        result.setAttribute('role', 'status');
        result.setAttribute('aria-live', 'polite');
        toolbar.append(stamp, refresh, addAll, removeAll, result);
        panel.appendChild(toolbar);

        const columns = document.createElement('div');
        columns.className = 'mcm-agent-columns';
        [['active', 'Active AI Agents', 'remove'], ['available', 'Nearby Available NPCs', 'add']].forEach((definition) => {
            const list = (mcmAgents && Array.isArray(mcmAgents[definition[0]])) ? mcmAgents[definition[0]] : [];
            const card = document.createElement('section');
            card.className = 'settings-section mcm-agent-card';
            const heading = document.createElement('h2');
            heading.textContent = `${definition[1]} (${list.length})`;
            card.appendChild(heading);
            if (!list.length) card.appendChild(mcmNote(mcmAgents ? 'None reported.' : 'Refresh to load runtime agents.'));
            list.forEach((agent) => card.appendChild(mcmAgentRow(agent, definition[2])));
            columns.appendChild(card);
        });
        panel.appendChild(columns);
    }
    function mcmToolCard(config) {
        const card = document.createElement('article');
        card.className = 'mcm-tool-card';
        const copy = document.createElement('div');
        copy.className = 'mcm-tool-copy';
        const title = document.createElement('h3');
        title.textContent = config.title;
        const description = document.createElement('p');
        description.textContent = config.description;
        copy.append(title, description);
        const button = document.createElement('button');
        button.type = 'button';
        button.id = config.id;
        button.className = 'button secondary';
        button.textContent = config.action;
        const result = document.createElement('p');
        result.className = 'mcm-result';
        result.setAttribute('role', 'status');
        result.setAttribute('aria-live', 'polite');
        const run = () => sendMcmCommand(config.request, { button: button, busyLabel: config.busyLabel, pendingMessage: config.pendingMessage });
        button.addEventListener('click', () => {
            if (!config.confirm) { run(); return; }
            openMcmConfirm({ title: config.confirm.title, body: config.confirm.body, confirmLabel: config.confirm.label, onConfirm: run });
        });
        card.append(copy, button, result);
        return card;
    }
    function renderMcmToolsPanel(panel) {
        const list = document.createElement('div');
        list.className = 'mcm-tool-list';
        list.appendChild(mcmToolCard({
            id: 'mcm-tool-sync-factions',
            title: 'Sync Factions and Locations',
            description: 'Rebuilds the factions and locations database from the loaded game. The game stays blocked for the whole run, which usually takes 3 to 5 minutes.',
            action: 'Run Sync',
            busyLabel: 'Syncing...',
            pendingMessage: 'Syncing factions and locations. The game stays blocked until this finishes, usually 3 to 5 minutes.',
            request: 'mcm|tool|sync_factions_locations',
            confirm: {
                title: 'Sync factions and locations',
                body: 'This blocks the game for roughly 3 to 5 minutes and cannot be cancelled once it starts. Continue?',
                label: 'Start Sync'
            }
        }));
        list.appendChild(mcmToolCard({
            id: 'mcm-tool-voice-samples',
            title: 'Send Voice Samples',
            description: 'Uploads the local voice sample set to the CHIM server so voices can be matched.',
            action: 'Send Samples',
            busyLabel: 'Sending...',
            pendingMessage: 'Sending voice samples...',
            request: 'mcm|tool|send_voice_samples'
        }));
        panel.appendChild(list);
    }
    function renderMcmPanel(pageId) {
        const panel = byId(`mcm-panel-${pageId}`);
        if (!panel) return;
        panel.replaceChildren();
        if (pageId === 'ai_agents') renderMcmAgentsPanel(panel);
        else if (pageId === 'tools') renderMcmToolsPanel(panel);
        else renderMcmSettingsPanel(pageId, panel);
    }
    function mcmTabButtons() { return Array.from(document.querySelectorAll('.mcm-sub-tab')); }
    function switchMcmPage(pageId) {
        const id = MCM_PAGES.some((page) => page.id === pageId) ? pageId : MCM_PAGES[0].id;
        stopMcmCapture();
        command('input_capture|off');
        closeMcmConfirm(false);
        activeMcmPage = id;
        mcmTabButtons().forEach((tab) => {
            const active = tab.dataset.mcmPage === id;
            tab.classList.toggle('active', active);
            tab.setAttribute('aria-selected', active ? 'true' : 'false');
            tab.tabIndex = active ? 0 : -1;
        });
        document.querySelectorAll('.mcm-panel').forEach((panel) => { panel.hidden = panel.id !== `mcm-panel-${id}`; });
        renderMcmPanel(id);
        if (id === 'ai_agents' && !mcmAgents) sendMcmCommand('mcm|agents_refresh', { pendingMessage: 'Loading AI Agents...' });
    }
    function tabListKeydown(event, buttons) {
        const index = buttons.indexOf(document.activeElement);
        if (index < 0) return;
        let next = -1;
        if (event.key === 'ArrowRight' || event.key === 'ArrowDown') next = (index + 1) % buttons.length;
        else if (event.key === 'ArrowLeft' || event.key === 'ArrowUp') next = (index - 1 + buttons.length) % buttons.length;
        else if (event.key === 'Home') next = 0;
        else if (event.key === 'End') next = buttons.length - 1;
        else return;
        event.preventDefault();
        buttons.forEach((button, position) => { button.tabIndex = position === next ? 0 : -1; });
        buttons[next].focus();
    }
    function buildMcmTabs() {
        const list = byId('mcm-sub-tabs');
        const panels = byId('mcm-panels');
        list.replaceChildren();
        panels.replaceChildren();
        MCM_PAGES.forEach((page, index) => {
            const tab = document.createElement('button');
            tab.type = 'button';
            tab.id = `mcm-tab-${page.id}`;
            tab.className = `sub-tab mcm-sub-tab${index === 0 ? ' active' : ''}`;
            tab.dataset.mcmPage = page.id;
            tab.textContent = page.label;
            tab.setAttribute('role', 'tab');
            tab.setAttribute('aria-controls', `mcm-panel-${page.id}`);
            tab.setAttribute('aria-selected', index === 0 ? 'true' : 'false');
            tab.tabIndex = index === 0 ? 0 : -1;
            tab.addEventListener('click', () => switchMcmPage(page.id));
            list.appendChild(tab);
            const panel = document.createElement('div');
            panel.id = `mcm-panel-${page.id}`;
            panel.className = 'mcm-panel';
            panel.setAttribute('role', 'tabpanel');
            panel.setAttribute('aria-labelledby', tab.id);
            panel.hidden = index !== 0;
            panels.appendChild(panel);
        });
        list.addEventListener('keydown', (event) => tabListKeydown(event, mcmTabButtons()));
    }
    function requestMcmSnapshot() {
        mcmRequested = true;
        mcmStatus('Loading CHIM MCM state...', false);
        command('mcm|snapshot');
    }
    function openMcmPage() {
        renderMcmPanel(activeMcmPage);
        if (!mcmRequested || !mcmState) requestMcmSnapshot();
        if (activeMcmPage === 'ai_agents' && !mcmAgents) command('mcm|agents_refresh');
    }

    function mcmConfirmFocusable() {
        return Array.from(byId('mcm-confirm-backdrop').querySelectorAll('button:not([disabled])'));
    }
    function openMcmConfirm(options) {
        const backdrop = byId('mcm-confirm-backdrop');
        const accept = byId('mcm-confirm-accept');
        byId('mcm-confirm-title').textContent = options.title;
        byId('mcm-confirm-body').textContent = options.body;
        accept.textContent = options.confirmLabel || 'Confirm';
        accept.classList.toggle('danger', !!options.danger);
        accept.classList.toggle('primary', !options.danger);
        mcmConfirmInvoker = document.activeElement instanceof HTMLElement ? document.activeElement : null;
        mcmConfirmAction = typeof options.onConfirm === 'function' ? options.onConfirm : null;
        backdrop.classList.remove('hidden');
        command('input_capture|off');
        accept.focus();
    }
    function closeMcmConfirm(restoreFocus) {
        const backdrop = byId('mcm-confirm-backdrop');
        if (!backdrop || backdrop.classList.contains('hidden')) return;
        backdrop.classList.add('hidden');
        mcmConfirmAction = null;
        const invoker = mcmConfirmInvoker;
        mcmConfirmInvoker = null;
        if (restoreFocus !== false && invoker && document.contains(invoker) && !invoker.disabled) invoker.focus();
    }
    function initMcm() {
        buildMcmTabs();
        byId('mcm-refresh').addEventListener('click', () => {
            requestMcmSnapshot();
            if (mcmStaged.size) mcmSaveNote('Refreshing game values; your unsaved changes remain staged.', 'busy');
        });
        byId('mcm-save').addEventListener('click', saveMcmChanges);
        byId('mcm-discard').addEventListener('click', () => {
            if (!mcmStaged.size || mcmSaving) return;
            confirmDiscardMcmChanges({
                title: 'Discard unsaved changes?',
                body: 'Discard the edits in this CHIM MCM tab?',
                confirmLabel: 'Discard',
                onConfirm: () => discardMcmChanges()
            });
        });
        refreshMcmSaveBar();
        const backdrop = byId('mcm-confirm-backdrop');
        backdrop.addEventListener('keydown', (event) => {
            if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); closeMcmConfirm(); return; }
            if (event.key !== 'Tab') return;
            const focusable = mcmConfirmFocusable();
            if (!focusable.length) return;
            const first = focusable[0];
            const last = focusable[focusable.length - 1];
            if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
            else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
        });
        backdrop.addEventListener('mousedown', (event) => { if (event.target === backdrop) closeMcmConfirm(); });
        byId('mcm-confirm-cancel').addEventListener('click', () => closeMcmConfirm());
        byId('mcm-confirm-accept').addEventListener('click', () => {
            const action = mcmConfirmAction;
            closeMcmConfirm();
            if (action) action();
        });
        document.addEventListener('focusin', (event) => {
            if (backdrop.classList.contains('hidden') || backdrop.contains(event.target)) return;
            byId('mcm-confirm-accept').focus();
        });
    }

    window.updateChimMcmState = (payload) => {
        const data = parseMcmPayload(payload);
        if (!data) { mcmStatus('CHIM MCM state payload could not be read.', true); return; }
        const revision = Number(data.revision);
        const previous = mcmState ? Number(mcmState.revision) : NaN;
        if (Number.isFinite(revision) && Number.isFinite(previous) && revision < previous) return;
        mcmState = { revision: revision, entries: Array.isArray(data.entries) ? data.entries : [] };
        mcmStaged.forEach((value, key) => {
            const entry = mcmState.entries.find((candidate) => String(candidate.key || '') === key);
            if (entry && Number(value) === mcmBaseValue(entry)) mcmStaged.delete(key);
        });
        refreshMcmSaveBar();
        const unknown = new Set();
        mcmState.entries.forEach((entry) => {
            const page = normalizeMcmPage(entry.page);
            if (!MCM_PAGES.some((known) => known.id === page)) unknown.add(String(entry.page || 'unknown'));
        });
        renderMcmPanel(activeMcmPage);
        const retained = mcmStaged.size ? ` ${mcmStaged.size} unsaved change${mcmStaged.size === 1 ? '' : 's'} retained.` : '';
        const summary = `${mcmState.entries.length} settings loaded.${retained}`;
        if (unknown.size) mcmStatus(`${summary} Unrecognised page(s) not shown: ${Array.from(unknown).join(', ')}.`, true);
        else mcmStatus(summary, false);
    };
    window.updateChimMcmAgents = (payload) => {
        const data = parseMcmPayload(payload);
        if (!data) { mcmStatus('CHIM MCM agent payload could not be read.', true); return; }
        mcmAgents = {
            active: Array.isArray(data.active) ? data.active : [],
            available: Array.isArray(data.available) ? data.available : [],
            updated_at: String(data.updated_at || '')
        };
        if (activeMcmPage === 'ai_agents') renderMcmPanel('ai_agents');
    };
    window.chimMcmCommandResult = (payload) => {
        const data = parseMcmPayload(payload);
        if (!data) return;
        const request = String(data.request || '');
        const pendingRequest = matchingMcmPendingRequest(request);
        const entry = pendingRequest ? mcmPending.get(pendingRequest) : null;
        if (pendingRequest) mcmPending.delete(pendingRequest);
        const ok = data.ok !== false;
        const message = String(data.message || (ok ? 'Done.' : 'Command failed.'));
        if (entry) {
            if (entry.timeoutId) clearTimeout(entry.timeoutId);
            const button = entry.buttonId ? byId(entry.buttonId) : null;
            setMcmBusy(button, false);
            const result = mcmResultNode(button);
            if (result) { result.textContent = message; result.classList.toggle('error', !ok); }
            if (typeof entry.onResult === 'function') entry.onResult(ok, message);
        }
        mcmStatus(message, !ok);
    };

    function switchPage(page) {
        if (!SETTINGS_PAGES.includes(page)) page = 'globals';
        stopMcmCapture();
        command('input_capture|off');
        closeMcmConfirm(false);
        document.querySelectorAll('.top-tab').forEach((button) => {
            const active = button.dataset.page === page;
            button.classList.toggle('active', active);
            button.setAttribute('aria-selected', active ? 'true' : 'false');
            button.tabIndex = active ? 0 : -1;
        });
        document.querySelectorAll('.page').forEach((element) => { const active = element.id === `${page}-page`; element.hidden = !active; element.classList.toggle('active', active); });
        byId('status-line').hidden = page === 'npcs' || page === 'mcm';
        if (page === 'globals' && !globalData) loadGlobals().catch(showError);
        if (page === 'profiles' && !profileData) loadProfiles().catch(showError);
        if (page === 'npcs' && window.onNpcManagerShown) window.onNpcManagerShown();
        if (page === 'mcm') openMcmPage();
    }
    function showError(error) { status(error.message || String(error), true); }
    function init() {
        enhanceSelect(byId('profile-form').elements.slot);
        initMcm();
        byId('close-button').addEventListener('click', () => { stopMcmCapture(); closeMcmConfirm(false); command('input_capture|off'); command('close'); });
        const topTabs = Array.from(document.querySelectorAll('.top-tab'));
        topTabs.forEach((button) => button.addEventListener('click', () => switchPage(button.dataset.page)));
        document.querySelector('.top-tabs').addEventListener('keydown', (event) => tabListKeydown(event, topTabs));
        byId('globals-form').addEventListener('submit', (event) => saveGlobals(event).catch(showError));
        byId('profile-form').addEventListener('submit', (event) => saveProfile(event).catch(showError));
        byId('new-profile').addEventListener('click', () => createProfile().catch(showError));
        byId('delete-profile').addEventListener('click', () => deleteProfile().catch(showError));
        const updateInputCapture = () => command(document.activeElement && document.activeElement.matches('input, textarea, select') ? 'input_capture|on' : 'input_capture|off');
        document.addEventListener('focusin', updateInputCapture);
        document.addEventListener('focusout', () => queueMicrotask(updateInputCapture));
        document.addEventListener('click', (event) => { if (!event.target.closest('.settings-tile-selector')) closeTileSelectors(); });
        document.addEventListener('keydown', (event) => { if (event.key === 'Escape') { command('input_capture|off'); command('close'); } });
        command('dom_ready');
    }
    window.setConfigManagerServerUrl = (value) => {
        serverBaseUrl = normalizeBaseUrl(value);
        if (window.setNpcManagerServerUrl) window.setNpcManagerServerUrl(value);
        if (!globalData && document.querySelector('.top-tab.active').dataset.page === 'globals') {
            loadGlobals().catch(showError);
        }
    };
    window.setConfigManagerTab = (tab) => switchPage(SETTINGS_PAGES.includes(tab) ? tab : 'globals');
    window.onConfigManagerShown = () => {
        const page = document.querySelector('.top-tab.active').dataset.page;
        if (page === 'profiles') loadProfiles(selectedProfileId).catch(showError);
        else if (page === 'npcs' && window.onNpcManagerShown) window.onNpcManagerShown();
        else if (page === 'mcm') openMcmPage();
        else loadGlobals().catch(showError);
    };
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init); else init();
})();
