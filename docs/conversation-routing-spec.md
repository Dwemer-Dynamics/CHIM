# Player Conversation Routing Specification

## Purpose

This document defines how CHIM selects one responder and builds the wider
conversation audience for player speech. The same resolver must be used by:

- Voice/STT input
- The legacy text-entry menu
- Prisma text chat

The UI may provide an explicit target or speech modifier, but it must not
implement a separate target-selection algorithm.

## Terms

- **Responder**: The single NPC or Narrator asked to answer the player.
- **Direct target**: An NPC explicitly selected by name, Prisma target, or the
  true game crosshair.
- **Automatic target**: An NPC selected because they are in view or nearby.
- **Audience**: Other nearby NPCs that receive the exchange as conversation
  context. Audience members do not automatically generate a response.
- **Hard eligibility**: Safety requirements that cannot be bypassed.
- **Soft eligibility**: State checks that direct targeting may bypass.
- **Privacy scope**: The effective listening and audience radius for the
  utterance, including normal, sneaking, and intimate speech.

## Current Implementation

Player routing is currently split across several systems:

1. `SpatialSnapshotManager` builds cached managed-NPC candidates.
2. `HTTPManager::stream` chooses the responder.
3. `HTTPManager::stream` separately builds an audience snapshot.
4. Prisma builds and validates its own visible target list before passing an
   optional override to `HTTPManager`.
5. The legacy text menu can cast the intimacy spell, while Prisma does not send
   the equivalent modifier.
6. The server treats the plugin-provided audience snapshot as authoritative.

This division allows the displayed target, selected responder, physical
audibility, and context audience to disagree.

### Current responder order

For ordinary player input, the current resolver broadly uses:

1. A valid Prisma target override.
2. The current look target, if targetable.
3. The first auto-hearing eligible managed NPC.
4. The Narrator.
5. A later `"Hey <name>"` pass can replace that result, but it only searches
   followers and uses a case-sensitive first-word substring match.
6. Narrator name and camera-pitch overrides run after an NPC cooldown check.

Consequences:

- A named NPC who is not a follower cannot be selected by the current
  `"Hey <name>"` logic.
- Duplicate first names are ambiguous.
- A rejected named target can cancel the request on cooldown before the later
  Narrator override is evaluated.
- A cached look fallback can be shown as the current Prisma target even when
  final listener validation rejects it.

### Current spatial behavior

- Default auto-hearing radius is 8 meters, or 560 Skyrim units.
- Inside that radius, `SpatialAwareness::Evaluate` immediately returns success
  at full volume. Closed doors, line of sight, and navmesh path checks are
  skipped.
- Outside that radius, interior/exterior hearing limits, doors, line of sight,
  and path length can affect audibility.
- Player speech multipliers for Prisma mode and sneaking scale the larger
  hearing limits, but do not scale the 8-meter auto-hearing radius.
- The nearby-context pass also uses the unscaled 8-meter auto-hearing radius.
- One audible scan evaluates only the six nearest actors and caches its result.

Consequences:

- Sneaking and Whisper mode do not reliably reduce automatic listener or
  audience range.
- Nearby NPCs can receive context through walls and closed doors.
- Crowded scenes can produce incomplete or frame-dependent candidates.
- Raw player position and the VR HMD-adjusted player position are not used
  consistently.

### Current availability behavior

Current candidate systems do not use one availability definition.

- The spatial candidate blocker rejects cooldown, some hostile actors, combat
  when disabled, and restrained actors.
- Legacy `isAvailableforDialog` also rejects sleeping actors and actors in a
  current scene when scene dialogue is disabled.
- The source comment says current scenes are log-only, but the function still
  returns `false`.
- Direct crosshair selection is still subject to final spatial and automatic
  blockers. It is not guaranteed to respond.

### Current intimacy behavior

The legacy text menu checks key code 29 when text is submitted and casts
`IntimacySpell`. Its comment calls this Left Shift, but key code 29 is the
DirectInput left Ctrl key used by the intended Ctrl+Enter behavior.

