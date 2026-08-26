const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const viewRoot = path.resolve(__dirname, '../../AIAgent/PrismaUI/views/CHIM');
const html = fs.readFileSync(path.join(viewRoot, 'chatbox.html'), 'utf8');
const masterMenuHtml = fs.readFileSync(path.join(viewRoot, 'master_menu.html'), 'utf8');
const masterMenuScript = fs.readFileSync(path.join(viewRoot, 'master_menu.js'), 'utf8');
const css = fs.readFileSync(path.join(viewRoot, 'chatbox.css'), 'utf8');
const script = fs.readFileSync(path.join(viewRoot, 'chatbox.js'), 'utf8');
const overlayScript = fs.readFileSync(path.join(viewRoot, 'overlay.js'), 'utf8');
const overlayCss = fs.readFileSync(path.join(viewRoot, 'overlay.css'), 'utf8');
const bridge = fs.readFileSync(path.resolve(__dirname, '../PrismaUIBridge.cpp'), 'utf8');
const conversationRouter = fs.readFileSync(path.resolve(__dirname, '../PlayerConversationRouter.h'), 'utf8');
const httpManager = fs.readFileSync(path.resolve(__dirname, '../HTTPManager.cpp'), 'utf8');

// Evaluate the small pure helper straight out of overlay.js so the mapping itself is tested.
function loadOverlayFunction(name) {
    const marker = 'function ' + name + '(';
    const start = overlayScript.indexOf(marker);
    assert.notEqual(start, -1, name + ' not found in overlay.js');
    const end = overlayScript.indexOf('\n    }', start);
    assert.notEqual(end, -1, name + ' body not delimited in overlay.js');
    const source = overlayScript.slice(start, end + '\n    }'.length);
    return new Function(source + '\nreturn ' + name + ';')();
}

function loadChatboxFunction(name) {
    const marker = 'function ' + name + '(';
    const start = script.indexOf(marker);
    assert.notEqual(start, -1, name + ' not found in chatbox.js');
    const end = script.indexOf('\n    }', start);
    assert.notEqual(end, -1, name + ' body not delimited in chatbox.js');
    const source = script.slice(start, end + '\n    }'.length);
    return new Function(source + '\nreturn ' + name + ';')();
}

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

test('keeps Context Window passive and relays scrolling through the focused master menu', () => {
    assert.match(bridge, /cmd == "chatbox"[\s\S]*?ToggleChatboxPanel\(\)/);
    assert.doesNotMatch(bridge, /FocusContextWindowPanel/);
    assert.doesNotMatch(bridge, /g_chatboxContextFocusActive/);
    assert.match(bridge, /kContextScrollPrefix = "context_scroll\|"/);
    assert.match(bridge, /window\.scrollStandaloneContext && window\.scrollStandaloneContext/);
    assert.match(script, /window\.scrollStandaloneContext = function\(deltaY\)/);
    assert.match(masterMenuScript, /window\.setContextWindowVisible = function\(visible\)/);
    assert.match(masterMenuScript, /function handleContextWindowWheel\(event\)/);
    assert.match(masterMenuScript, /window\.chimMasterMenuCommand\('context_scroll\|' \+ delta\)/);
    assert.doesNotMatch(script, /isContextWindowFocused/);
    assert.match(bridge, /void ShowChatboxPanel\(\)[\s\S]*?No auto-focus - player retains control/);
});

