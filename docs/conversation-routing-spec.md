# Player Conversation Routing Specification

## Purpose

CHIM resolves one responder and one authoritative audience snapshot for each
player utterance. Voice/STT, legacy text entry, and Prisma text chat all pass a
`PlayerConversationRoutingContext` to `HTTPManager::streamPlayer`, which invokes
`PlayerConversationRouter::Resolve` once for that request.

The UI may supply an explicit target, conversation mode, or broadcast request.
It does not implement a separate responder-selection algorithm.

## Terms

- **Responder**: The single NPC or Narrator asked to answer.
- **Direct target**: An NPC selected by Prisma, exact name, or the true game
  crosshair.
- **Automatic target**: An NPC selected by field of view or proximity.
- **Audience**: Nearby NPCs that receive the exchange as context but do not
  automatically respond.
- **People present**: Loaded actors physically inside the current conversation
  area. This list may include actors that are not activated in CHIM and cannot
  respond or receive AI actions.
- **Hard eligibility**: Safety checks that direct targeting cannot bypass.
- **Soft eligibility**: Automatic-selection checks that direct targeting may
  bypass.
- **Conversation mode**: Standard, Whisper, Close, or Shout.
- **Tool mode**: Director, Spawn, Cheat, Auto Chat, or event injection. Tool
  modes are not conversation-distance modes.

## Current Architecture

The native plugin owns player routing:

1. The input surface constructs `PlayerConversationRoutingContext`.
2. `HTTPManager::streamPlayer` calls `PlayerConversationRouter::Resolve`.
3. The resolver selects the responder, builds the managed audience, and records
   physical scene presence for the same request.
4. `HTTPManager` serializes the result as an audience snapshot whose source is
   `plugin_player_routing_v2`.
5. HerikaServer consumes that audience as authoritative participant scope.

The routing result contains:

- responder actor, agent, name, and selection reason;
- deterministic audience names;
- deterministic physical-presence entries with form ID and managed state;
- speech mode;
- listener and audience radii;
- direct-target, Narrator, and broadcast flags; and
- eligible nearby party actors for request context.

The older shuffled `InspectSurroundings` output may still provide descriptive
scene context, but it is not authoritative participant identity.

## Input Surfaces

### Voice and STT

Voice input reads the current persistent CHIM conversation mode, builds a
routing context, and calls `HTTPManager::streamPlayer`.

### Legacy text entry

Legacy text entry uses the same player stream. Holding left Ctrl when submitting
selects persistent Close mode and sends the utterance through the legacy
compatibility input path.
The eight-slot legacy mode wheel remains unchanged; Close is selected through
Ctrl submission or Prisma. The legacy spell compatibility effect may still
select the mode, but it does not mutate global hearing distances.

### Prisma text chat

Prisma exposes one **Mode** selector containing the conversation modes
Standard, Whisper, Close, Shout, and Narrator plus the tool modes Director,
Spawn, Cheat Mode, Auto Chat, Event Inject, and Inject & Chat.

Pressing Ctrl+Enter selects persistent Close mode before submitting the
message. Normal Enter uses the currently selected persistent mode. Prisma sends
an explicit NPC form ID when the user chooses a target.

`Everyone` is unavailable in Close mode because Close speech has a private
two-person audience.

## Mode Contract

| Mode | Native routing radius | Audience | Server delivery |
| --- | --- | --- | --- |
| Standard | Configured base auto-hearing radius | Responder plus eligible audible NPCs | Normal speech |
| Whisper | Base radius multiplied by 0.35 | Plugin snapshot is reduced; server narrows context to player and responder | Quiet/private speech treatment |
| Close | Fixed 200 Skyrim units | Player and resolved responder only | Private close-range speech |
| Shout | Base radius multiplied by 2.0 | Responder plus eligible audible NPCs inside the expanded radius | Loud speech treatment |

Sneaking multiplies the effective radius by 0.5 after the mode policy is
applied. Close therefore uses 200 units normally and 100 units while
sneaking.