`AIAgentIntimacyBubbleEffect` temporarily changes
`_max_distance_inside` and `_max_distance_outside` to 200 units. Those settings
now update only the legacy `DISTANCE_ACTIVATING_NPC_IN/OUT` globals. Current
player routing uses `_spatial_hearing_inside`, `_spatial_hearing_outside`, and
the separate auto-hearing radius.

Consequences:

- The intimacy bubble does not restrict the current responder or audience.
- The unscaled 560-unit auto-hearing path would still override a 200-unit
  interior/exterior setting.
- Prisma Enter has no Ctrl modifier path and never casts or transmits the
  intimacy state.

### Current nearby context behavior

The old `InspectSurroundings` function scans high-process actors by raw
distance, annotates their state, and shuffles the result. It is still added as
an `infonpc` event, but it is not the authoritative audience.

The audience sent to the server is a union of:

- Managed NPCs found inside the raw auto-hearing radius.
- Spatially valid player speech targets.
- The selected responder as a fallback.
- The player.

Narrator requests skip the nearby NPC audience. The raw-radius union can include
NPCs that failed line-of-sight or path checks.

## Required Behavior

### One deterministic resolver

All player input must call one resolver:

```text
ResolvePlayerUtterance(
    text,
    input_source,
    explicit_target_form_id,
    speech_mode,
    camera_state,
    player_state
) -> {
    responder,
    audience,
    routing_reason,
    candidate_diagnostics
}
```

The resolver must run once per utterance. The selected responder and audience
must be serialized together and remain unchanged for that request.

### Hard eligibility

An NPC cannot be selected when any of these are true:

- The actor/form is invalid or deleted.
- The actor is dead.
- The actor is disabled.
- The actor is not loaded enough to safely address.
- The actor is outside the applicable loaded world/cell boundary.
- The actor is excluded by the utterance privacy scope.

Hard eligibility is always enforced.

### Soft eligibility

Automatic selection should reject:

- Conversation cooldown.
- Hostile actors when hostile auto-add is disabled.
- Combat actors when combat dialogue is disabled.
- Restrained actors.
- Sleeping actors.
- Actors in a current scripted scene when scene interruption is disabled.

Direct targeting may bypass soft eligibility. This permits the requested
crosshair or explicit-name response from sleeping or scene-bound NPCs, but it
must not force an animation, package change, or gameplay action that interrupts
the scene. It only permits an AI dialogue response.

### Responder priority

Evaluate these rules in order:

1. Prisma `Everyone` mode produces a broadcast request. The plugin still
   selects one deterministic transport listener because the current server
   protocol requires a listener field, but that listener is not treated as an
   exclusive dialogue target.
2. Explicit Narrator mode or an explicit `"Hey Narrator"` addresses the
   Narrator.
3. An explicit Prisma target form ID or exact `"Hey <full NPC name>"` addresses
   that direct target.
4. A true game crosshair actor addresses that direct target.
5. A deliberate Narrator camera gesture, such as looking above the configured
   sky threshold, addresses the Narrator.
6. A bare `"Hey"` selects the best automatically eligible NPC inside both the
   camera/HMD field of view and the configured interaction radius.
7. Otherwise, select the nearest automatically eligible and physically audible
   NPC inside the effective listening radius.
8. If no eligible NPC is found, address the Narrator.

The true crosshair must mean the actual game crosshair reference. A forward-cone
fallback must not be reported or treated as a crosshair target.

### Name matching

`"Hey <NPC>"` matching must:

- Be anchored to the start of the player's utterance after protocol prefixes.
- Be case-insensitive and punctuation-tolerant.
- Prefer the longest exact normalized full-name match.
- Search present managed NPCs, not followers only.
- Resolve duplicate names deterministically by closest hard-eligible actor.
- Obey a configured direct-address maximum radius and loaded-area boundary.
- Bypass FOV and soft eligibility, but not hard eligibility.

UI selection must use form ID rather than display name.

### Bare "Hey" matching

