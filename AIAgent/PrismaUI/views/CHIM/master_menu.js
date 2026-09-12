let chimEnabled = true;

// Native state is authoritative, including reconnect failures and pending changes.
window.updateChimState = function (state) {
    chimEnabled = state.enabled === true;
    const toggle = document.getElementById('chim-toggle');
    toggle.textContent = 'CHIM: ' + (chimEnabled ? 'On' : 'Off');
    toggle.setAttribute('aria-checked', String(chimEnabled));
    toggle.disabled = state.syncing === true;
    document.getElementById('chim-chat-button').disabled = !chimEnabled;
    const notice = document.getElementById('chim-interaction-notice');
    notice.hidden = chimEnabled && !state.syncing && !state.failed;
    notice.textContent = state.failed
        ? "Couldn't connect. CHIM is off locally. Retrying when connected."
        : state.syncing ? 'Updating CHIM…'
        : 'AI dialogue and actions are off. Game events are still recorded.';
};

function toggleChim() {
    if (window.chimMasterMenuCommand) window.chimMasterMenuCommand('chim_toggle');
}

// CHIM Master Menu JavaScript

let hotkeyCloseArmedAt = 0;
let hudLayoutExpanded = false;
let toolsExpanded = false;
let contextWindowVisible = false;

const SUPPORT_REPORT_HELP = 'Generate logs for debugging';
const SUPPORT_REPORT_LABEL = 'Generate Logs';
const SUPPORT_REPORT_IDLE_ARIA = SUPPORT_REPORT_LABEL + ': ' + SUPPORT_REPORT_HELP;

// Footer copy carries an explicit prefix so the outcome never depends on colour alone.
const SUPPORT_REPORT_STATES = {
    idle: {
        label: SUPPORT_REPORT_LABEL,
        aria: SUPPORT_REPORT_IDLE_ARIA,
        busy: false,
        prefix: '',
        fallback: '',
        color: '#e8e8e8'
    },
    confirming: {
        label: 'Confirming...',
        aria: 'Waiting for confirmation',
        busy: true,
        prefix: 'Logs: ',
        fallback: 'confirm to continue',
        color: '#f2c317'
    },
    generating: {
        label: 'Generating...',
        aria: 'Generating logs',
        busy: true,
        prefix: 'Logs: ',
        fallback: 'generating logs...',
        color: '#f2c317'
    },
    success: {
        label: SUPPORT_REPORT_LABEL,
        aria: SUPPORT_REPORT_IDLE_ARIA,
        busy: false,
        prefix: 'Logs ready: ',
        fallback: 'saved to your Desktop',
        color: '#8fe3a8'
    },
    partial: {
        label: SUPPORT_REPORT_LABEL,
        aria: SUPPORT_REPORT_IDLE_ARIA,
        busy: false,
        prefix: 'Logs incomplete: ',
        fallback: 'some logs could not be collected',
        color: '#f2c317'
    },
    error: {
        label: SUPPORT_REPORT_LABEL,
        aria: SUPPORT_REPORT_IDLE_ARIA,
        busy: false,
        prefix: 'Logs failed: ',
        fallback: 'couldn\'t generate logs',
        color: '#ff9b8f'
    }
};

let supportReportState = 'idle';
let supportReportNotice = '';
let supportReportNoticeColor = '';

window.setPluginVersion = function(version) {
    const normalizedVersion = String(version || '').trim();
    const title = normalizedVersion ? `CHIM (${normalizedVersion})` : 'CHIM';
    const titleElement = document.getElementById('master-menu-title');

    document.title = title;
    if (titleElement) {
        titleElement.textContent = title;
    }
};

function setFooterHelp(text, color) {
    const descElement = document.getElementById('hover-description');
    if (descElement) {
        descElement.textContent = text;
        descElement.style.color = color;
    }
}

// Show description in footer
window.showDescription = function(text) {
    setFooterHelp(text, '#e8e8e8');
};