test('keeps standalone context expanded and reserves collapse for focused chat', () => {
    assert.match(css, /#chim-chatbox-viewer > #chim-chatbox \.focus-chatbox-context-toggle\s*\{[\s\S]*?display:\s*none/);
    assert.match(script, /function setContextPlacement\(focused\)[\s\S]*?if \(focused\)[\s\S]*?applyContextCollapsed\(loadContextCollapsed\(\)\)[\s\S]*?contextPanelElement\.classList\.remove\('collapsed'\)/);
    assert.match(html, /<div class="chatbox-control-label">LLM Model<\/div>/);
    assert.doesNotMatch(html, /<div class="chatbox-control-label">Global Model<\/div>/);
});

test('keeps one persistent CHIM mode authority for the chatbox and the overlay badge', () => {
    assert.match(bridge, /static void PushCurrentModeToViews\(\)[\s\S]*?UpdateChatboxModeUI\(mode\);[\s\S]*?UpdateOverlayModeUI\(mode\);/);
    assert.match(bridge, /static void OnOverlayDomReady[\s\S]*?PushCurrentModeToViews\(\);/);
    assert.match(bridge, /static void OnChatboxDomReady[\s\S]*?PushCurrentModeToViews\(\);/);
    // Periodic server payloads no longer repaint the persistent mode.
    assert.match(overlayScript, /window\.updateOverlayMode = function\(mode\)[\s\S]*?pluginModeApplied = true/);
    assert.match(overlayScript, /if \(!pluginModeApplied\) \{[\s\S]*?updateMode\(overlay\.mode\);/);
    // Overlay label and colour parity with the chatbox selector.
    ['CLOSE', 'SHOUT', 'NARRATOR', 'INJECTION_CHAT'].forEach((mode) => {
        assert.ok(overlayScript.includes("'" + mode + "': { label:"), mode);
    });
    ['close', 'shout', 'narrator'].forEach((cls) => {
        assert.ok(overlayCss.includes('.mode-badge.' + cls + ' {'), cls);
    });
});

test('colors the overlay listener status semantically without changing its text', () => {
    const classify = loadOverlayFunction('classifyListenerStatus');

    assert.equal(classify('Crosshair: Can hear you'), 'hearing-ok');
    assert.equal(classify('Nearest: Can hear you: around a wall'), 'hearing-ok');
    assert.equal(classify('Crosshair: Can hear you: 3m above you'), 'hearing-ok');

    assert.equal(classify("Crosshair: Can't hear you clearly"), 'hearing-partial');
    assert.equal(classify('Nearest: Can hear you, muffled by door'), 'hearing-partial');

    assert.equal(classify('Crosshair: Too far away'), 'hearing-blocked');
    assert.equal(classify("Nearest: Can't hear you"), 'hearing-blocked');
    assert.equal(classify("Crosshair: Can't hear you: closed door"), 'hearing-blocked');

    assert.equal(classify('No target'), '');
    assert.equal(classify('Crosshair'), '');
    assert.equal(classify('Nearest'), '');
    assert.equal(classify('Crosshair: In combat'), '');
    assert.equal(classify(''), '');
    assert.equal(classify(null), '');

    assert.match(overlayScript, /const statusClass = toneClass \? `target-status \$\{toneClass\}` : 'target-status'/);
    ['hearing-ok', 'hearing-partial', 'hearing-blocked'].forEach((cls) => {
        assert.ok(overlayCss.includes('.target-status.' + cls + ' {'), cls);
    });
});

test('accepts every Delete Events count the selector offers', () => {
    const selectMatch = html.match(/<select id="chatbox-delete-events-select"[\s\S]*?<\/select>/);
    assert.ok(selectMatch, 'delete events select not found in chatbox.html');
    const offered = [...selectMatch[0].matchAll(/<option value="(\d+)"/g)].map((m) => Number(m[1]));
    assert.deepEqual(offered, [5, 10, 20, 50, 100]);

    const normalize = loadChatboxFunction('normalizeDeleteEventCount');
    offered.forEach((count) => {
        assert.equal(normalize(count), count, 'numeric ' + count);
        assert.equal(normalize(String(count)), count, 'select value "' + count + '"');
    });

    // Anything outside the selector is refused instead of deleting a different amount.
    [0, 1, 7, 15, 99, 200, -5, NaN, null, undefined, '', 'all'].forEach((bad) => {
        assert.equal(normalize(bad), 0, String(bad));
    });

    // Click handling and the request path share the single validator.
    assert.doesNotMatch(script, /\[20, 50, 100\]/);
    assert.equal(script.match(/normalizeDeleteEventCount\(/g).length, 3);
});

test('requires two presses on Delete Events and asks "Are you sure?" in between', () => {
    assert.match(script, /function armDeleteConfirmation\(deleteCount\)[\s\S]*?deleteEventConfirmButton\.textContent = 'Are you sure\?'/);
    assert.doesNotMatch(script, /Confirm Delete/);

    // The second press only fires when it confirms the same selected count.
    assert.match(script, /if \(pendingDeleteCount === deleteCount\) \{[\s\S]*?window\.deleteRecentEvents\(deleteCount\);[\s\S]*?armDeleteConfirmation\(deleteCount\);/);
});

test('never leaves the Delete Events button stuck on the confirmation prompt', () => {
    assert.match(script, /function clearPendingDeleteConfirmation\(\)[\s\S]*?pendingDeleteCount = 0;[\s\S]*?window\.clearTimeout\(pendingDeleteConfirmTimeoutId\)[\s\S]*?deleteEventConfirmButton\.textContent = 'Delete'/);
    // Expiry, count changes, and every busy/error/success exit reset the label.
    assert.match(script, /pendingDeleteConfirmTimeoutId = window\.setTimeout\(function\(\) \{\s*clearPendingDeleteConfirmation\(\);\s*\}, \d+\)/);
    assert.match(script, /deleteEventSelect\.addEventListener\('change', function\(\) \{\s*clearPendingDeleteConfirmation\(\);/);
    assert.match(script, /\} finally \{\s*setDeleteEventControlsBusy\(false\);\s*clearPendingDeleteConfirmation\(\);/);

    // It stays a real focusable button, with the tooltip matching whichever state it shows.
    assert.match(html, /<button id="chatbox-delete-events-confirm"[^>]*type="button"[^>]*title="Delete the selected number of recent events"[^>]*>Delete<\/button>/);
    assert.match(script, /textContent = 'Are you sure\?';\s*deleteEventConfirmButton\.title = 'Press again to delete the selected events'/);
    assert.match(script, /textContent = 'Delete';\s*deleteEventConfirmButton\.title = 'Delete the selected number of recent events'/);
});

test('offers a compact one-shot player mood selector with no mood as the default', () => {
    const moodPicker = html.match(/<fieldset class="focus-chatbox-mood-picker">([\s\S]*?)<\/fieldset>/);
    assert.ok(moodPicker, 'mood picker not found');

    const values = [...moodPicker[1].matchAll(/name="chatbox-player-mood" value="([^"]*)"/g)]
        .map((match) => match[1]);
    assert.deepEqual(values, ['', 'happy', 'sad', 'angry', 'annoyed', 'scared', 'surprised', 'confused', 'suspicious', 'playful', 'flirty']);
    assert.match(moodPicker[1], /value="" aria-label="No mood" checked/);
    assert.match(css, /\.focus-chatbox-mood-input:checked \+ \.focus-chatbox-mood-option/);
    assert.match(css, /\.focus-chatbox-mood-input:focus-visible \+ \.focus-chatbox-mood-option/);

    const normalizeMood = loadChatboxFunction('normalizePlayerMood');
    values.slice(1).forEach((mood) => assert.equal(normalizeMood(mood), mood));
    ['', 'neutral', 'mood=happy', null, undefined].forEach((mood) => assert.equal(normalizeMood(mood), ''));

    assert.match(script, /window\.openFocusChatbox[\s\S]*?resetPlayerMood\(\)/);
    assert.match(script, /window\.closeFocusChatbox[\s\S]*?resetPlayerMood\(\)/);
    assert.match(script, /window\.clearFocusMessage[\s\S]*?resetPlayerMood\(\)/);
});

test('sends validated mood metadata and mirrors the default mood phrasing in the live story row', () => {
    assert.match(script, /mood \? 'send_mood\|' \+ mood \+ '\|' \+ message : 'send\|' \+ message/);
    assert.match(bridge, /cmd\.starts_with\("send_mood\|"\)[\s\S]*?SendChatboxMessage\(message, playerMood\)/);

    // The optimistic row reads as prose, matching the server's default suffix so a refresh dedupes it.
    const moodSuffixes = {
        happy: '(speaks in a happy tone.)',
        sad: '(speaks in a sad tone.)',
        angry: '(speaks in an angry tone.)',
        annoyed: '(speaks in an annoyed tone.)',
        scared: '(speaks in a frightened tone.)',
        surprised: '(speaks in a surprised tone.)',
        confused: '(speaks in a confused tone.)',
        suspicious: '(speaks in a suspicious tone.)',
        playful: '(speaks in a playful tone.)',
        flirty: '(speaks in a flirtatious tone.)'
    };
    Object.keys(moodSuffixes).forEach((mood) => {
        assert.ok(
            bridge.includes(`if (playerMood == "${mood}") return "${moodSuffixes[mood]}";`),
            `default mood phrasing missing for ${mood}`
        );
    });

    // Every selectable mood needs phrasing, and no mood must stay untouched.
    const offeredMoods = [...html.matchAll(/name="chatbox-player-mood" value="([^"]*)"/g)]
        .map((match) => match[1])
        .filter(Boolean);
    offeredMoods.forEach((mood) => assert.ok(moodSuffixes[mood], `no default phrasing for ${mood}`));
    assert.match(bridge, /static std::string_view DefaultPlayerMoodSuffix\(const std::string& playerMood\)[\s\S]*?return \{\};/);
    assert.match(bridge, /const std::string_view moodSuffix = DefaultPlayerMoodSuffix\(playerMood\);\s*if \(!moodSuffix\.empty\(\)\) \{[\s\S]*?displayMessage \+= moodSuffix;/);
    assert.ok(!bridge.includes('[mood: '), 'raw mood tag must not reach the story row');
    assert.match(bridge, /PushChatboxMessage\(playerName, displayMessage, "", "player"\)/);
    assert.match(bridge, /const bool supportedMood =[\s\S]*?playerMood == "annoyed"[\s\S]*?playerMood == "surprised"[\s\S]*?playerMood == "confused"[\s\S]*?playerMood == "suspicious"[\s\S]*?playerMood == "playful"[\s\S]*?playerMood == "flirty";/);
    assert.match(bridge, /routingContext\.playerMood = playerMood/);
    assert.match(conversationRouter, /std::string playerMood;/);
    assert.match(httpManager, /audienceSnapshot\["player_mood"\] = routingContext->playerMood/);
    assert.match(httpManager, /requestModeSnapshot\["player_mood"\] = routingContext->playerMood/);
});