A bare `"Hey"` must:

- Use the camera forward vector, or HMD forward vector in VR.
- Restrict candidates to the configured interaction radius.
- Use a deterministic score and tie-break order.
- Obey hard and soft eligibility.
- Fall through to nearest eligible selection when no FOV candidate exists.

### Narrator gesture

The Narrator camera gesture must be checked before generic nearest-NPC fallback.
This resolves the otherwise contradictory requirements that no crosshair uses
the nearest NPC while looking at the sky uses the Narrator.

The threshold must be configurable and based on camera/HMD pitch, not player
body orientation.

### Audience and nearby context

Audience construction is separate from responder selection:

- Always include the player.
- Include the resolved NPC responder.
- Include other present managed NPCs only when they are inside the effective
  audience radius, physically audible, and inside the same privacy scope.
- Do not make audience members respond merely because they receive context.
- Use deterministic ordering, preferably responder first and then distance and
  form ID.
- Do not use the shuffled `InspectSurroundings` output as authoritative
  participant data.
- Do not impose a partial six-candidate evaluation cap on the final audience.

The old `InspectSurroundings` concept can remain as descriptive scene context,
but participant identity must come from the resolved audience snapshot.

Narrator requests should include only the player and Narrator unless a future
feature explicitly defines a narrated group scene.

### Listening, sneaking, and intimacy

One effective-radius policy must control:

- Automatic responder range.
- Auto-hearing distance.
- Nearby audience/context range.
- Physical spatial-audio evaluation limits where applicable.

All relevant radii must receive the same request modifier. A modifier must not
scale only the outer hearing limits while leaving auto-hearing unchanged.

Recommended initial modes:

| Mode | Effective radius policy |
| --- | --- |
| Standard | Configured base listening and audience radius |
| Sneaking | Base radius multiplied by 0.5 |
| Intimate / Ctrl+Enter | 200 Skyrim units, then apply sneaking if active |
| Shout | Configured base radius multiplied by 2.0 |

Ctrl+Enter must be represented in the request passed to the shared resolver.
Prisma and legacy text entry must send the same mode. The spell may remain as a
visual/gameplay indicator, but routing must not depend on mutable global
distance settings or spell timing.

An intimacy request should be a per-utterance privacy scope. Only the player,
resolved responder, and explicitly admitted participants receive its context.
Nearby outsiders must not be added by the general audience scan.

### Physical audibility

Physical audibility should be used for automatic selection and incidental
audience membership. Direct visual targeting may bypass audibility because the
player is intentionally addressing a visible actor.

The automatic path must not return success solely because an actor is inside
the auto-hearing radius. Closed doors and meaningful physical separation must
still be able to exclude incidental listeners.

### Prisma parity

Prisma may:

- Supply an explicit target form ID.
- Supply an explicit speech mode.
- Display the resolver's current candidate information.

Prisma must not:

- Maintain a different target priority.
- Treat a forward-cone fallback as a true crosshair.
- Show a blocked actor as the active responder without explaining that it is
  unavailable.
- Lose Ctrl+Enter/intimate behavior.

Voice, legacy text, and Prisma text with the same text, target, player state,
and camera state must resolve to the same responder and audience.

## Logical Conflicts and Required Decisions

### No crosshair versus sky Narrator

Both states have no crosshair target. The specification resolves this by
checking the explicit Narrator camera gesture before nearest-NPC fallback.

### "Always responds" versus invalid game state

No actor can respond when dead, deleted, disabled, or not safely loaded. Those
are hard safety exclusions. Sleeping, cooldown, and scene participation are
soft exclusions that direct targeting can bypass without forcing gameplay
actions.

### Direct targeting versus privacy

An outside actor cannot be pulled into an active intimate utterance merely by
crosshair or proximity. The direct target is used to establish that
utterance's responder before the privacy audience is finalized. Subsequent
outsiders remain excluded.

### Nearby context versus quiet speech

"Nearby" must mean inside the effective audience radius for this utterance, not
inside the global default radius. Sneaking and intimacy therefore reduce both
listener selection and context distribution.

