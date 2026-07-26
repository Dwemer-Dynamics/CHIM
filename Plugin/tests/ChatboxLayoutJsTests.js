const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const viewRoot = path.resolve(__dirname, '../../AIAgent/PrismaUI/views/CHIM');
const html = fs.readFileSync(path.join(viewRoot, 'chatbox.html'), 'utf8');
const masterMenuHtml = fs.readFileSync(path.join(viewRoot, 'master_menu.html'), 'utf8');
const css = fs.readFileSync(path.join(viewRoot, 'chatbox.css'), 'utf8');
const script = fs.readFileSync(path.join(viewRoot, 'chatbox.js'), 'utf8');
const bridge = fs.readFileSync(path.resolve(__dirname, '../PrismaUIBridge.cpp'), 'utf8');

test('keeps a standalone recent-context viewer available outside the chat modal', () => {
    const viewerStart = html.indexOf('<div id="chim-chatbox-viewer">');
    const contextStart = html.indexOf('<section id="chim-chatbox"');
    const contextEnd = html.indexOf('</section>', contextStart);
    const modalStart = html.indexOf('<div id="focus-chatbox-modal"');

    assert.notEqual(viewerStart, -1);
    assert.notEqual(contextStart, -1);
    assert.notEqual(contextEnd, -1);
    assert.notEqual(modalStart, -1);
    assert.ok(viewerStart < contextStart);
    assert.ok(contextEnd < modalStart);
    assert.match(html, /data-chim-menu-scale-target="#chim-chatbox-viewer, \.focus-chatbox-shell"/);
    assert.match(html, /placeholder="Enter message here, press Enter to send"/);
});

test('moves recent context between the bottom-left viewer and focused chat shell', () => {
    const viewerRule = css.match(/#chim-chatbox-viewer\s*\{([\s\S]*?)\}/);

    assert.ok(viewerRule);
    assert.match(viewerRule[1], /position:\s*fixed/);
    assert.match(viewerRule[1], /bottom:\s*20px/);
    assert.match(viewerRule[1], /left:\s*20px/);
    assert.match(css, /\.focus-chatbox-shell > #chim-chatbox\s*\{[\s\S]*?position:\s*absolute/);
    assert.match(script, /function setContextPlacement\(focused\)/);
    assert.match(script, /destination\.appendChild\(contextPanelElement\)/);
    assert.match(script, /window\.closeFocusChatbox[\s\S]*?setContextPlacement\(false\)/);
});

test('retains independent top, center, and bottom chat modal anchors', () => {
    assert.match(css, /\.focus-chatbox-modal\.focus-position-center\s*\{[\s\S]*?align-items:\s*center/);
    assert.match(css, /\.focus-chatbox-modal\.focus-position-top\s*\{[\s\S]*?align-items:\s*flex-start/);
    assert.match(css, /\.focus-chatbox-modal\.focus-position-bottom\s*\{[\s\S]*?align-items:\s*flex-end/);
    assert.match(css, /\.focus-chatbox-modal\.focus-position-bottom \.focus-chatbox-shell > #chim-chatbox\s*\{[\s\S]*?bottom:\s*calc\(100% \+ 10px\)/);
});

test('prefetches recent context while the warm-loaded chatbox view is hidden', () => {
    assert.match(bridge, /static void FetchAndUpdateChatboxStory\(bool replaceExisting\)[\s\S]*?\(!replaceExisting && g_chatboxState\.load\(\) == 0\)/);
    assert.match(bridge, /static void OnChatboxDomReady[\s\S]*?FetchAndUpdateChatboxStory\(true\);/);
});

test('opens standalone and focused recent context at the newest entry', () => {
    assert.match(script, /function scrollStoryToBottomAfterLayout\(\)[\s\S]*?defer\(scrollStoryToBottom\)/);
    assert.match(script, /window\.openFocusChatbox[\s\S]*?scrollStoryToBottomAfterLayout\(\)/);
    assert.match(script, /window\.onChatboxShown = function\(\)[\s\S]*?scrollStoryToBottomAfterLayout\(\)/);
    assert.match(bridge, /void ShowChatboxPanel\(\)[\s\S]*?window\.onChatboxShown && window\.onChatboxShown\(\)/);
});

test('labels the standalone panel as Context Window in the Prisma menu', () => {
    assert.match(masterMenuHtml, /onclick="selectPanel\('chatbox'\)"[\s\S]*?>Context Window<\/button>/);
    assert.match(masterMenuHtml, />Context Window<\/div>/);
    assert.doesNotMatch(masterMenuHtml, />Chatbox View<\/(?:button|div)>/);
});
