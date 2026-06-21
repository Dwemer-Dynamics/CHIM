(function () {
    'use strict';

    var root = document.getElementById('chim-confirmation');
    var title = document.getElementById('confirmation-title');
    var message = document.getElementById('confirmation-message');
    var cancelButton = document.getElementById('confirmation-cancel');
    var acceptButton = document.getElementById('confirmation-accept');
    var closeButton = document.getElementById('confirmation-close');
    var resolved = false;

    function send(command) {
        if (typeof window.chimConfirmationCommand === 'function') {
            window.chimConfirmationCommand(command);
        } else {
            console.log('[CHIM Confirmation] bridge unavailable:', command);
        }
    }

    function hideLocal() {
        if (root) {
            root.classList.add('hidden');
        }
    }

    function choose(command) {
        if (resolved) {
            return;
        }
        resolved = true;
        hideLocal();
        send(command);
    }

    window.showChimConfirmation = function (jsonPayload) {
        var payload = {};
        try {
            payload = JSON.parse(jsonPayload || '{}');
        } catch (error) {
            console.warn('[CHIM Confirmation] invalid payload:', error);
        }

        resolved = false;
        title.textContent = payload.title || 'Confirm Action';
        message.textContent = payload.message || '';
        cancelButton.textContent = payload.cancelLabel || 'Cancel';
        acceptButton.textContent = payload.acceptLabel || 'Accept';

        root.classList.remove('hidden');
        window.setTimeout(function () {
            acceptButton.focus();
        }, 0);
    };

    cancelButton.addEventListener('click', function () {
        choose('cancel');
    });

    closeButton.addEventListener('click', function () {
        choose('close');
    });

    acceptButton.addEventListener('click', function () {
        choose('accept');
    });

    window.addEventListener('keydown', function (event) {
        if (root.classList.contains('hidden')) {
            return;
        }
        if (event.key === 'Escape') {
            event.preventDefault();
            choose('escape');
        }
    });

    window.addEventListener('DOMContentLoaded', function () {
        send('dom_ready');
    });
})();