// Clear description in footer
window.clearDescription = function() {
    if (supportReportNotice) {
        setFooterHelp(supportReportNotice, supportReportNoticeColor);
        return;
    }
    setFooterHelp('Select a panel to toggle', '#999');
};

function getSupportReportButton() {
    return document.getElementById('generate-logs-btn');
}

function isSupportReportBusy() {
    const config = SUPPORT_REPORT_STATES[supportReportState];
    return !!(config && config.busy);
}

// Show the button's purpose on hover, or the live progress copy while it is working.
window.describeSupportReport = function() {
    if (supportReportNotice && isSupportReportBusy()) {
        setFooterHelp(supportReportNotice, supportReportNoticeColor);
        return;
    }
    window.showDescription(SUPPORT_REPORT_HELP);
};

// Called by native code to publish support report progress:
// idle | confirming | generating | success | partial | error
window.setSupportReportState = function(state, message) {
    const requested = String(state === undefined || state === null ? '' : state).trim().toLowerCase();
    const config = SUPPORT_REPORT_STATES[requested] || SUPPORT_REPORT_STATES.idle;
    const resolvedState = SUPPORT_REPORT_STATES[requested] ? requested : 'idle';
    const detail = String(message === undefined || message === null ? '' : message).trim();

    supportReportState = resolvedState;

    const button = getSupportReportButton();
    if (button) {
        const label = document.getElementById('generate-logs-label');
        if (label) {
            label.textContent = config.label;
        }
        button.disabled = config.busy;
        button.setAttribute('aria-busy', config.busy ? 'true' : 'false');
        button.setAttribute('aria-label', config.aria);
    }

    const body = detail || config.fallback;
    supportReportNotice = resolvedState === 'idle' ? detail : (config.prefix + body);
    supportReportNoticeColor = supportReportNotice ? config.color : '';

    const statusElement = document.getElementById('support-report-status');
    if (statusElement) {
        statusElement.textContent = supportReportNotice;
    }

    window.clearDescription();
};

window.requestSupportReport = function() {
    if (isSupportReportBusy()) {
        window.describeSupportReport();
        return;
    }

    if (!window.chimMasterMenuCommand) {
        window.setSupportReportState('error', 'CHIM is not ready. Try again.');
        return;
    }

    console.log('[CHIM Master Menu] Requesting support report');
    window.chimMasterMenuCommand('generate_logs');

    // Optimistic lock so the button cannot be double-fired before native reports back.
    window.setSupportReportState('confirming');
};

// Initialize when DOM is ready
function initMasterMenu() {
    console.log('[CHIM Master Menu] Menu initialized');
    hotkeyCloseArmedAt = Date.now() + 250;
    
    // Signal to C++ that DOM is ready
    if (window.chimMasterMenuCommand) {
        window.chimMasterMenuCommand('dom_ready');
    }
    
    // Add keyboard listener for ESC key to close menu
    document.addEventListener('keydown', handleKeyDown);
    document.addEventListener('wheel', handleContextWindowWheel, { passive: false });

    initLayoutPickers();
    initMenuScalePicker();
    window.setSupportReportState('idle');
    setHudLayoutExpanded(false);
    setToolsExpanded(false);
}

window.setContextWindowVisible = function(visible) {
    contextWindowVisible = !!visible;
};

function getContextWindowBounds() {
    const viewportWidth = window.innerWidth || 0;
    const viewportHeight = window.innerHeight || 0;
    const compact = viewportWidth <= 900;
    const baseWidth = compact
        ? Math.min(520, Math.max(0, viewportWidth - 40))
        : Math.min(720, viewportWidth * 0.44);
    const baseHeight = compact
        ? Math.min(240, viewportHeight * 0.28)
        : Math.min(300, viewportHeight * 0.32);
    const requestedScale = window.chimUIScale ? window.chimUIScale.getPercent() / 100 : 1;
    const fittingScale = baseWidth > 0 && baseHeight > 0
        ? Math.min(
            Math.max(1, viewportWidth - 48) / baseWidth,
            Math.max(1, viewportHeight - 48) / baseHeight
        )
        : 1;
    const scale = Math.max(1, Math.min(requestedScale, fittingScale));
    const width = baseWidth * scale;
    const height = baseHeight * scale;
    const gap = window.chimLayout ? window.chimLayout.getGap() : 20;
    const corner = window.chimLayout ? window.chimLayout.getCorner('chatbox') : 'bottom-left';
    const left = corner.endsWith('right') ? viewportWidth - gap - width : gap;
    const top = corner.startsWith('bottom') ? viewportHeight - gap - height : gap;

    return {
        left: left,
        top: top,
        right: left + width,
        bottom: top + height
    };
}

