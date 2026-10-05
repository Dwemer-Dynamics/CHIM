'use strict';
let interactionId = '', submitted = false, items = [], acknowledgementTimer = null;
const element = id => document.getElementById(id);
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
    Array.prototype.forEach.call(element('interaction').querySelectorAll('input,select,textarea,button'), field => {
        field.disabled = busy;
    });
    element('submit').disabled = busy || element('items').options.length === 0;
    element('quantity').disabled = busy || element('items').value === '';
    element('interaction').setAttribute('aria-busy', busy ? 'true' : 'false');
}
function updateQuantity() {
    const select = element('items');
    const option = select.options[select.selectedIndex];
    element('quantity').max = Math.min(100, Number(option ? option.getAttribute('data-count') : 1));
    element('quantity').value = 1;
    element('quantity').disabled = submitted || !option || option.value === '';
}
function filterItems() {
    const term = element('search').value.toLocaleLowerCase();
    element('items').innerHTML = '';
    const none = document.createElement('option');
    none.value = '';
    none.textContent = 'No item';
    none.setAttribute('data-count', 1);
    element('items').appendChild(none);
    items.filter(item => item.name.toLocaleLowerCase().includes(term)).forEach(item => {
        const option = document.createElement('option');
        option.value = item.key;
        option.textContent = item.name + ' (' + item.count + ')' + (item.details ? ' - ' + item.details : '');
        option.setAttribute('data-count', item.count);
        element('items').appendChild(option);
    });
    updateQuantity();
    element('submit').disabled = submitted || element('items').options.length === 0;
}
window.setInteraction = data => {
    clearTimeout(acknowledgementTimer);
    if (data.id !== interactionId) {
        interactionId = data.id;
        if (!data.preserveDraft) {
            items = data.items || [];
            element('target').textContent = data.target;
            element('intent').value = '';
            element('search').value = '';
            status('');
            filterItems();
        }
        element('approve').disabled = false;
        setBusy(false);
        if (!data.preserveDraft) element('search').focus();
    }
    if (data.state === 'error') setBusy(false);
    if (data.state === 'busy') setBusy(true);
    element('confirmation').hidden = !data.confirm;
    if (data.status) status(data.status, data.state);
    if (data.confirm) element('approve').focus();
};
function cancel() {
    clearTimeout(acknowledgementTimer);
    send('input_capture|off');
    send({op: 'cancel'});
}
function submitInteraction(event) {
    if (event) event.preventDefault();
    if (submitted) return;
    const select = element('items');
    const option = select.options[select.selectedIndex];
    const hasItem = option && option.value !== '';
    const quantity = hasItem ? Number(element('quantity').value) : 0;
    const intent = element('intent').value.trim();
    if (!option) { status('Choose an inventory item.', 'error'); select.focus(); return; }
    if (hasItem && (!Number.isInteger(quantity) || quantity < 1 || quantity > Math.min(100, Number(option.getAttribute('data-count'))))) {
        status('Choose a whole quantity within the available amount.', 'error'); element('quantity').focus(); return;
    }
    if (!intent || intent.length > 1000) {
        status('Describe what you try to do, using up to 1000 characters.', 'error'); element('intent').focus(); return;
    }
    // Prisma menus use explicit click handlers; do not depend on embedded-browser form validation APIs.
    const payload = {op: 'submit', key: hasItem ? Number(option.value) : null, quantity: quantity, intent: intent};
    status('Sending interaction...', 'busy');
    setBusy(true);
    send('input_capture|off');
    if (!send(payload)) { setBusy(false); return; }
    acknowledgementTimer = setTimeout(() => {
        status('CHIM has not acknowledged this interaction. Close and reopen the menu before trying again. Your description is still here.', 'error');
    }, 5000);
}
element('close').onclick = cancel;
element('decline').onclick = cancel;
element('approve').onclick = () => {
    if (send({op: 'approve'})) { element('approve').disabled = true; status('Playing the interaction...', 'busy'); }
};
element('items').onchange = updateQuantity;
element('search').oninput = filterItems;
element('submit').onclick = submitInteraction;
element('interaction').onsubmit = submitInteraction;
document.addEventListener('focusin', event => {
    if (event.target.matches('input,textarea,select')) send('input_capture|on');
});
document.addEventListener('focusout', () => setTimeout(() => {
    if (!document.activeElement || !document.activeElement.matches('input,textarea,select')) send('input_capture|off');
}, 0));
document.addEventListener('keydown', event => {
    if (event.key === 'Escape') { event.preventDefault(); cancel(); }
    if (event.key === 'Tab') {
        const fields = Array.prototype.filter.call(document.querySelectorAll('button,input,textarea,select'), field => !field.disabled && field.offsetParent !== null);
        const first = fields[0], last = fields[fields.length - 1];
        if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
        else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
    }
});
