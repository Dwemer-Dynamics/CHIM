# CHIM agent guide

## Identify the installation

CHIM consists of a Windows Skyrim client and a separate PHP/PostgreSQL backend, HerikaServer. SKSE means Skyrim Script Extender; LLM means language model; STT and TTS mean speech-to-text and text-to-speech.

- **Installed mod:** locate the active mod-manager profile and winning files for `SKSE/Plugins/AIAgent.dll`, `AIAgent.esp`, `Scripts/` and `PrismaUI/`. Files elsewhere may be overridden or unused.
- **Source checkout:** [CHIM](https://github.com/Dwemer-Dynamics/CHIM) contains `Plugin/` and `AIAgent/`. Record `git status --short --branch` and `git rev-parse HEAD`. Historical `HerikaAI-NG` and `aiagent-aiff` trees are not the current source.
- **Server:** [HerikaServer](https://github.com/Dwemer-Dynamics/HerikaServer) owns providers, profiles, prompts, memories and database state. Read its own `AGENTS.md` before changing it.

Record the Skyrim runtime, SKSE version, CHIM client version, server version and enabled extensions. The package's `fomod/info.xml`, native startup log and server's `.version.txt`/`.version_number.txt` provide version evidence. Do not infer binary provenance solely from a folder name. Online `unstable` links describe current development, which may differ from the installed release.

## How a turn works

1. Native code and Papyrus observe the world and receive player input. PrismaUI supplies the in-game web interface.
2. The client sends game events and conversation context to HerikaServer over HTTP. Voice capture uploads use the server's STT route.
3. HerikaServer records events, selects profiles/context, calls the configured LLM and produces dialogue/actions and TTS audio.
4. CHIM receives responses, resolves the intended actor, plays speech and applies permitted game actions through native/Papyrus code.
5. Load, halt and conversation changes can invalidate work. A successful HTTP response is not proof that the correct actor spoke or completed an action.

## Source map

Paths below are relative to the CHIM checkout, not this installed documentation folder.

| Work | Start here |
|---|---|
| Startup, observations and input | `Plugin/Plugin.cpp`, `Plugin/ChimInteraction.cpp` |
| Connection discovery and requests | `Plugin/Conf.cpp`, `Plugin/HTTPManager.cpp`, `Plugin/HTTPUploader.cpp` |
| Conversation targets | `Plugin/PlayerConversationRouter.cpp`, `Plugin/ActorTargetIdentifierUtils.h` |
| Responses and actions | `Plugin/SPGResponse.cpp`, `Plugin/Commands.cpp`, `Plugin/Papyrus.cpp` |
| Microphone and playback | `Plugin/Voicerec.cpp`, `Plugin/AudioManager.cpp`, `Plugin/SpeakManager.cpp` |
| Papyrus declarations/behavior | `AIAgent/Source/Scripts/AIAgentFunctions.psc`, `AIAgentPapyrusFunctions.psc` in the same directory |
| In-game UI/native bridge | `AIAgent/PrismaUI/views/CHIM/`, `Plugin/PrismaUIBridge.cpp` |
| Installer and bundled files | `AIAgent/fomod/ModuleConfig.xml`, `AIAgent/` |

For prompts, providers or memory, move to HerikaServer instead of implementing a second copy in the client. Changes to shared settings or action messages may require paired changes and checks in both repositories.

## Configuration and diagnostics

`Plugin/Conf.cpp` checks `Data/SKSE/Plugins/AIAgent.ini` first, then launcher discovery at `127.0.0.1:7135`, then the built-in fallback `127.0.0.1:8081/HerikaServer/comm.php`. An INI remains supported. Inspect the active file and logged selected route rather than assuming the fallback is in use. Do not replace user connection settings with these examples.

Inspect the actual SKSE log directory under the active Windows Documents/My Games location. Common folders include `Skyrim Special Edition/SKSE`, `Skyrim VR/SKSE`, or a customized game folder. Search for the recently written AIAgent/SKSE logs and check their timestamps; do not hard-code another person's Documents path. HerikaServer normally writes `log/chim.log` under its runtime root, with Apache/PHP and worker logs providing the other side of the request.

| Symptom | Evidence to collect first |
|---|---|
| Plugin does not load | SKSE loader log, runtime compatibility, winning DLL and dependencies |
| No server connection | Selected route from client log, launcher state, server HTTP/PHP logs |
| No transcript | Microphone/capture and upload evidence, STT response; playback errors alone do not prove an STT failure |
| Text arrives without speech | TTS generation result, audio download and playback logs |
| Wrong NPC or action | Actor/reference identity, request/response target and game-side result |
| Memory/profile problem | Matching server playthrough, relevant profile/history and worker logs |

Use the user's existing diagnostic controls where possible. Redact keys and private dialogue before publishing logs. Do not run database reset, cleanup, deployment or game launches merely to inspect an issue.

## Build, extend and validate

See [building.md](building.md) and [custom-plugins.md](custom-plugins.md). Native, Papyrus, UI and server changes have different checks. Compile only the affected code, verify the intended output, and report actual game testing separately. Preserve optional integration behavior when dependencies are absent.

## Packaging these instructions

`AIAgent/docs/CHIM/` is the single canonical copy. The FOMOD installs it for every option, and `AIAgent/AIAgent/README.md` points installed users here. A maintainer's packager may remove the payload-root README, so do not rely on that file alone. Keep the namespaced folder intact when copying or archiving the mod; do not install a generic `Data/AGENTS.md`.

## Background Life automatic enrollment

Automatic Enrollment is off by default. Turn it on and set Events Before Enrollment (1-5000, default 200) in the Background Life page settings, the HerikaServer map page or Global Settings. All three save the same server settings, `BGL_AUTO_ENROLL_ENABLED` and `BGL_AUTO_ENROLL_EVENT_THRESHOLD`.

When on, HerikaServer checks only the NPC whose reply to the player was confirmed as spoken. The NPC is added when they have no stored enrollment choice, are alive, unique by name, not a known animal or summon, and have taken part in enough recorded events. Lines that were emitted but never confirmed, or were aborted, do not count. Automatic enrollment leaves Actions, Letters and combat off and waits a full trigger period before the first update. Removing an NPC by hand keeps them out until they are added again.

On older servers the dashboard response has no automatic-enrollment settings, so those controls stay disabled and only the trigger time is saved. The in-game Settings hub needs no client change: `config_manager.js` renders Global Settings fields, types and `min`/`max` limits from the server catalog and uses the shared focus handlers for keyboard capture, while `chim_global_settings.php` validates and clamps saved values.

## Chat Background Life and affinity

The chat's compact Background Life button sits beside the Current Target name; long names truncate before the distance or the button. It appears only for one real NPC target, including a resolved Auto target, and is hidden for Everyone, the Narrator and no target. The button shows **Loading…**, **Add BGL**, **Adding…**, **Remove BGL** or **Removing…**; its tooltip has the full explanation. **Remove BGL** appears for a saved, enrolled NPC when the plugin reports on DOM ready that it can remove (`setChatboxBackgroundLifeRemoveAvailable`); with an older DLL the button shows a disabled **In BGL** instead. **Loading…** is disabled from the start of a save load or new game until that load's Background Life setup has run: for a save, the existing 15-second post-load pass that sends `enable_bg` for saved Background Life NPCs; for a new game, its connected setup. Ready means those requests were sent, not that the server saved them. Starting a load cancels an add in progress, clears cached statuses and unknown-status retries, and ignores late replies; when the game reports the load ready, the chat reads fresh statuses once. The bridge enforces the same gate for `bgl_enroll` when the command is queued and again on the game thread, using the playthrough load generation, so a click queued before a load cannot enroll in the next save. If the playthrough connection fails, the button stays **Loading…** until a later load connects. A newly created chat view receives the current state on DOM ready. Older plugins never send this state, so the chat keeps its earlier behavior. When the NPC cannot be added yet (no saved server record, several records with the same name, an older plugin or server, or another add in progress), the button is dimmed but still clickable, and a click reports the reason in the chat. When the cached status is not a saved, unenrolled NPC, a click first makes one fresh status read and sends nothing to the game unless that read finds one. An unknown current target is read again after 3, 10 and 30 seconds while the chat stays open, so an NPC that CHIM registers after the first read becomes addable without reopening; reopening the chat renews those three reads. Loading a save removes NPC records that were not backed up by that save, so an NPC can stay unknown until CHIM registers them again. The click records the target's form ID and sends `bgl_enroll|<request>|<formId>` to `Plugin/PrismaUIBridge.cpp`. On the game thread, the bridge checks that the actor is still a visible chat target. It then uses the Background Life page's enrollment path: add the actor to the rolemaster faction and send `enable_bg` for that actor. The page's own selection is not used. The chat shows **In BGL** only after HerikaServer reports the saved enrollment. It checks at most four times, and each read stops after 6 seconds, so a missing reply or server ends with a "Could not add" message rather than a permanent **Adding…**. Changing the server URL during an add cancels it with that message. A manual add ignores the automatic threshold, clears an earlier opt-out and leaves Actions, Letters and combat unchanged.

**Remove BGL** sends `bgl_enroll|<request>|<formId>|remove` for the captured form ID, with the same target check and load gate; the bridge removes the actor from the rolemaster faction and sends `disable_bg`. The server then saves the NPC as not enrolled and opted out, so automatic enrolment skips them until they are added again by hand. The result callback carries `enabled`, and a reply for the other action is ignored. Removal is reported only when the same four bounded reads find the saved record not enrolled; a missing or ambiguous record cannot confirm it, and a stale or failed read never reports success.

Target rows show the saved NPC-to-Player affinity after the name, such as `(+25)` or `(0)`. They show nothing when no relationship is saved, and never on Auto, Everyone or Narrator rows. The values come from one `chat_targets` read in `background_life_npc.php`, for at most 32 targets: the selected target first, then the other rows in list order. Rows beyond those 32 show no score until selected. The chat reads them when it opens, when the visible targets or the selected target change, after a recorded relationship change and while confirming an add. Distance updates do not refresh. Results are cached by RefID and name for the server's playthrough and player, only for the current 32 targets, and are cleared when the server URL changes. Without that server operation, scores are hidden and a click on the button reports that the server did not answer. With an older DLL, a click reports that the current plugin is needed.

## Chat Wait Here

The chat's **Wait Here** button, beside **Halt AI Actions**, does what holding the text hotkey does: the living NPC under the crosshair waits through `AIAgentAIMind.StartWait`. It sends no AI request, ignores the chat target, never falls back to a name or the nearest NPC, and is not limited to followers. The click sends `wait_here`; `Plugin/PrismaUIBridge.cpp` captures the crosshair actor's form ID at once and queues `rp_wait_here|<formId>`. `AIAgentPapyrusFunctions.WaitForNpc` revalidates that exact actor before waiting, and a missing, dead, disabled, unloaded or player target shows the hotkey's "Look at a living NPC" notice. The typed draft and textbox focus are kept.

## NPC schedules

The NPC editor's Schedules tab creates, edits, cancels and deletes appointments. Select a recognised location, an in-game day number and appointment time. Daily repetition is 24 game hours; zero means one time. Destination validation requires the paired server and CHIM scripts connected to the game. Saved schedules stay pending until the game resolves a persistent arrival marker; unknown or ambiguous AI destinations remain reminders with a clarification request.

Travel starts three game hours early. Routine progress checks run hourly; the worker checks departure and arrival deadlines on each service tick using the accepted game clock. Early arrivals wait. At the deadline CHIM checks the actor's actual location and moves late actors to the validated marker. Combat/dialogue can defer execution. Visits finish after arrival, stays after their duration, and duties require an AI-reported outcome. Repetition starts only after releasing the previous activity. Overlapping travel windows are rejected; multiple recurring schedules require matching intervals.

CHIM Off blocks new schedule commands. An already installed travel package can continue until it is released. Cancellation/deletion waits for the game's release acknowledgement; Retry resends a stalled operation. Timeline changes invalidate old occurrences after release. Load the corresponding game and server saves together. Dynamic references and ambiguous same-name AI duties are rejected. A valid marker does not prove navmesh reachability: verify travel, waiting, teleport and package restoration in Skyrim before release.

The paired protocol uses `BackgroundCmd@actor@Schedule/run/token/operation/destination/issuedDays` and `util_npc_schedule` replies containing `run/token/actor/operation/result/marker`. Operations are validate, travel, check, ensure and release. The client restores only its owned travel override and link, and the server accepts only the pending operation's matching token, actor and clock epoch. Compile CHIMSchedule.psc alongside AIAgentAIMind.psc when building the client payload.
## Automatic voice responder

For voice input only, when the router would use its nearest-eligible fallback among two or more NPCs, `Plugin/HTTPManager.cpp` asks HerikaServer's `stt_target.php` once which of those NPCs should answer. Explicit targets, a valid crosshair target, NPC names, the narrator mode or gesture, Everyone, and the Director, Hypnosis and other non-speech modes never ask. Whisper, Shout and Close keep their ranges and audiences: only NPCs the fallback could already choose are offered.

Actor data is copied on the game thread, the request runs on the thread pool, and the turn resumes on the game thread after the reply or 2 seconds, whichever comes first. The request uses the existing game-data transport with a 2-second total deadline covering connect, send and reply, a 16 KB reply limit and no retry. A queued request that starts after the deadline, a newer player input, Stop, an interaction switch or a save load is not sent. The decided NPC is used only if it was offered and is still automatically eligible; otherwise the live router decides as before. A newer player input, Stop, interaction switch, save load, a different parent cell (interior or exterior) or more than 2048 units of player travel during the wait drops the turn instead of speaking late. Only HTTP 404 for the endpoint disables the request until the next load. `not_configured` (the server's dedicated Decision Connector is unset, disabled or not a decision connector), timeouts, errors and malformed replies are retried by the next voice turn, so enabling the Decision Connector takes effect without reloading a save. `[PLAYER-ROUTING] Automatic decision` log lines record the source, reason, form ID and latency.

## Speech trace diagnostics

Search `AIAgent.log` for `[SPEECH_TRACE]` and an `utterance_id` from HerikaServer's `log/chim.log`. Records use the existing optional ScriptQueue ID field; older server lines without an ID still play normally but cannot be correlated end to end.

`received` means the parsed line reached the speech manager. `queued` and `dequeued` show queue admission and waiting time. Downloads have start/end markers and byte counts. `playback_started` is logged only after the audio engine accepts playback. `completed`, `interrupted`, `cancelled`, `skipped`, and `failed` report distinct outcomes. `subtitle_only_completed` means the existing audio-failure fallback displayed timed subtitles instead. `queue_removed` means the line was removed from the queue; active playback may still finish. A cancellation request alone does not prove playback stopped.

The existing speech callback is not proof of audible output. Missing stages remain unknown rather than being classified as success. Trace records contain IDs and bounded metadata, not dialogue or audio. Existing log levels and support-log collection apply.

Durations use the local monotonic clock. Do not compare its absolute value with the server's monotonic clock. No new network requests, protocol fields, settings, or game-state events are introduced.

## CHIM Interact

Interact keeps a valid crosshair target first, then tries the first camera-ray scenery collision within 8192 game units. Terrain without an exact loaded reference remains unsupported; this adds no execution distance gate. Scenery can receive timed fire visuals (no health damage or spread), resize, or removal. Destroy uses authored destruction where available and otherwise disables the exact scenery reference without debris; minor injury and actor emotions are not substituted with destruction.

Timed statuses use direct TargetActor delivery rather than projectile-based Aimed delivery. After updating this feature, restart Skyrim so the matching ESP records and DLL load together; stale delivery records are excluded from available actions.

Every injury applies health damage once, signals Skyrim’s assault response, and requests a moderate stagger if the target survives. Stagger acceptance is recorded separately from observed animation; some actors or animation states can reject it. This is not a full weapon hit and does not trigger weapon enchantments or manually set a bounty.

Every injury applies health damage once, signals Skyrim’s assault response, and requests a moderate stagger if the target survives. Stagger acceptance is recorded separately from observed animation; some actors or animation states can reject it. This is not a full weapon hit and does not trigger weapon enchantments or manually set a bounty.

Player context includes level, all 18 named current skills, armor rating, current and maximum health/stamina/magicka, combat/sneak state and up to eight active effects. Player maxima exclude accumulated damage while retaining temporary fortify modifiers; indefinite effect expiry is reported as unknown.

Any selected item can be a narrative prop for supported synthetic effects; **No item** also works. Interact grants supported mechanics by default; actual engine restrictions still apply. Healing/stamina/magicka restore 1–100 points without consuming the prop. Real inventory consumption remains a separate action. Actor-only poison, burning, paralysis, calm, fear and frenzy last 5, 10, 20 or 30 seconds and refresh the same CHIM status instead of stacking. Skyrim owns active-effect expiry and save/load persistence; resisting or immune targets may prevent application. Up to five dependent outcomes can execute, each narrated only after its own receipt.

Interact uses the unrestricted intent policy without a mode toggle. It cannot fabricate successful engine outcomes.

In **Prompt Manager**, search `interact_` to edit interaction rules or narration guidance. Action descriptions remain code-owned. Clear a custom prompt to restore its default. Eligible actions, numeric limits and receipt validation remain enforced.


Describe an action and press **Enter** to interact; **Shift+Enter** adds a line. **Choose…** opens the optional inventory picker, with exact-copy details and an amount for stacks. **Esc** closes the picker first, then the menu. Submitting an action authorizes it without an additional confirmation dialog.

To eat or drink a world item, aim at a single loose food item or non-poison potion, leave **No item** selected, and describe eating/drinking it. CHIM transfers the actual reference and uses Skyrim's player consumption path, checking transfer and consumption counts. Owned items send the normal theft alarm. World stacks and references with enchantment/poison extras are excluded. Existing inventory-food-to-NPC consumption is unchanged; selecting inventory food does not silently make the player consume it.

For actions on a living NPC active in CHIM, that exact NPC can respond after the Narrator finishes. The reply is generated alongside Narrator audio, then held until narration completes. It uses the recorded outcome, including failure or uncertainty. Interrupted narration, cancellation, save/load, CHIM Off or a removed/dead target suppresses the reply. An unmanaged target produces a notice to activate that exact actor in CHIM; another same-name actor is never substituted. Narration is split into streamed sentence audio. Replies wait for every narration chunk to finish and the audio stream to complete; incomplete playback never confirms the outcome as spoken. Pending replies expire after two idle minutes, refreshed by narration completion.

For a single loose, playable inventory item such as bread, a weapon or armor, the Director can request **pickup**: the engine takes one item from the captured world reference into the player inventory. CHIM checks the inventory increase and world-reference change before reporting success. Pickup is issued at most once; later effects on that world target are skipped. Quest-bound references and world stacks are excluded. The actual reference transfers through Papyrus; owned items send the theft alarm.

The repeatable **CHIM Interact** lesser power is granted when CHIM connects after a new game or save load. Aim at a loaded actor or object, cast the power, describe the attempt, and optionally select an exact inventory copy and quantity. **No item** is selected by default, including with an empty inventory; quantity is disabled for itemless actions. Interact does not track distance or impose a range limit; the captured target must remain loaded and valid. Prisma UI is required. Quest inventory items can be shown or used without removal; transfer and consumption are excluded. Search filters the entire captured inventory; enchanted or tempered copies show their instance details.

The optional **Magic** selector is independent of the item selector. It lists playable known spells and powers, plus the highest unlocked rank of each known shout. CHIM casts the selected authored magic once without equipping, learning, unlocking, spending resources or changing cooldowns. Concentration spells receive one scripted application. A new exact spell effect or an authored-direction health change can confirm application; unobservable results remain unknown. Modded Magic-menu list parity and actual casting still require in-game verification.

Additional actor effects include frost (equal health/stamina damage plus slow), shock (equal health/magicka damage), resource drains and absorption to the player, slow/haste, armor and weapon-damage modifiers, ethereal state, soul trap, reanimation, turning undead, and banishing summoned Daedra. Timed effects last 5, 10, 20 or 30 seconds; persistent authored spells let Skyrim handle expiry and save/load. Temporary stat modifiers use engine recovery, not delayed manual restoration. Speed/weapon percentages use the current stat; armor uses points. Frost's slow component uses the requested value as SpeedMult points. Exact active components must be observed; resistance, immunity and vanilla target conditions can prevent application. Reanimation requires a corpse commanded by the player after casting; it is not resurrection. Soul trap only confirms the trap is armed, never a captured soul. Standalone stagger sends one graph request and observes state for up to one second without adding injury.

**Extinguish**, **neutralize poison** and **release paralysis** remove only CHIM's matching effects. Dispel chooses from up to eight captured, timed, nonscripted spells and revalidates exact active instances before removal; abilities and permanent effects are excluded. Already-absent effects report no change. Cleanup never sweeps another mod's effects by appearance.

Loose playable, nonquest single objects can be thrown toward/away from the player or upward, rotated by at most 180 degrees on a selected axis, or moved 1–256 units relative to the player's heading/world vertical axis. Movement requires the same loaded cell, clear point rays and floor support; these checks do not guarantee full bounding-box or navigation clearance. Actors, doors and static buildings are excluded. Frost/shock scenery visuals use engine-registered shaders; **impact burst** is a one-second spark/shock visual, not a physics impact, explosion or damage. Each effect is issued once, with unknown outcomes preserved rather than retried.

Selected-item context includes per-item engine gold value (not a merchant price), weight, weapon class/base damage or armor class/base rating, exact enchantment/charge/tempering/poison details, and bounded authored potion, ingredient or scroll effects. Ingredient properties may be undiscovered; none are presented as currently active. Base damage and armor are not final combat calculations.

The paired HerikaServer uses the configured Director connector. It sends relevant item/target/player facts and up to ten recent target events, validates at most five conditional effects, then records the accepted attempt and awaits its execution receipt. A valid empty plan plays a truthful, gently humorous failure scene with no mechanical effects, followed by the eligible target’s response after narration. Invalid or expired requests remain suppressed. The configured Narrator describes confirmed effects after execution. Failed or uncertain steps never use their proposed success narration. Kill and disable run from the submitted intent without a second confirmation. Disable uses the native engine reference API and verifies the captured reference’s disabled flag, bypassing the Papyrus enable-parent restriction without unlinking or disabling the parent. An unconfirmed flag remains an unknown result.

Supported operations are observation, actor/container transfer, real potion/food consumption, equipment, resolved health damage, confirmed killing, impulses, locks, activation/open/close, authored destruction or explicit reference disabling, bounded scaling, combat, and selected single-target scrolls with supported authored effects. Scroll support includes single-effect health/stamina/magicka modifiers, paralysis, calm, fear and frenzy; self-cast, area/explosion, conditional and other archetypes are excluded. Vanilla Ice Spike has an additional stagger effect and is excluded. Supported scrolls use their authored magic; resistance or a missed projectile can leave application unconfirmed. Direct injury does not simulate a normal weapon hit. Scaling does not promise updated collision. Shared actor-base protections are not changed for nonunique actors; quest alias protections can prevent killing and are reported as failure.

Healing and stamina/magicka restoration directly restore 1–100 points to a living actor and verify an increase. No item is consumed; a full statistic returns failure without inventing a benefit. Disarm captures a particular equipped hand (0 right, 1 left), unequips and drops that exact weapon. Unequip captures a supplied armor slot (30–61), leaving armor in the NPC inventory; changed, quest-bound or ambiguous equipment is rejected. Drop removes the selected quantity at the player. Place drops it near the captured target in the same cell, without promising precise surface placement or stable physics. Both check the actual returned world reference and inventory decrease.

Equipment success requires a freshly observed worn extra-data instance, not merely another equipped copy of the same base item. Ambiguous existing copies are reported as a partial transfer. Consumption requires engine acceptance and an inventory decrease; narration confirms consumption without claiming an unmeasured healing or magic effect. Lock success checks the requested level, and combat success checks that the opponent is the player. Generic activation narration confirms activation only.

Transfers preserve the selected extra-data instance. If destination merging makes the exact instance ambiguous, the transfer is recorded but follow-up consumption/equipment is refused. Killing and disabling are authorized by the submitted action. The client revalidates the captured target, inventory and session; it never substitutes a nearby actor. A missing callback stops the sequence after 15 seconds, marks the result uncertain, and does not repeat effects. Outcome reporting may retry once without repeating game actions. Save/load invalidates pending work.

Implementation: `Plugin/ItemInteraction.cpp`, `CHIMItemInteraction.psc`, `PrismaUI/views/CHIM/item_interaction.*`, and HerikaServer `item_interaction.php` / `lib/item_interaction.php`. `Plugin/tools/build_interact_records.py` documents and verifies the two source ESP records without modifying existing records. Compile the Papyrus script and deploy it together with the ESP, native DLL and Prisma files. Source PRs do not contain compiled DLL/PEX artifacts.

### In-game checks

1. Load a disposable save with the paired client/server. Confirm **CHIM Interact** appears under Powers and can be cast repeatedly.
2. Aim at an NPC, select a food item, and describe showing it. Confirm one attempt and one outcome in Event Log, configured Narrator audio/subtitles, and no inventory change.
3. Give one of two differently enchanted or tempered copies. Verify the intended copy moves, the other stays, and quantity is correct. Repeat with a container.
4. Administer a real healing potion to an injured NPC. Verify inventory consumption and healing. Try equipping eligible equipment.
5. Try injury followed by combat, a lock/unlock, a door open/close, and a physics impulse. Confirm only verified effects are narrated as successful.
6. Try killing a protected unique NPC or disabling an object on a disposable save. Submitting authorizes execution without another confirmation; compare actual state with its outcome. Restore the save afterward.
7. Try an ordinary apple as a shrinking prop: the default intent policy should select supported scaling. Inspect the actual scale and collision manually.
8. Use an eligible elemental scroll, including against a resistant target. Confirm the scroll is consumed once and uncertain application is not narrated as a hit.
9. During resolution move away, drop the selected copy, unload the target, or load another save. No stale effects or speech may play in the new session.
10. Check Escape, Tab/Shift+Tab, text typing, quantity limits, empty inventory, search, and repeated submission. Gameplay hotkeys must return immediately on close. Repeat relevant input and power tests in VR before claiming VR gameplay validation.

Build, lint, local deployment, and in-game behavior are separate evidence. These checks require Skyrim; a successful build does not establish them.