function handleContextWindowWheel(event) {
    if (!contextWindowVisible || !window.chimMasterMenuCommand) {
        return;
    }

    const bounds = getContextWindowBounds();
    if (event.clientX < bounds.left || event.clientX > bounds.right ||
        event.clientY < bounds.top || event.clientY > bounds.bottom) {
        return;
    }

    const delta = Math.max(-1200, Math.min(1200, Number(event.deltaY) || 0));
    if (delta === 0) {
        return;
    }

    event.preventDefault();
    event.stopPropagation();
    window.chimMasterMenuCommand('context_scroll|' + delta);
}

function updateMenuScalePicker() {
    const percent = window.chimUIScale ? window.chimUIScale.getPercent() : 100;
    document.querySelectorAll('.menu-scale-btn[data-scale]').forEach(function (button) {
        const isActive = Number.parseInt(button.dataset.scale, 10) === percent;
        button.classList.toggle('active', isActive);
        button.setAttribute('aria-pressed', isActive ? 'true' : 'false');
    });
}

function initMenuScalePicker() {
    updateMenuScalePicker();
    window.addEventListener('chim-ui-scale-change', updateMenuScalePicker);
    window.addEventListener('chim-ui-scale-applied', updateMenuScalePicker);
}

window.setMenuScale = function (percent) {
    if (!window.chimUIScale) {
        return;
    }
    const selectedPercent = window.chimUIScale.setPercent(percent);
    updateMenuScalePicker();
    window.showDescription('Prisma menu size set to ' + selectedPercent + '%');
};

// Handle keyboard events
function handleKeyDown(event) {
    // ESC key closes the menu
    if (event.key === 'Escape' || event.keyCode === 27) {
        event.preventDefault();
        event.stopPropagation();
        closeMenu();
        return;
    }

    // Allow the bound hotkey to close the menu again.
    // We intentionally avoid closing on modifier/navigation keys.
    if (Date.now() < hotkeyCloseArmedAt || event.repeat) {
        return;
    }
    if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) {
        return;
    }

    const code = event.code || '';
    const isHotkeyLike = code.startsWith('Key') ||
        code.startsWith('Digit') ||
        code.startsWith('Numpad') ||
        code.startsWith('F') ||
        [
            'Backquote', 'Minus', 'Equal',
            'BracketLeft', 'BracketRight', 'Backslash',
            'Semicolon', 'Quote', 'Comma', 'Period', 'Slash'
        ].includes(code);

    if (isHotkeyLike) {
        event.preventDefault();
        event.stopPropagation();
        closeMenu();
    }
}

// Handle panel selection
function selectPanel(panelId) {
    if (panelId === 'textchat' && !chimEnabled) return;
    console.log('[CHIM Master Menu] Selected panel:', panelId);
    
    // Send command to C++ bridge
    if (window.chimMasterMenuCommand) {
        window.chimMasterMenuCommand(panelId);
    }
}

function setHudLayoutExpanded(expanded) {
    const dropdown = document.getElementById('hud-layout-dropdown');
    const toggle = document.getElementById('hud-layout-toggle');
    const indicator = document.getElementById('hud-layout-indicator');
    if (!dropdown || !toggle || !indicator) {
        return;
    }

    hudLayoutExpanded = !!expanded;
    toggle.setAttribute('aria-expanded', hudLayoutExpanded ? 'true' : 'false');
    dropdown.hidden = !hudLayoutExpanded;
    indicator.textContent = hudLayoutExpanded ? '-' : '+';
}

