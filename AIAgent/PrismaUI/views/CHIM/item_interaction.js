'use strict';
let interactionId = '', submitted = false, items = [], magic = [], selectedKey = null, acknowledgementTimer = null, nativeSignature = '';
const element = id => document.getElementById(id);
const selectedItem = () => items.find(item => item.key === selectedKey) || null;
function status(message, state) {
    element('status').textContent = message;
    element('status').setAttribute('data-state', state || '');
}
function send(value) {
    if (typeof window.chimItemInteraction !== 'function') {
        status('CHIM is not ready. Close this menu and cast Interact again.', 'error');
        return false;
    }
    try {
        window.chimItemInteraction(typeof value === 'string' ? value : JSON.stringify(Object.assign({id: interactionId}, value)));
        return true;
    } catch (error) {
        status('CHIM could not receive the interaction. Your description has been kept.', 'error');
        return false;
    }
}
function setBusy(busy) {
    submitted = busy;
    Array.prototype.forEach.call(element('interaction').querySelectorAll('input,textarea,select,button'), field => {
        field.disabled = busy;
    });
    element('quantity').disabled = busy || !selectedItem();
    element('interaction').setAttribute('aria-busy', busy ? 'true' : 'false');
}
const validMagic = list => (Array.isArray(list) ? list : []).filter(entry => entry && Number.isInteger(entry.key) && typeof entry.name === 'string');
// Known spells, powers and shouts come from native; names are untrusted, so only textContent is used.
// keepSelection retains the chosen key when a later same-request update still offers it.
function setMagic(list, keepSelection) {
    const previous = keepSelection ? selectedMagicKey() : null;
    magic = validMagic(list);
    const select = element('magic');
    select.textContent = '';
    const none = document.createElement('option');
    none.value = ''; none.textContent = 'No magic'; select.appendChild(none);
    const groups = new Map();
    magic.forEach(entry => {
        const kind = typeof entry.kind === 'string' ? entry.kind : '';
        let parent = select;
        if (kind) {
            if (!groups.has(kind)) {
                const group = document.createElement('optgroup');
                group.label = kind; select.appendChild(group); groups.set(kind, group);
            }
            parent = groups.get(kind);
        }
        const option = document.createElement('option');
        option.value = String(entry.key); option.textContent = entry.name;
        if (entry.details) option.title = String(entry.details);
        parent.appendChild(option);
    });
    select.value = previous !== null && magic.some(entry => entry.key === previous) ? String(previous) : '';
}
function selectedMagicKey() {
    const value = element('magic').value;
    if (value === '') return null;
    const key = Number(value);
    return magic.some(entry => entry.key === key) ? key : null;
}
function showPicker(open) {
    element('picker').hidden = !open;
    element('choose').setAttribute('aria-expanded', open ? 'true' : 'false');
    if (open) { filterItems(); element('search').focus(); }
    else if (!submitted) element('intent').focus();
}
function chooseItem(key) {
    selectedKey = key;
    const item = selectedItem();
    element('selected-item').textContent = item ? item.name : 'No item';
    element('selected-item').title = item ? (item.details || '') : '';
    element('clear-item').hidden = !item;
    element('amount-field').hidden = !item || item.count <= 1;
    element('quantity').max = item ? Math.min(100, item.count) : 1;
    element('quantity').value = 1;
    element('quantity').disabled = submitted || !item;
    showPicker(false);
}
function filterItems() {
    const term = element('search').value.toLocaleLowerCase();
    const list = element('items');
    list.innerHTML = '';
    function addRow(item) {
        const row = document.createElement('button');
        row.type = 'button'; row.className = 'inventory-row';
        row.setAttribute('aria-pressed', (item ? item.key === selectedKey : selectedKey === null) ? 'true' : 'false');
        const name = document.createElement('span');
        name.textContent = item ? item.name : 'No item'; row.appendChild(name);
        if (item) {
            const details = document.createElement('small');
            details.textContent = item.count + ' available' + (item.details ? ' · ' + item.details : '');
            row.appendChild(details);
        }
        row.onclick = () => { if (!submitted) chooseItem(item ? item.key : null); };
        list.appendChild(row);
    }
    addRow(null);
    const matches = items.filter(item => (item.name + ' ' + (item.details || '')).toLocaleLowerCase().includes(term));
    matches.forEach(addRow);
    if (!matches.length) {
        const empty = document.createElement('p'); empty.className = 'empty-inventory';
        empty.textContent = items.length ? 'No matching items.' : 'No inventory items.'; list.appendChild(empty);
    }
}
window.setInteraction = data => {
    // Native merges updates into one payload, so a delayed magic list repeats the last state and status.
    if (data.id !== interactionId && data.preserveDraft) return;
    const signature = JSON.stringify([data.state || '', data.status || '', !!data.confirm]);
    if (data.id === interactionId && Array.isArray(data.magic) && JSON.stringify(validMagic(data.magic)) !== JSON.stringify(magic)) {
        setMagic(data.magic, true);
        // A magic-only refresh is not an acknowledgement; leave status, busy state and its timer alone.
        if (signature === nativeSignature) return;
    }
    nativeSignature = signature;
    clearTimeout(acknowledgementTimer);
    if (data.id !== interactionId) {
        interactionId = data.id;
        if (!data.preserveDraft) {
            items = data.items || [];
            element('target').textContent = data.target;
            element('intent').value = '';
            element('search').value = '';
            status('');
            setMagic(data.magic);
            chooseItem(null);
        }
        setBusy(false);
        element('intent').focus();
    }
    if (data.state === 'error') setBusy(false);
    if (data.state === 'busy') { setBusy(true); showPicker(false); }
    if (data.state === 'busy') status('Calculating Result...', 'busy');
    else if (data.status) status(data.status, data.state);
};
function cancel() {
    clearTimeout(acknowledgementTimer);
    send('input_capture|off');
    send({op: 'cancel'});
}
function submitInteraction(event) {
    if (event) event.preventDefault();
    if (submitted) return;
    const item = selectedItem();
    const quantity = item ? Number(element('quantity').value) : 0;
    const intent = element('intent').value.trim();
    if (item && (!Number.isInteger(quantity) || quantity < 1 || quantity > Math.min(100, item.count))) {
        status('Choose a whole amount within the available quantity.', 'error'); element('quantity').focus(); return;
    }
    if (!intent || intent.length > 1000) {
        status(!intent ? 'Describe an action first.' : 'Use up to 1000 characters.', 'error'); element('intent').focus(); return;
    }
    // Use the explicit Prisma bridge; embedded form-validation APIs are not required.
    const payload = {op: 'submit', key: item ? Number(item.key) : null, quantity: quantity, magic_key: selectedMagicKey(), intent: intent};
    showPicker(false); status('Calculating Result...', 'busy'); setBusy(true);
    send('input_capture|off');
    if (!send(payload)) { setBusy(false); element('intent').focus(); return; }
    acknowledgementTimer = setTimeout(() => {
        status('CHIM has not acknowledged this interaction. Close and reopen before trying again.', 'error');
    }, 5000);
}
element('close').onclick = cancel;
element('choose').onclick = () => showPicker(element('picker').hidden);
element('clear-item').onclick = () => chooseItem(null);
element('search').oninput = filterItems;
element('submit').onclick = submitInteraction;
element('interaction').onsubmit = event => event.preventDefault();
document.addEventListener('focusin', event => {
    if (event.target.matches('input,textarea,select')) send('input_capture|on');
});
document.addEventListener('focusout', () => setTimeout(() => {
    if (!document.activeElement || !document.activeElement.matches('input,textarea,select')) send('input_capture|off');
}, 0));
document.addEventListener('keydown', event => {
    if (event.isComposing || event.keyCode === 229) return;
    if (event.key === 'Escape') {
        event.preventDefault();
        if (!element('picker').hidden) showPicker(false);
        else cancel();
    }
    if (event.key === 'Enter') {
        if (event.target === element('intent') && !event.shiftKey) submitInteraction(event);
        else if (event.target === element('search') || event.target === element('quantity') || event.target === element('magic')) event.preventDefault();
    }
    if (event.key === 'Tab') {
        const fields = Array.prototype.filter.call(document.querySelectorAll('button,input,textarea,select'), field => !field.disabled && field.offsetParent !== null);
        const first = fields[0], last = fields[fields.length - 1];
        if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
        else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
    }
});
