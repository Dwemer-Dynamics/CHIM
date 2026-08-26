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

test('offers a compact player mood selector with no mood as the default', () => {
    const moodPicker = html.match(/<fieldset class="focus-chatbox-mood-picker">([\s\S]*?)<\/fieldset>/);
    assert.ok(moodPicker, 'mood picker not found');

    const values = [...moodPicker[1].matchAll(/name="chatbox-player-mood" value="([^"]*)"/g)]
        .map((match) => match[1]);
    assert.deepEqual(values, ['', 'happy', 'sad', 'angry', 'annoyed', 'scared', 'surprised', 'confused', 'suspicious', 'playful', 'flirty', 'custom']);
    assert.match(moodPicker[1], /value="" aria-label="No mood" checked/);
    assert.match(css, /\.focus-chatbox-mood-input:checked \+ \.focus-chatbox-mood-option/);
    assert.match(css, /\.focus-chatbox-mood-input:focus-visible \+ \.focus-chatbox-mood-option/);

    // Custom carries its own free text, so it must not widen the validated predefined roster.
    const normalizeMood = loadChatboxFunction('normalizePlayerMood');
    values.slice(1, -1).forEach((mood) => assert.equal(normalizeMood(mood), mood));
    ['', 'custom', 'neutral', 'mood=happy', null, undefined].forEach((mood) => assert.equal(normalizeMood(mood), ''));

    // No mood is the fallback for an empty store, not a reset applied on every open.
    assert.match(script, /function normalizeStoredPlayerMood\(value\)[\s\S]*?return stored === customPlayerMoodValue \? customPlayerMoodValue : normalizePlayerMood\(stored\)/);
    assert.doesNotMatch(script, /resetPlayerMood/);
});