window.toggleHudLayout = function() {
    setHudLayoutExpanded(!hudLayoutExpanded);
};

function setToolsExpanded(expanded) {
    const dropdown = document.getElementById('tools-dropdown');
    const toggle = document.getElementById('tools-toggle');
    const indicator = document.getElementById('tools-indicator');
    if (!dropdown || !toggle || !indicator) {
        return;
    }

    toolsExpanded = !!expanded;
    toggle.setAttribute('aria-expanded', toolsExpanded ? 'true' : 'false');
    dropdown.hidden = !toolsExpanded;
    indicator.textContent = toolsExpanded ? '-' : '+';
}

window.toggleToolsDropdown = function() {
    setToolsExpanded(!toolsExpanded);
};

window.setViewCorner = function(viewId, corner) {
    if (window.chimLayout) {
        window.chimLayout.setCorner(viewId, corner);
    }

    const picker = document.querySelector('.corner-picker[data-view="' + viewId + '"]');
    if (!picker) {
        return;
    }

    const buttons = picker.querySelectorAll('.corner-btn');
    for (let i = 0; i < buttons.length; i++) {
        const button = buttons[i];
        if (button.getAttribute('data-corner') === corner) {
            button.classList.add('active');
        } else {
            button.classList.remove('active');
        }
    }
};

window.resetLayoutDefaults = function() {
    if (!window.chimLayout || !window.chimLayout.views) {
        return;
    }

    const defaults = window.chimLayout.views;
    for (const viewId in defaults) {
        if (Object.prototype.hasOwnProperty.call(defaults, viewId)) {
            window.setViewCorner(viewId, defaults[viewId]);
        }
    }

    window.updateLayoutGap(20);
    const slider = document.getElementById('layout-gap-slider');
    if (slider) {
        slider.value = 20;
    }

    window.showDescription('HUD layout reset to defaults');
    setTimeout(window.clearDescription, 1500);
};

window.updateLayoutGap = function(value) {
    const gap = parseInt(value, 10);
    if (Number.isNaN(gap)) {
        return;
    }

    const valueLabel = document.getElementById('layout-gap-value');
    if (valueLabel) {
        valueLabel.textContent = gap + 'px';
    }

    if (window.chimLayout) {
        window.chimLayout.setGap(gap);
    }
};

function initLayoutPickers() {
    const slider = document.getElementById('layout-gap-slider');
    const valueLabel = document.getElementById('layout-gap-value');

    if (!window.chimLayout) {
        if (slider && valueLabel) {
            valueLabel.textContent = slider.value + 'px';
        }
        return;
    }

    const savedGap = window.chimLayout.getGap();
    if (slider) {
        slider.value = savedGap;
    }
    if (valueLabel) {
        valueLabel.textContent = savedGap + 'px';
    }

    const pickers = document.querySelectorAll('.corner-picker[data-view]');
    for (let i = 0; i < pickers.length; i++) {
        const picker = pickers[i];
        const viewId = picker.getAttribute('data-view');
        const savedCorner = window.chimLayout.getCorner(viewId);
        const buttons = picker.querySelectorAll('.corner-btn');

        for (let j = 0; j < buttons.length; j++) {
            const button = buttons[j];
            if (button.getAttribute('data-corner') === savedCorner) {
                button.classList.add('active');
            } else {
                button.classList.remove('active');
            }
        }
    }
}

// Close menu
function closeMenu() {
    console.log('[CHIM Master Menu] Closing menu');
    
    // Send close command to C++
    if (window.chimMasterMenuCommand) {
        window.chimMasterMenuCommand('close');
    }
}

// Called by C++ to signal that the JavaScript environment is ready
window.chimMasterMenuReady = function() {
    console.log('[CHIM Master Menu] JS ready callback received');
};

// Initialize on load
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initMasterMenu);
} else {
    initMasterMenu();
}
