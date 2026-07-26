const test = require('node:test');
const assert = require('node:assert/strict');
const storyLog = require('../../AIAgent/PrismaUI/views/CHIM/story_log.js');

test('normalizes dialogue speakers and removes routing suffixes', () => {
    const entries = storyLog.normalizeEntries([
        {
            Event: 'chat',
            Events: 'Lydia: I am sworn to carry your burdens. (talking to Dragonborn)',
            'Tamrielic Time': '19th of Last Seed, 4E 201, 09:33',
            ROWID: '12'
        },
        {
            Event: 'inputtext',
            Events: 'Dragonborn:Follow me. (Talking to Lydia)',
            'Tamrielic Time': '19th of Last Seed, 4E 201, 09:32',
            ROWID: '11'
        }
    ], 'The Narrator');

    assert.deepEqual(entries.map((entry) => entry.kind), ['player', 'npc']);
    assert.equal(entries[0].speaker, 'Dragonborn');
    assert.equal(entries[0].text, 'Follow me.');
    assert.equal(entries[1].text, 'I am sworn to carry your burdens.');
    assert.equal(entries[1].timestamp, '09:33');
});

test('uses the configured narrator name', () => {
    const entry = storyLog.normalizeEvent({
        Event: 'chat',
        Events: 'The Chronicler: Rain darkens the road.',
        ROWID: '20'
    }, 'The Chronicler');

    assert.equal(entry.kind, 'narrator');
    assert.equal(entry.speaker, 'The Chronicler');
});

test('turns location context into a concise scene marker', () => {
    const entry = storyLog.normalizeEvent({
        Event: 'infoloc',
        Events: '(Context location: Riverwood outdoors ,Hold: Whiterun, Buildings to go:Sleeping Giant Inn)'
    }, 'The Narrator');

    assert.equal(entry.kind, 'scene');
    assert.equal(entry.text, 'Riverwood outdoors - Whiterun Hold');
    assert.equal(entry.sceneKey, 'riverwood outdoors|whiterun');
});

test('keeps story events and rejects maintenance events', () => {
    const death = storyLog.normalizeEvent({
        Event: 'death',
        Events: 'A bandit was slain.',
        ROWID: '30'
    }, 'The Narrator');
    const maintenance = storyLog.normalizeEvent({
        Event: 'status_msg',
        Events: 'Internal status update.',
        ROWID: '31'
    }, 'The Narrator');

    assert.equal(death.kind, 'death');
    assert.equal(death.speaker, 'Death');
    assert.equal(maintenance, null);
});
