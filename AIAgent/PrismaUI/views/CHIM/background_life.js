(function () {
    'use strict';

    const modal = document.getElementById('background-life-modal');
    const activityList = document.getElementById('activity-list');
    const status = document.getElementById('panel-status');
    const npcFilter = document.getElementById('npc-filter');
    const searchInput = document.getElementById('search-input');
    const limitSelect = document.getElementById('limit-select');
    const refreshButton = document.getElementById('refresh-button');
    const previousButton = document.getElementById('previous-button');
    const nextButton = document.getElementById('next-button');
    const pageLabel = document.getElementById('page-label');

    let currentPage = 1;
    let totalPages = 1;
    let searchTimer = null;

    function sendCommand(command) {
        if (window.chimBackgroundLifeCommand) {
            window.chimBackgroundLifeCommand(command);
        }
    }

    function createElement(tagName, className, text) {
        const element = document.createElement(tagName);
        if (className) {
            element.className = className;
        }
        if (text !== undefined) {
            element.textContent = text;
        }
        return element;
    }

    function setLoading(loading) {
        modal.classList.toggle('loading', loading);
        refreshButton.disabled = loading;
    }

    function setStatus(message, isError) {
        status.textContent = message;
        status.classList.toggle('error', !!isError);
    }

    function renderNpcOptions(npcs) {
        const selected = npcFilter.value;
        npcFilter.replaceChildren();

        const allOption = document.createElement('option');
        allOption.value = '';
        allOption.textContent = 'All NPCs';
        npcFilter.appendChild(allOption);

        (npcs || []).forEach(function (npc) {
            const option = document.createElement('option');
            option.value = npc.name;
            option.textContent = npc.name + ' (' + npc.count + ')';
            npcFilter.appendChild(option);
        });

        npcFilter.value = selected;
    }

    function renderEmpty(message) {
        activityList.replaceChildren(createElement('div', 'empty-state', message));
    }

    function renderEntries(entries) {
        activityList.replaceChildren();
        if (!entries || entries.length === 0) {
            renderEmpty('No Background Life activity matches these filters.');
            return;
        }

        entries.forEach(function (entry) {
            const item = createElement('article', 'activity-entry');
            item.tabIndex = 0;
            item.setAttribute('aria-expanded', 'false');

            item.appendChild(createElement('div', 'activity-time', entry.tamrielic_time || 'Unknown time'));
            item.appendChild(createElement('div', 'activity-npc', entry.npc || 'Unknown NPC'));

            const summary = createElement('div', 'activity-summary');
            summary.appendChild(createElement('span', 'category-badge', entry.category || 'activity'));
            summary.appendChild(createElement('span', 'activity-text', entry.activity || 'No details recorded'));
            item.appendChild(summary);

            const details = createElement(
                'div',
                'activity-details',
                [entry.server_time, entry.rowid ? 'History ID ' + entry.rowid : ''].filter(Boolean).join(' | ')
            );
            item.appendChild(details);

            function toggleExpanded() {
                const expanded = !item.classList.contains('expanded');
                item.classList.toggle('expanded', expanded);
                item.setAttribute('aria-expanded', expanded ? 'true' : 'false');
            }

            item.addEventListener('click', toggleExpanded);
            item.addEventListener('keydown', function (event) {
                if (event.key === 'Enter' || event.key === ' ') {
                    event.preventDefault();
                    toggleExpanded();
                }
            });

            activityList.appendChild(item);
        });
    }

    function requestHistory() {
        setLoading(true);
        setStatus('Loading activity...', false);

        const params = new URLSearchParams({
            page: String(currentPage),
            limit: limitSelect.value
        });
        if (npcFilter.value) {
            params.set('npc', npcFilter.value);
        }
        if (searchInput.value.trim()) {
            params.set('search', searchInput.value.trim());
        }

        sendCommand('fetch|' + params.toString());
    }

    window.updateBackgroundLifeHistory = function (payload) {
        try {
            const data = typeof payload === 'string' ? JSON.parse(payload) : payload;
            if (!data || !data.success) {
                throw new Error((data && data.error) || 'Unable to load activity');
            }

            renderNpcOptions(data.npcs);
            renderEntries(data.entries);

            const pagination = data.pagination || {};
            currentPage = pagination.current_page || 1;
            totalPages = pagination.total_pages || 1;
            previousButton.disabled = currentPage <= 1;
            nextButton.disabled = currentPage >= totalPages;
            pageLabel.textContent = 'Page ' + currentPage + ' of ' + totalPages;

            const total = pagination.total_records === undefined ? data.entries.length : pagination.total_records;
            setStatus(total + ' activit' + (total === 1 ? 'y' : 'ies'), false);
        } catch (error) {
            renderEmpty('Background Life history could not be loaded.');
            setStatus(error.message || 'Unable to load activity', true);
        } finally {
            setLoading(false);
        }
    };

    window.showBackgroundLifeError = function (message) {
        renderEmpty('Background Life history could not be loaded.');
        setStatus(message || 'Unable to load activity', true);
        setLoading(false);
    };

    window.closePanel = function () {
        sendCommand('close');
    };

    npcFilter.addEventListener('change', function () {
        currentPage = 1;
        requestHistory();
    });
    searchInput.addEventListener('input', function () {
        window.clearTimeout(searchTimer);
        searchTimer = window.setTimeout(function () {
            currentPage = 1;
            requestHistory();
        }, 300);
    });
    limitSelect.addEventListener('change', function () {
        currentPage = 1;
        requestHistory();
    });
    refreshButton.addEventListener('click', requestHistory);
    previousButton.addEventListener('click', function () {
        if (currentPage > 1) {
            currentPage -= 1;
            requestHistory();
        }
    });
    nextButton.addEventListener('click', function () {
        if (currentPage < totalPages) {
            currentPage += 1;
            requestHistory();
        }
    });
    document.addEventListener('keydown', function (event) {
        if (event.key === 'Escape') {
            event.preventDefault();
            window.closePanel();
        }
    });
    document.addEventListener('DOMContentLoaded', function () {
        sendCommand('dom_ready');
    });
})();