Mode selection is request state, not global distance configuration. Switching
conversation modes does not rewrite `_max_distance_inside`,
`_max_distance_outside`, or server spatial settings.

## Candidate Eligibility

### Hard eligibility

An NPC cannot be selected when the actor:

- is invalid, deleted, disabled, or dead;
- is not loaded and safe to address;
- lacks an attached cell;
- is in a different interior/exterior loaded area; or
- is outside the current mode's direct-address boundary.

Hard eligibility always applies.

### Soft eligibility

Automatic selection rejects an NPC when:

- conversation cooldown is active;
- the NPC is hostile and hostile auto-add is disabled;
- the NPC is in combat and combat dialogue is disabled;
- the NPC is restrained;
- the NPC is sleeping; or
- the NPC is in a scripted scene and scene dialogue is disabled.

An explicit Prisma target, exact named target, or true crosshair target may
bypass soft eligibility. This permits a dialogue response only; it does not
authorize a package, animation, scene, quest, or gameplay-action interruption.

## Responder Priority

The resolver evaluates these rules in order:

1. `Everyone` requests select a deterministic transport listener and preserve a
   broadcast flag. Close mode rejects `Everyone`.
2. Explicit Narrator mode selects the Narrator.
3. An utterance beginning with `Hey Narrator` selects the Narrator.
4. A valid explicit Prisma form ID selects that NPC.
5. A longest, exact, normalized `Hey <full NPC name>` match selects that NPC.
6. A valid true game crosshair actor selects that NPC.
7. The configured skyward camera/HMD gesture selects the Narrator.
8. A bare `Hey` selects the best eligible field-of-view candidate.
9. Otherwise, the nearest eligible and physically audible NPC is selected.
10. If no eligible NPC exists, the Narrator is selected.

The true crosshair is the game crosshair reference. A forward-cone fallback is
not represented as a crosshair target.

Name matching is case-insensitive and punctuation-tolerant. It prefers the
longest exact normalized full name, then resolves duplicate names by distance
and form ID.

## Radius and Audibility

The effective listener radius also controls incidental audience membership.
Automatic candidates must be inside the effective radius and pass
`SpatialAwareness::Evaluate`.

For the request-local spatial evaluation:

- maximum air, interior, and exterior distances are set to the effective
  audience radius;
- immediate and auto-hearing shortcuts are disabled; and
- cached spatial results are invalidated before the routing scan.

This prevents a larger global auto-hearing shortcut from defeating Whisper,
Close, or sneaking scope.

Direct visual/name targeting may bypass soft audibility checks but still obeys
hard eligibility and the mode's direct-address boundary.

## Audience Construction

Audience construction happens after responder selection and uses the same
candidate snapshot:

- the player is always included;
- the resolved NPC responder is included;
- in Standard, Whisper, and Shout, other hard-eligible and physically audible
  NPCs inside the effective radius are included;
- in Close mode, no incidental NPC is included; and
- Narrator requests do not add nearby NPCs.

The responder is ordered first, followed by incidental audience members sorted
by distance and form ID. Duplicate names are removed.

Audience membership supplies context only. It does not cause every audience
member to generate a response.

## Physical Presence

Physical presence is separate from the managed audience and never participates
in responder selection. Standard and Shout requests scan loaded actors in the
current attached area and effective conversation radius. Dead, disabled,
unloaded, blank-named, different-cell, and different-worldspace actors are
excluded.

By default, only dialogue-capable non-creature races are included. Enabling the
existing **Add All races** MCM option also permits creatures and non-dialogue
races. Duplicate display names are collapsed case-insensitively, so multiple
generic actors such as `Chicken` occupy one context entry. Results are ordered
by distance and form ID and capped at 32 entries.

Whisper, Close, and Narrator requests do not include incidental physical
presence. This preserves their existing private audience behavior.

## Server Contract

The plugin sends an audience snapshot containing:

