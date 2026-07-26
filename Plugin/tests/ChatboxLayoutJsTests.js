const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const viewRoot = path.resolve(__dirname, '../../AIAgent/PrismaUI/views/CHIM');
const html = fs.readFileSync(path.join(viewRoot, 'chatbox.html'), 'utf8');
const css = fs.readFileSync(path.join(viewRoot, 'chatbox.css'), 'utf8');
const script = fs.readFileSync(path.join(viewRoot, 'chatbox.js'), 'utf8');

test('keeps recent context outside the independently positioned chat modal', () => {
    const contextStart = html.indexOf('<section id="chim-chatbox"');
    const contextEnd = html.indexOf('</section>', contextStart);
    const modalStart = html.indexOf('<div id="focus-chatbox-modal"');

    assert.notEqual(contextStart, -1);
    assert.notEqual(contextEnd, -1);
    assert.notEqual(modalStart, -1);
    assert.ok(contextEnd < modalStart);
    assert.match(html, /data-chim-menu-scale-target="#chim-chatbox, \.focus-chatbox-shell"/);
});

test('anchors recent context at bottom-left without layout-manager overrides', () => {
    const rootRule = css.match(/#chim-chatbox\s*\{([\s\S]*?)\}/);

    assert.ok(rootRule);
    assert.match(rootRule[1], /position:\s*fixed/);
    assert.match(rootRule[1], /bottom:\s*20px/);
    assert.match(rootRule[1], /left:\s*20px/);
    assert.doesNotMatch(script, /chimLayout\.apply\(chatboxRoot/);
});

test('retains independent top, center, and bottom chat modal anchors', () => {
    assert.match(css, /\.focus-chatbox-modal\.focus-position-center\s*\{[\s\S]*?align-items:\s*center/);
    assert.match(css, /\.focus-chatbox-modal\.focus-position-top\s*\{[\s\S]*?align-items:\s*flex-start/);
    assert.match(css, /\.focus-chatbox-modal\.focus-position-bottom\s*\{[\s\S]*?align-items:\s*flex-end/);
    assert.doesNotMatch(css, /\.focus-chatbox-modal\.focus-position-bottom \.focus-chatbox-shell/);
});
