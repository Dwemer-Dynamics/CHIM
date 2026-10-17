Scriptname CHIMInteractExtensions Hidden
{Public API for adding plugin-owned actions to CHIM Interact. See docs/CHIM/agent-guide.md.}

; Registrations are not saved. They belong to the current game load only and are discarded when a
; save is loaded or a new game starts. A new game's OnInit can run before that load begins, so CHIM
; sends the mod event "CHIM_InteractActionsReady" once per load when it is ready. In OnInit and in a
; player alias's OnPlayerLoadGame, call RegisterForModEvent("CHIM_InteractActionsReady", ...) and
; register immediately; register again whenever the event arrives.
;
; actionId: "namespace:action", lowercase letters, digits and underscores, each part 1-31 characters,
;   starting with a letter. The namespace "chim" is reserved. One handler form owns an ID per load.
; description: 1-240 bytes of UTF-8 without control characters. It is shown to the Director as data.
; targetFlags: sum of 1 (living actor), 2 (dead actor), 4 (non-actor reference).
; itemRequirement: 0 optional, 1 a selected inventory item is required, 2 no item may be selected.
; minValue/maxValue: finite inclusive bounds within -1000000 to 1000000. wholeNumber requires integers.
;
; RegisterAction returns 1 registered, 2 updated by the same handler, or an error:
; -1 invalid handler, -2 invalid ID, -3 invalid description, -4 invalid target flags,
; -5 invalid item requirement, -6 invalid bounds, -7 ID owned by another handler, -8 registry full.
Int Function GetApiVersion() Global Native
Int Function RegisterAction(Form handler, String actionId, String description, Int targetFlags, Int itemRequirement, Float minValue, Float maxValue, Bool wholeNumber) Global Native
Bool Function UnregisterAction(Form handler, String actionId) Global Native

; CHIM sends one event to the owning handler form for each planned step of its action:
;   Event OnCHIMInteractAction(String requestId, Int step, String actionId, ObjectReference target, Form itemBase, Float value)
; itemBase is read-only context: the base form of the selected item, or None. It does not identify the
; selected enchanted, tempered or otherwise unique instance; never move or remove inventory through it.
;
; IsActionPending is true only while this handler's exact request step is waiting for its result: before
; its 15-second deadline, while this handler still owns the registration and the target is still eligible.
; CompleteAction reports "succeeded", "failed", "skipped" or "unknown" with up to 200 bytes of detail.
; Exactly one call can return True for a step, even when several arrive together; IsActionPending is
; False after it. It returns False for another handler, a stale step, a duplicate, an ineligible target,
; an unregistered action, a cancellation, a load or a timeout. Results are labelled as plugin-reported.
; CHIM records "unknown" if no result arrives within 15 seconds, or if the target or registration lapses
; between a True return and the receipt being recorded.
Bool Function IsActionPending(Form handler, String requestId, Int step) Global Native
Bool Function CompleteAction(Form handler, String requestId, Int step, String status, String detail) Global Native