- `source: plugin_player_routing_v2`;
- speaker and resolved listener;
- ordered companion/audience names;
- ordered `present_actors` containing name, form ID, distance, managed state,
  and creature state;
- target mode;
- routing reason;
- speech mode;
- listener radius; and
- audience radius.

HerikaServer treats the managed audience as the maximum response and action
boundary for the request. Physical presence is used only for prompt scene
context and event-history membership, allowing an actor activated later to
recover events witnessed while inactive. It may narrow private delivery but
must not widen Close or Whisper scope using an independent nearby-NPC scan.

Server mode behavior:

- `WHISPER` retains quiet delivery and narrows context to the player and
  resolved listener.
- `CLOSE` uses private close-range wording and participant tags without
  changing global server distance settings.
- changing mode never restores hard-coded global distance defaults.

## Diagnostics

Each routed player utterance logs one `[PLAYER-ROUTING]` record with:

- input source and mode;
- normalized utterance;
- explicit target and true crosshair form IDs;
- responder and routing reason;
- direct and broadcast flags;
- effective listener and audience radii;
- audience count;
- physical-presence and inactive-presence counts; and
- rejected candidates with reasons.

This record is the primary evidence for target, range, cooldown, loading,
privacy, and spatial-audibility reports.

## Current Limitations

- Responder candidate enumeration is based on managed CHIM agents. Unmanaged
  actors may appear as physical context but are not promoted or selected.
- Direct address is bounded by the request's direct-address radius and current
  loaded area.
- Audience identity is serialized by name because that is the current server
  protocol.
- The Narrator camera gesture threshold is currently native behavior rather
  than a dedicated user-facing setting.

## Acceptance Scenarios

1. Voice, legacy text, and Prisma with equivalent state resolve through the same
   router.
2. A loaded inactive dialogue-capable NPC appears in physical scene context and
   event people but cannot respond or receive an AI action.
3. Creatures are excluded unless **Add All races** is enabled, and duplicate
   generic creature names collapse to one entry.
4. An explicit sleeping or scene-bound NPC can answer without a gameplay
   package interruption.
5. `Hey Lydia` resolves the closest present exact Lydia.
6. Bare `Hey` selects a deterministic eligible FOV candidate.
7. No crosshair and no special phrase selects the nearest eligible audible NPC.
8. The skyward gesture selects the Narrator before proximity fallback.
9. Standard speech includes eligible audible nearby NPCs as context.
10. Whisper and sneaking reduce both responder and audience scope.
11. Close mode at 200 units includes only player and responder.
12. Close mode while sneaking uses a 100-unit boundary.
13. `Everyone` cannot remain active after switching to Close mode.
14. Ctrl+Enter selects persistent Close mode in Prisma and legacy text.
15. Tool selection does not masquerade as a conversation-distance mode.
16. The server receives `plugin_player_routing_v2` with the matching speech
    mode, reason, and radius values.

## Source References

- `Plugin/PlayerConversationRouter.cpp`: unified responder and audience routing.
- `Plugin/PlayerConversationRoutingPolicy.h`: deterministic priority policy.
- `Plugin/HTTPManager.cpp`: one resolver call and authoritative snapshot
  serialization.
- `Plugin/SpatialAwareness.cpp`: request-local physical audibility.
- `Plugin/PrismaUIBridge.cpp`: Prisma mode/target transport and target display.
- `Plugin/Papyrus.cpp`: legacy text routing and mode synchronization.
- `Plugin/Voicerec.cpp`: voice/STT routing context.
- `AIAgent/PrismaUI/views/CHIM/chatbox.js`: Prisma controls and Ctrl+Enter.
- `AIAgent/Source/Scripts/AIAgentPapyrusFunctions.psc`: legacy controls and mode
  wheel.
- `AIAgent/Source/Scripts/AIAgentIntimacyBubbleEffect.psc`: compatibility mode
  selection without global distance mutation.
- HerikaServer `main.php`, `processor/chim_modes.php`, and
  `lib/chat_helper_functions.php`: authoritative participant and mode handling.
