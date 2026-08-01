(function(root, factory) {
    const api = factory();
    if (typeof module === 'object' && module.exports) {
        module.exports = api;
    }
    if (root) {
        root.ChimStoryLog = api;
    }
})(typeof window !== 'undefined' ? window : globalThis, function() {
    'use strict';

    const dialogueEvents = new Set(['chat', 'inputtext', 'ginputtext']);
    const actionEvents = new Set(['infoaction', 'book', 'combat', 'itemfound']);
    const storyEvents = new Set(['quest', 'death', 'info_timeforward', 'instruction', 'narration']);
    const persistedDuplicateWindowMs = 10000;

    function clean(value, decode) {
        const text = String(value || '');
        return typeof decode === 'function' ? decode(text).trim() : text.trim();
    }

    function sanitizeDialogue(text) {
        return String(text || '')
            .replace(/^\(Context (?:new )?location:[^)]+\)\s*/i, '')
            .replace(/\s+\(Context (?:new )?location:[\s\S]*$/i, '')
            .replace(/\s*,+\s*Current Date in Skyrim World:[\s\S]*$/i, '')
            .replace(/\s*\((?:talking|speaking) to [^)]+\)\s*$/i, '')
            .trim();
    }

    function parseSpeaker(text) {
        const colonIndex = text.indexOf(':');
        if (colonIndex <= 0 || colonIndex >= 80) {
            return { speaker: '', text: text };
        }
        return {
            speaker: text.substring(0, colonIndex).trim(),
            text: text.substring(colonIndex + 1).trim()
        };
    }

    function shortTimestamp(timestamp) {
        const value = String(timestamp || '').trim();
        const twentyFourHour = value.match(/,\s*(\d{1,2}:\d{2})\s*$/);
        if (twentyFourHour) return twentyFourHour[1];

        const twelveHour = value.match(/(\d{1,2}:\d{2}\s*(?:AM|PM))/i);
        if (twelveHour) return twelveHour[1].toUpperCase();
        return value;
    }

    function humanizeEventType(eventType) {
        return String(eventType || '')
            .replace(/^info_?/i, '')
            .replace(/_/g, ' ')
            .replace(/\b\w/g, function(character) { return character.toUpperCase(); });
    }

    function findField(entry, fieldName) {
        const expected = String(fieldName || '').toLowerCase();
        const key = Object.keys(entry || {}).find(function(candidate) {
            return String(candidate).toLowerCase().includes(expected);
        });
        return key ? entry[key] : '';
    }

    function parseUtcTimestamp(value) {
        const match = String(value || '').trim().match(
            /^(\d{2})-(\d{2})-(\d{4})\s+(\d{2}):(\d{2}):(\d{2})$/
        );
        if (!match) return 0;

        return Date.UTC(
            Number(match[3]),
            Number(match[2]) - 1,
            Number(match[1]),
            Number(match[4]),
            Number(match[5]),
            Number(match[6])
        );
    }

    function normalizeElapsedHours(text) {
        return String(text || '').replace(
            /(\d+(?:\.\d+)?)\s+hours?\s+have passed/gi,
            function(match, rawHours) {
                const hours = Number(rawHours);
                const rounded = Math.round(hours);
                if (!Number.isFinite(hours) || Math.abs(hours - rounded) > 0.01) {
                    return match;
                }
                return rounded + (rounded === 1 ? ' hour has passed' : ' hours have passed');
            }
        );
    }

    function buildEntry(rowId, timestamp, kind, speaker, text, source, occurredAtMs) {
        const normalizedSpeaker = String(speaker || '').trim();
        const normalizedText = String(text || '').trim();
        if (!normalizedText) return null;

        const contentKey = [
            kind,
            normalizedSpeaker.toLowerCase(),
            normalizedText.toLowerCase()
        ].join('|');

        return {
            rowId: Number(rowId || 0),
            timestamp: shortTimestamp(timestamp),
            kind: kind,
            speaker: normalizedSpeaker,
            text: normalizedText,
            source: source || '',
            occurredAtMs: Number(occurredAtMs || 0),
            contentKey: contentKey
        };
    }

    function normalizeEvent(entry, narratorName, decode) {
        const eventType = clean(entry.Event || entry.eventType || 'chat', decode).toLowerCase();
        const rawText = clean(entry.Events || entry.text || '', decode);
        const timestamp = clean(
            findField(entry, 'Tamrielic Time') || entry.timestamp || '',
            decode
        );
        const occurredAtMs = parseUtcTimestamp(
            clean(findField(entry, 'Time (UTC)') || entry.utcTimestamp || '', decode)
        );
        const rowId = Number(entry.ROWID || entry.rowId || 0);
        const source = clean(entry.Source || entry.source || '', decode);
        if (!rawText) return null;

        if (dialogueEvents.has(eventType)) {
            const parsed = parseSpeaker(sanitizeDialogue(rawText));
            if (!parsed.text) return null;

            let kind = eventType === 'inputtext' || eventType === 'ginputtext' ? 'player' : 'npc';
            const speakerLower = parsed.speaker.toLowerCase();
            const narratorLower = String(narratorName || '').toLowerCase();
            if (speakerLower === narratorLower || speakerLower === 'the narrator' || speakerLower === 'narrator') {
                kind = 'narrator';
            }
            return buildEntry(
                rowId,
                timestamp,
                kind,
                parsed.speaker || 'Unknown',
                parsed.text,
                source,
                occurredAtMs
            );
        }

        if (actionEvents.has(eventType)) {
            return buildEntry(rowId, timestamp, 'action', 'Action', rawText, source, occurredAtMs);
        }

        if (storyEvents.has(eventType)) {
            const kind = eventType === 'death' ? 'death' : 'story';
            const label = eventType === 'info_timeforward' ? 'Time' : humanizeEventType(eventType);
            const text = eventType === 'info_timeforward'
                ? normalizeElapsedHours(rawText)
                : rawText;
            return buildEntry(rowId, timestamp, kind, label, text, source, occurredAtMs);
        }

        return null;
    }

    function normalizeLiveMessage(speaker, text, timestamp, type, source) {
        const normalizedType = String(type || 'npc').toLowerCase();
        const kind = ['player', 'narrator', 'system'].includes(normalizedType)
            ? normalizedType
            : 'npc';
        return buildEntry(0, timestamp, kind, speaker, text, source);
    }

    function isPersistedDuplicate(previous, entry) {
        if (!previous || !entry || previous.contentKey !== entry.contentKey) {
            return false;
        }

        if (entry.occurredAtMs && previous.occurredAtMs) {
            const timeDelta = entry.occurredAtMs - previous.occurredAtMs;
            return timeDelta >= 0 && timeDelta <= persistedDuplicateWindowMs;
        }

        if (!entry.occurredAtMs && !previous.occurredAtMs && entry.rowId && previous.rowId) {
            const rowDelta = entry.rowId - previous.rowId;
            return rowDelta > 0 && rowDelta <= 10;
        }

        return false;
    }

    function normalizeEntries(entries, narratorName, decode) {
        const normalized = (Array.isArray(entries) ? entries : [])
            .map(function(entry) { return normalizeEvent(entry, narratorName, decode); })
            .filter(Boolean)
            .sort(function(left, right) {
                if (left.rowId && right.rowId) return left.rowId - right.rowId;
                return 0;
            });

        const recentContent = new Map();
        return normalized.filter(function(entry) {
            const previous = recentContent.get(entry.contentKey);
            if (isPersistedDuplicate(previous, entry)) return false;
            recentContent.set(entry.contentKey, entry);
            return true;
        });
    }

    return {
        isPersistedDuplicate: isPersistedDuplicate,
        normalizeEvent: normalizeEvent,
        normalizeEntries: normalizeEntries,
        normalizeLiveMessage: normalizeLiveMessage,
        shortTimestamp: shortTimestamp
    };
});