### Scene actors and quest safety

Allowing a scene actor to produce dialogue is not permission to interrupt the
actor's scene, package, animation, or quest behavior. Dialogue routing and
gameplay action eligibility remain separate.

### Managed versus newly encountered actors

Current spatial routing primarily searches managed agents. If the true
crosshair or exact named target is a valid but unmanaged NPC, the resolver
needs a bounded promote-on-demand path before it can satisfy direct-target
semantics. Automatic scans should remain managed-only to avoid promoting every
nearby actor.

## Implementation Boundaries

A future implementation should:

1. Add a single `PlayerUtteranceRoutingRequest` and
   `PlayerUtteranceRoutingResult`.
2. Move name, crosshair, FOV, nearest, and Narrator priority into one resolver.
3. Separate hard and soft eligibility functions.
4. Build the audience only after selecting the responder.
5. Apply one effective radius/privacy policy to target and audience scans.
6. Carry speech mode and explicit target form ID from every input UI.
7. Remove the late follower-only `"Hey"` override.
8. Stop using global intimacy-distance mutation as routing state.
9. Make VR position and forward-vector handling consistent.
10. Send one authoritative responder/audience snapshot to the server.
11. Add structured diagnostics for every routing decision.

## Required Diagnostics

Each player utterance should emit one compact structured log record containing:

- Input source and speech mode.
- Explicit target form ID, if any.
- True crosshair form ID, if any.
- Selected responder form ID/name.
- Routing reason.
- Effective listener and audience radii.
- Audience form IDs/names.
- Rejected candidate form ID and rejection reason.

This is required to distinguish intended Narrator fallback from stale target,
cooldown, privacy, range, loading, and spatial-audibility failures.

## Acceptance Scenarios

1. Crosshair on a sleeping living NPC: that NPC responds without being forced
   out of furniture.
2. Crosshair on a scene-bound living NPC: that NPC responds without a package
   or action interruption.
3. `"Hey Lydia"` while looking elsewhere: the closest present exact Lydia
   responds.
4. Bare `"Hey"` with two NPCs in view: the deterministic best FOV candidate
   responds.
5. No crosshair and no special phrase: nearest eligible audible NPC responds.
6. Looking at the sky with no crosshair: Narrator responds.
7. No eligible NPC in range: Narrator responds.
8. Standard speech near three audible NPCs: one responds and the other two
   receive context.
9. Ctrl+Enter at 200 units: the direct responder receives the utterance and an
   NPC outside 200 units receives no context.
10. Sneaking: responder and audience scans use the reduced radius.
11. Closed door: an incidental NPC on the other side is not selected and does
    not receive context.
12. The same utterance through voice, legacy text, and Prisma resolves to the
    same responder and audience.

## Source Audit References

- `Plugin/HTTPManager.cpp`: player responder and audience resolution.
- `Plugin/SpatialSnapshotManager.cpp`: crosshair/look fallback, candidate
  availability, auto-hearing eligibility, and caches.
- `Plugin/SpatialAwareness.cpp`: physical audibility evaluation.
- `Plugin/Commands.cpp`: player spatial settings and audible actor scans.
- `Plugin/PrismaUIBridge.cpp`: Prisma target list and speech-distance mode.
- `Plugin/Papyrus.cpp`: player text event creation and legacy distance settings.
- `Plugin/Voicerec.cpp`: voice/STT player input.
- `Plugin/Globals.h`: legacy dialogue availability.
- `AIAgent/PrismaUI/views/CHIM/chatbox.js`: Prisma Enter handling.
- `AIAgent/Source/Scripts/AIAgentPapyrusFunctions.psc`: legacy text entry and
  Ctrl intimacy trigger.
- `AIAgent/Source/Scripts/AIAgentIntimacyBubbleEffect.psc`: intimacy distance
  mutation.
- HerikaServer `main.php` and `lib/chat_helper_functions.php`: authoritative
  audience snapshot consumption.
