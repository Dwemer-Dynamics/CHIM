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

test('omits scene context events from the recent context log', () => {
    const entry = storyLog.normalizeEvent({
        Event: 'infoloc',
        Events: '(Context location: Riverwood outdoors ,Hold: Whiterun, Buildings to go:Sleeping Giant Inn)'
    }, 'The Narrator');

    assert.equal(entry, null);
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

test('collapses duplicate persisted events within ten seconds', () => {
    const tamrielicKey = '<a href="#">Tamrielic Time</a>';
    const entries = storyLog.normalizeEntries([
        {
            Event: 'quest',
            Events: 'Quest Updated "CHIM Quests" new objective: CHIM Quest is active!',
            [tamrielicKey]: '18th of Last Seed, 4E 201, 19:47',
            'Time (UTC)': '19-07-2026 22:58:01',
            ROWID: '21220'
        },
        {
            Event: 'quest',
            Events: 'Quest Updated "CHIM Quests" new objective: CHIM Quest is active!',
            [tamrielicKey]: '18th of Last Seed, 4E 201, 19:48',
            'Time (UTC)': '19-07-2026 22:58:06',
            ROWID: '21226'
        },
        {
            Event: 'infoaction',
            Events: 'RANGROO uses Sit',
            [tamrielicKey]: '18th of Last Seed, 4E 201, 21:12',
            'Time (UTC)': '19-07-2026 22:59:31',
            ROWID: '21289'
        },
        {
            Event: 'infoaction',
            Events: 'RANGROO uses Sit',
            [tamrielicKey]: '18th of Last Seed, 4E 201, 21:13',
            'Time (UTC)': '19-07-2026 22:59:33',
            ROWID: '21290'
        },
        {
            Event: 'infoaction',
            Events: 'RANGROO uses Sit',
            [tamrielicKey]: '18th of Last Seed, 4E 201, 21:20',
            'Time (UTC)': '19-07-2026 23:00:03',
            ROWID: '21313'
        }
    ], 'The Narrator');

    assert.deepEqual(entries.map((entry) => entry.rowId), [21220, 21289, 21313]);
    assert.deepEqual(entries.map((entry) => entry.timestamp), ['19:47', '21:12', '21:20']);
});

test('cleans near-whole elapsed hour values without changing real fractions', () => {
    const entries = storyLog.normalizeEntries([
        {
            Event: 'info_timeforward',
            Events: '0.9999984 hours have passed. Current date/time: Morndas, 9:12 PM.',
            'Time (UTC)': '19-07-2026 22:59:31',
            ROWID: '1'
        },
        {
            Event: 'info_timeforward',
            Events: '9.9999984 hours have passed. Current date/time: Tirdas, 7:19 AM.',
            'Time (UTC)': '19-07-2026 23:00:05',
            ROWID: '2'
        },
        {
            Event: 'info_timeforward',
            Events: '1.5 hours have passed. Current date/time: Tirdas, 8:49 AM.',
            'Time (UTC)': '19-07-2026 23:00:30',
            ROWID: '3'
        }
    ], 'The Narrator');

    assert.equal(entries[0].text, '1 hour has passed. Current date/time: Morndas, 9:12 PM.');
    assert.equal(entries[1].text, '10 hours have passed. Current date/time: Tirdas, 7:19 AM.');
    assert.equal(entries[2].text, '1.5 hours have passed. Current date/time: Tirdas, 8:49 AM.');
});

test('identifies duplicate persisted events received in separate polling batches', () => {
    const first = storyLog.normalizeEvent({
        Event: 'infoaction',
        Events: 'RANGROO uses Sit',
        'Time (UTC)': '19-07-2026 22:59:31',
        ROWID: '21289'
    }, 'The Narrator');
    const duplicate = storyLog.normalizeEvent({
        Event: 'infoaction',
        Events: 'RANGROO uses Sit',
        'Time (UTC)': '19-07-2026 22:59:33',
        ROWID: '21290'
    }, 'The Narrator');
    const laterRepeat = storyLog.normalizeEvent({
        Event: 'infoaction',
        Events: 'RANGROO uses Sit',
        'Time (UTC)': '19-07-2026 23:00:03',
        ROWID: '21313'
    }, 'The Narrator');

    assert.equal(storyLog.isPersistedDuplicate(first, duplicate), true);
    assert.equal(storyLog.isPersistedDuplicate(first, laterRepeat), false);
});

test('keeps ambient background chat as NPC dialogue', () => {
    const entry = storyLog.normalizeEvent({
        Event: 'chat_background',
        Events: '(Context location: Whiterun background chat) Lydia: The wind is picking up. (talking to Dragonborn)',
        'Tamrielic Time': '19th of Last Seed, 4E 201, 09:33',
        ROWID: '40'
    }, 'The Narrator');

    assert.equal(entry.kind, 'npc');
    assert.equal(entry.speaker, 'Lydia');
    assert.equal(entry.text, 'The wind is picking up.');
});

test('labels relationship events by affinity direction', () => {
    const entries = storyLog.normalizeEntries([
        {
            Event: 'relationship',
            Events: "Lydia's affinity toward RANGROO increased by 5 (28 to 33, now Friendly) and the relationship changed from neutral to platonic. Appreciated the help.",
            'Tamrielic Time': '19th of Last Seed, 4E 201, 10:05',
            'Time (UTC)': '19-07-2026 10:05:00',
            ROWID: 'relationship:50'
        },
        {
            Event: 'relationship',
            Events: "Lydia's affinity toward Nazeem decreased by 3 (20 to 17).",
            'Tamrielic Time': '19th of Last Seed, 4E 201, 10:06',
            'Time (UTC)': '19-07-2026 10:06:00',
            ROWID: 'relationship:51'
        },
        {
            Event: 'relationship',
            Events: "Lydia's relationship toward RANGROO changed from neutral to platonic.",
            'Tamrielic Time': '19th of Last Seed, 4E 201, 10:07',
            'Time (UTC)': '19-07-2026 10:07:00',
            ROWID: 'relationship:52'
        }
    ], 'The Narrator');

    assert.deepEqual(
        entries.map((entry) => entry.kind),
        ['relationship-up', 'relationship-down', 'relationship']
    );
    assert.deepEqual(
        entries.map((entry) => entry.speaker),
        ['Affinity', 'Affinity', 'Relationship']
    );
    // Direction must survive in the visible text, not only in the row colour.
    assert.match(entries[0].text, /increased by 5/);
    assert.match(entries[1].text, /decreased by 3/);
    assert.equal(entries[0].timestamp, '10:05');
});
