(function () {
    'use strict';

    const STORAGE_KEY = 'chim_prisma_menu_scale';
    const DEFAULT_PERCENT = 100;
    const ALLOWED_PERCENTAGES = [100, 125, 150, 175, 200];
    const VIEWPORT_MARGIN = 24;
    let lastStoredValue = null;
    let refreshQueued = false;

    function normalizePercent(value) {
        const parsed = Number.parseInt(value, 10);
        return ALLOWED_PERCENTAGES.includes(parsed) ? parsed : DEFAULT_PERCENT;
    }

    function readPercent() {
        try {
            return normalizePercent(window.localStorage.getItem(STORAGE_KEY));
        } catch (error) {
            return DEFAULT_PERCENT;
        }
    }

    function getTargets() {
        const selectorList = document.body && document.body.dataset.chimMenuScaleTarget;
        if (!selectorList) {
            return [];
        }

        const targets = [];
        selectorList.split(',').forEach(function (selector) {
            const trimmed = selector.trim();
            if (!trimmed) {
                return;
            }
            document.querySelectorAll(trimmed).forEach(function (target) {
                targets.push(target);
            });
        });
        return targets;
    }

    function applyScale(target, requestedFactor) {
        const width = target.offsetWidth;
        const height = target.offsetHeight;
        let effectiveFactor = requestedFactor;

        if (width > 0 && height > 0) {
            const availableWidth = Math.max(1, window.innerWidth - (VIEWPORT_MARGIN * 2));
            const availableHeight = Math.max(1, window.innerHeight - (VIEWPORT_MARGIN * 2));
            const fittingFactor = Math.min(availableWidth / width, availableHeight / height);
            effectiveFactor = Math.max(1, Math.min(requestedFactor, fittingFactor));
        }

        const zoomValue = String(effectiveFactor);
        const effectivePercent = String(Math.round(effectiveFactor * 100));
        if (target.style.zoom !== zoomValue) {
            target.style.zoom = zoomValue;
        }
        if (target.dataset.chimMenuScaleEffective !== effectivePercent) {
            target.dataset.chimMenuScaleEffective = effectivePercent;
        }
    }

    function refresh() {
        refreshQueued = false;
        const percent = readPercent();
        const factor = percent / 100;
        getTargets().forEach(function (target) {
            applyScale(target, factor);
        });

        window.dispatchEvent(new CustomEvent('chim-ui-scale-applied', {
            detail: { percent: percent }
        }));
    }

    function queueRefresh() {
        if (refreshQueued) {
            return;
        }
        refreshQueued = true;
        window.requestAnimationFrame(refresh);
    }

    function setPercent(value) {
        const percent = normalizePercent(value);
        try {
            window.localStorage.setItem(STORAGE_KEY, String(percent));
            lastStoredValue = String(percent);
        } catch (error) {
            // The active view still updates even if persistent storage is unavailable.
        }
        refresh();
        window.dispatchEvent(new CustomEvent('chim-ui-scale-change', {
            detail: { percent: percent }
        }));
        return percent;
    }

    function initialize() {
        try {
            lastStoredValue = window.localStorage.getItem(STORAGE_KEY);
        } catch (error) {
            lastStoredValue = null;
        }

        refresh();
        window.addEventListener('resize', queueRefresh);
        window.addEventListener('storage', function (event) {
            if (event.key === STORAGE_KEY) {
                lastStoredValue = event.newValue;
                queueRefresh();
            }
        });

        const observer = new MutationObserver(queueRefresh);
        observer.observe(document.body, {
            attributes: true,
            childList: true,
            subtree: true,
            attributeFilter: ['class', 'style']
        });

        // Ultralight file views do not consistently emit storage events between views.
        window.setInterval(function () {
            let storedValue = null;
            try {
                storedValue = window.localStorage.getItem(STORAGE_KEY);
            } catch (error) {
                storedValue = null;
            }
            if (storedValue !== lastStoredValue) {
                lastStoredValue = storedValue;
                queueRefresh();
            }
        }, 500);
    }

    window.chimUIScale = {
        allowedPercentages: ALLOWED_PERCENTAGES.slice(),
        getPercent: readPercent,
        setPercent: setPercent,
        refresh: refresh
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initialize);
    } else {
        initialize();
    }
})();
