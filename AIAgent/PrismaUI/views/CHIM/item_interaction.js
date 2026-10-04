'use strict';
let interactionId = '', submitted = false, items = [];
const element = id => document.getElementById(id);
const send = value => window.chimItemInteraction?.(typeof value === 'string' ? value : JSON.stringify({id: interactionId, ...value}));
function updateQuantity() {
  element('quantity').max = Math.min(100, Number(element('items').selectedOptions[0]?.dataset.count || 1));
  element('quantity').value = 1;
}
function filterItems() {
  const term = element('search').value.toLocaleLowerCase();
  element('items').replaceChildren(...items.filter(item => item.name.toLocaleLowerCase().includes(term)).map(item => {
    const option = document.createElement('option'); option.value = item.key;
    option.textContent = `${item.name} (${item.count})${item.details ? " — " + item.details : ""}`; option.dataset.count = item.count; return option;
  }));
  updateQuantity();
  element('submit').disabled = submitted || element('items').options.length === 0;
  if (!items.length) element('status').textContent = 'No eligible inventory items.';
}
window.setInteraction = data => {
  if (data.id !== interactionId) {
    interactionId = data.id; submitted = false; items = data.items || [];
    element('target').textContent = data.target;
    element('intent').value = ''; element('search').value = ''; element('quantity').value = 1;
    element('approve').disabled = false; element('status').textContent = '';
    for (const field of element('interaction').elements) field.disabled = false;
    filterItems(); element('search').focus();
  }
  element('confirmation').hidden = !data.confirm;
  if (data.status) element('status').textContent = data.status;
  if (data.confirm) element('approve').focus();
};
function cancel() { send('input_capture|off'); send({op: 'cancel'}); }
element('close').onclick = cancel; element('decline').onclick = cancel;
element('approve').onclick = () => { element('approve').disabled = true; send({op: 'approve'}); };
element('items').onchange = updateQuantity;
element('search').oninput = filterItems;
element('interaction').onsubmit = event => {
  event.preventDefault(); if (submitted || !element('interaction').reportValidity() || !element('intent').value.trim()) return;
  const payload = {op: 'submit', key: Number(element('items').value), quantity: Number(element('quantity').value), intent: element('intent').value.trim()};
  submitted = true;
  for (const field of element('interaction').elements) field.disabled = true;
  send('input_capture|off'); send(payload);
};
document.addEventListener('focusin', event => { if (event.target.matches('input,textarea,select')) send('input_capture|on'); });
document.addEventListener('focusout', () => queueMicrotask(() => { if (!document.activeElement?.matches('input,textarea,select')) send('input_capture|off'); }));
document.addEventListener('keydown', event => {
  if (event.key === 'Escape') { event.preventDefault(); cancel(); }
  if (event.key === 'Tab') {
    const fields = [...document.querySelectorAll('button,input,textarea,select')].filter(field => !field.disabled && field.offsetParent !== null);
    const first = fields[0], last = fields[fields.length - 1];
    if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
    else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
  }
});