test('keeps every mood icon-only, with Custom last and its field beside the pencil', () => {
    const moodPicker = html.match(/<fieldset class="focus-chatbox-mood-picker">([\s\S]*?)<\/fieldset>/);
    assert.ok(moodPicker, 'mood picker not found');

    // Each visible option is character entities only; no mood spells itself out in the compact row.
    const labels = [...moodPicker[1].matchAll(
        /<label class="focus-chatbox-mood-option" for="chatbox-mood-([a-z]+)" title="([^"]+)">((?:&#[0-9A-Fa-fx]+;)+)<\/label>/g
    )].map((match) => ({ id: match[1], title: match[2], icon: match[3] }));
    assert.equal(labels.length, 12, 'every mood radio needs an icon-only label');
    assert.equal(labels[labels.length - 2].id, 'flirty');
    assert.equal(labels[labels.length - 1].id, 'custom', 'Custom belongs directly after Flirty');
    assert.equal(labels[labels.length - 1].icon, '&#x270F;&#xFE0F;', 'Custom uses the pencil icon');
    assert.equal(labels[labels.length - 1].title, 'Custom mood');
    assert.match(moodPicker[1], /id="chatbox-mood-custom"[^>]*value="custom" aria-label="Custom mood"/);

    // The free-text field sits immediately to the right of the pencil, inside the same wrapping row.
    const customRadioIndex = moodPicker[1].indexOf('id="chatbox-mood-custom"');
    const customLabelIndex = moodPicker[1].indexOf('for="chatbox-mood-custom"');
    const customTextIndex = moodPicker[1].indexOf('id="chatbox-mood-custom-text"');
    assert.ok(customRadioIndex !== -1 && customLabelIndex !== -1 && customTextIndex !== -1);
    assert.ok(customRadioIndex < customLabelIndex && customLabelIndex < customTextIndex);

    const customText = moodPicker[1].match(/<input\s+id="chatbox-mood-custom-text"[\s\S]*?>/);
    assert.ok(customText, 'custom mood text input not found');
    assert.match(customText[0], /type="text"/);
    assert.match(customText[0], /maxlength="80"/);
    assert.match(customText[0], /placeholder="e\.g\. sarcastically"/);
    assert.match(customText[0], /aria-label="Custom mood description"/);
    assert.match(customText[0], /aria-describedby="chatbox-mood-custom-error"/);
    assert.match(customText[0], /aria-invalid="false"/);

    // Compact, keyboard-visible, and allowed to wrap rather than overflow the shell.
    assert.match(css, /\.focus-chatbox-mood-options\s*\{[\s\S]*?flex-wrap:\s*wrap/);
    const customTextRule = css.match(/\.focus-chatbox-mood-custom-input\s*\{([\s\S]*?)\}/);
    assert.ok(customTextRule, 'custom mood input styles not found');
    assert.match(customTextRule[1], /min-width:\s*0/);
    assert.match(customTextRule[1], /height:\s*30px/);
    assert.match(css, /\.focus-chatbox-mood-custom-input:focus,\s*\.focus-chatbox-mood-custom-input:focus-visible\s*\{/);
    assert.match(css, /\.focus-chatbox-mood-custom-input\[aria-invalid="true"\]\s*\{/);
});

test('ties the Custom mood radio and its text field to a single selection', () => {
    // Picking Custom drops the caret in the field; focusing or typing in the field picks Custom.
    assert.match(script, /if \(input\.value === customPlayerMoodValue\) \{\s*focusCustomPlayerMoodInput\(\);/);
    assert.match(script, /playerMoodCustomTextInput\.addEventListener\('focus', function\(\) \{\s*if \(selectCustomPlayerMood\(\)\) \{/);
    assert.match(script, /playerMoodCustomTextInput\.addEventListener\('input', function\(\) \{\s*selectCustomPlayerMood\(\);/);
    assert.match(script, /function selectCustomPlayerMood\(\)[\s\S]*?playerMoodCustomRadio\.checked = true/);

    // A predefined mood ignores the typed text without wiping it mid-session.
    const predefinedBranch = script.match(/input\.addEventListener\('change', function\(\) \{[\s\S]*?\n        \}\);/);
    assert.ok(predefinedBranch, 'mood change handler not found');
    assert.doesNotMatch(predefinedBranch[0], /playerMoodCustomTextInput\.value = ''/);
    assert.match(predefinedBranch[0], /setCustomPlayerMoodInvalid\(false\);/);

    // Skyrim input behaviour: the field keeps Enter-to-send and Escape-to-close like the composer.
    assert.match(script, /playerMoodCustomTextInput\.addEventListener\('keydown'[\s\S]*?'Escape'[\s\S]*?window\.closeFocusChatbox\(true\)[\s\S]*?'Enter'[\s\S]*?window\.sendFocusMessage\(\)/);

    // Restoring re-checks the saved radio and refills the field instead of blanking both.
    assert.match(script, /function restorePlayerMoodSelection\(\)[\s\S]*?input\.checked = input\.value === stored\.mood[\s\S]*?playerMoodCustomTextInput\.value = stored\.custom[\s\S]*?setCustomPlayerMoodInvalid\(false\)/);
    assert.match(script, /window\.sendFocusMessage[\s\S]*?window\.closeFocusChatbox\(true\)/);
});

test('normalizes and caps the custom mood text at the field maxlength', () => {
    const maxLength = Number(html.match(/id="chatbox-mood-custom-text"[\s\S]*?maxlength="(\d+)"/)[1]);
    assert.equal(maxLength, 80);

    const normalizeCustom = loadChatboxFunction('normalizeCustomPlayerMood');
    assert.equal(normalizeCustom('  sarcastically  '), 'sarcastically');
    assert.equal(normalizeCustom('very\n\tslowly   and   flatly'), 'very slowly and flatly');
    assert.equal(normalizeCustom('a'.repeat(maxLength + 40)).length, maxLength);

    // Blank-ish input is never a mood, so the send guard can rely on this alone.
    ['', '   ', '\t', '\n', '  \r\n ', null, undefined].forEach((text) => {
        assert.equal(normalizeCustom(text), '', JSON.stringify(text));
    });
});

test('refuses to send a blank Custom mood and exposes the invalid state', () => {
    // The composer stays open, the field takes focus, and the failure is announced.
    assert.match(script, /if \(customSelected && !customMood\) \{\s*setCustomPlayerMoodInvalid\(true\);\s*focusCustomPlayerMoodInput\(\);\s*return;/);
    assert.match(script, /function setCustomPlayerMoodInvalid\(invalid\)[\s\S]*?setAttribute\('aria-invalid', invalid \? 'true' : 'false'\)[\s\S]*?playerMoodCustomErrorElement\.hidden = !invalid/);
    assert.match(html, /<span id="chatbox-mood-custom-error"[^>]*role="alert"[^>]*hidden>/);

    // Typing clears the error instead of leaving a stale alert on screen.
    assert.match(script, /if \(getCustomPlayerMoodText\(\)\) \{\s*setCustomPlayerMoodInvalid\(false\);/);

    // The blank-custom bail happens before anything reaches the bridge.
    const sendBody = script.match(/window\.sendFocusMessage = function\(\) \{[\s\S]*?\n    \};/);
    assert.ok(sendBody, 'sendFocusMessage not found');
    assert.ok(
        sendBody[0].indexOf('setCustomPlayerMoodInvalid(true)') < sendBody[0].indexOf('sendMessageToBridge('),
        'blank custom mood must bail out before sending'
    );
});

test('sends the custom mood as JSON so pipes in player text stay intact', () => {
    const buildCustomMoodCommand = loadChatboxFunction('buildCustomMoodCommand');
    const command = buildCustomMoodCommand('sarcastically | dryly', 'Are you sure | about that?');

    assert.ok(command.startsWith('send_custom_mood|'), 'custom mood needs its own bridge command');
    // Everything after the single structural pipe is one JSON document, pipes and all.
    const payload = JSON.parse(command.slice('send_custom_mood|'.length));
    assert.deepEqual(payload, { custom_mood: 'sarcastically | dryly', message: 'Are you sure | about that?' });
    assert.deepEqual(Object.keys(payload), ['custom_mood', 'message']);

    // Quotes, newlines, and braces survive the same way.
    const tricky = buildCustomMoodCommand('with "air quotes"', 'line one\nline "two" {}');
    assert.deepEqual(JSON.parse(tricky.slice('send_custom_mood|'.length)), {
        custom_mood: 'with "air quotes"',
        message: 'line one\nline "two" {}'
    });

    // Predefined and no-mood transports are untouched, and custom never rides on send_mood|.
    assert.match(script, /mood \? 'send_mood\|' \+ mood \+ '\|' \+ message : 'send\|' \+ message/);
    assert.match(script, /function sendMessageToBridge\(message, playerMood, customMood\)[\s\S]*?if \(custom\) \{[\s\S]*?buildCustomMoodCommand\(custom, message\)/);
    assert.match(script, /sendMessageToBridge\(message, customSelected \? '' : getSelectedPlayerMood\(\), customMood\)/);

    // The optimistic row shows the typed message only; custom mood text travels out of band.
    assert.doesNotMatch(script, /pushChatMessage\([^)]*customMood/);
    assert.doesNotMatch(script, /customMood \+ /);

    // The bridge parses the JSON at a fixed offset, so the prefix length has to agree with it.
    const prefixLength = Number(bridge.match(/cmd\.starts_with\("send_custom_mood\|"\)[\s\S]*?json::parse\(cmd\.substr\((\d+)\)/)[1]);
    assert.equal(prefixLength, 'send_custom_mood|'.length);
    assert.match(bridge, /payload\.contains\("message"\)[\s\S]*?payload\.contains\("custom_mood"\)/);
    assert.match(bridge, /SendChatboxMessage\(message, "custom", customPlayerMood\)/);

    // The view caps text well under the plugin's own limit, so nothing typed can be silently rejected.
    const bridgeCap = Number(bridge.match(/customPlayerMood\.size\(\) > (\d+)/)[1]);
    const fieldMaxLength = Number(html.match(/id="chatbox-mood-custom-text"[\s\S]*?maxlength="(\d+)"/)[1]);
    assert.ok(fieldMaxLength <= bridgeCap, `field maxlength ${fieldMaxLength} exceeds bridge cap ${bridgeCap}`);

    // 'custom' rides as the mood sentinel with the phrasing in its own field, never as a predefined mood.
    assert.match(httpManager, /audienceSnapshot\["player_mood_custom"\] = routingContext->customPlayerMood/);
    assert.match(httpManager, /requestModeSnapshot\["player_mood_custom"\] = routingContext->customPlayerMood/);
    assert.match(conversationRouter, /std::string customPlayerMood;/);
});

test('sends validated mood metadata and leaves the live story row undecorated', () => {
    assert.match(script, /mood \? 'send_mood\|' \+ mood \+ '\|' \+ message : 'send\|' \+ message/);
    assert.match(bridge, /cmd\.starts_with\("send_mood\|"\)[\s\S]*?SendChatboxMessage\(message, playerMood, ""\)/);
    assert.match(bridge, /cmd\.starts_with\("send\|"\)[\s\S]*?SendChatboxMessage\(message, "", ""\)/);

    // The optimistic row renders the submitted text verbatim; mood phrasing belongs to the server.
    assert.match(bridge, /PushChatboxMessage\(playerName, message, "", "player"\)/);
    assert.ok(!bridge.includes('displayMessage'), 'story row must not rewrite the submitted message');
    assert.ok(!bridge.includes('[mood: '), 'raw mood tag must not reach the story row');
    assert.ok(!bridge.includes('DefaultPlayerMoodSuffix'), 'plugin must not build mood phrasing');
    assert.ok(!bridge.includes('speaks in a'), 'plugin must not build mood phrasing');

    // Mood still travels as routing metadata, and every selectable mood must survive validation.
    const supportedMood = bridge.match(/static bool IsSupportedFixedPlayerMood\(const std::string& playerMood\) \{\s*return([\s\S]*?);/);
    assert.ok(supportedMood, 'mood allowlist not found');
    // One allowlist covers both the typed send and the mood saved for speech-to-text.
    assert.equal(bridge.match(/IsSupportedFixedPlayerMood\(playerMood\)/g).length, 2);
    // 'custom' is resolved from its own JSON command, so it is deliberately absent from this allowlist.
    const offeredMoods = [...html.matchAll(/name="chatbox-player-mood" value="([^"]*)"/g)]
        .map((match) => match[1])
        .filter((mood) => mood && mood !== 'custom');
    assert.ok(offeredMoods.length >= 10, 'expected the full mood roster to be offered');
    offeredMoods.forEach((mood) => assert.ok(
        supportedMood[1].includes(`playerMood == "${mood}"`),
        `mood allowlist missing ${mood}`
    ));
    assert.match(bridge, /routingContext\.playerMood = playerMood/);
    assert.match(conversationRouter, /std::string playerMood;/);
    assert.match(httpManager, /audienceSnapshot\["player_mood"\] = routingContext->playerMood/);
    assert.match(httpManager, /requestModeSnapshot\["player_mood"\] = routingContext->playerMood/);
});

test('persists player mood and synchronizes it for speech-to-text', () => {
    assert.match(script, /const playerMoodStorageKey = 'chim_chatbox_player_mood'/);
    assert.match(script, /const playerMoodCustomTextStorageKey = 'chim_chatbox_custom_mood'/);
    assert.match(script, /function loadPlayerMoodSelection\(\)[\s\S]*?try \{[\s\S]*?localStorage\.getItem\(playerMoodStorageKey\)[\s\S]*?catch \(_err\) \{/);
    assert.match(script, /function savePlayerMoodSelection\(\)[\s\S]*?try \{[\s\S]*?localStorage\.setItem\(playerMoodStorageKey[\s\S]*?catch \(_err\) \{/);
    assert.match(script, /function restorePlayerMoodSelection\(\)[\s\S]*?input\.checked = input\.value === stored\.mood[\s\S]*?playerMoodCustomTextInput\.value = stored\.custom[\s\S]*?syncPlayerMoodToNative\(\)/);
    assert.match(script, /window\.openFocusChatbox[\s\S]*?restorePlayerMoodSelection\(\)/);
    assert.match(script, /setContextPlacement\(false\);\s*restorePlayerMoodSelection\(\);/);
    assert.match(script, /persistPlayerMoodSelection\(\)[\s\S]*?savePlayerMoodSelection\(\);\s*syncPlayerMoodToNative\(\)/);
    assert.doesNotMatch(script, /resetPlayerMood/);

    const buildPlayerMoodCommand = loadChatboxFunction('buildPlayerMoodCommand');
    const command = buildPlayerMoodCommand('custom', 'dryly | with "quotes"');
    assert.ok(command.startsWith('set_player_mood|'));
    assert.deepEqual(JSON.parse(command.slice('set_player_mood|'.length)), {
        player_mood: 'custom',
        custom_mood: 'dryly | with "quotes"'
    });
    const prefixLength = Number(bridge.match(/cmd\.starts_with\("set_player_mood\|"\)[\s\S]*?json::parse\(cmd\.substr\((\d+)\)/)[1]);
    assert.equal(prefixLength, 'set_player_mood|'.length);
    assert.match(script, /const mood = customSelected \? \(customMood \? customPlayerMoodValue : ''\) : getSelectedPlayerMood\(\)/);
    assert.match(bridge, /void ApplySavedPlayerMood\(PlayerConversationRoutingContext& routingContext\)[\s\S]*?routingContext\.playerMood = g_savedPlayerMood/);
});

test('applies the saved Prisma mood to both speech-to-text paths', () => {
    const voicerec = fs.readFileSync(path.resolve(__dirname, '../Voicerec.cpp'), 'utf8');
    const commands = fs.readFileSync(path.resolve(__dirname, '../Commands.cpp'), 'utf8');
    assert.match(voicerec, /routingContext\.source = PlayerConversationInputSource::Voice[\s\S]*?PrismaUIBridge::ApplySavedPlayerMood\(routingContext\)[\s\S]*?HTTPManager::streamPlayer/);
    assert.match(commands, /command\.contains\("ImpersonatePlayer"\)[\s\S]*?routingContext\.source = PlayerConversationInputSource::Voice[\s\S]*?PrismaUIBridge::ApplySavedPlayerMood\(routingContext\)[\s\S]*?sendMessageReal\(message, messageType, routingContext\)/);
});
