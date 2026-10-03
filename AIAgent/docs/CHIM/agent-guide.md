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


## NPC schedules

The NPC editor's Schedules tab creates, edits, cancels and deletes appointments. Select a recognised location, an in-game day number and appointment time. Daily repetition is 24 game hours; zero means one time. Destination validation requires the paired server and CHIM scripts connected to the game. Saved schedules stay pending until the game resolves a persistent arrival marker; unknown or ambiguous AI destinations remain reminders with a clarification request.

Travel starts three game hours early. Routine progress checks run hourly; the worker checks departure and arrival deadlines on each service tick using the accepted game clock. Early arrivals wait. At the deadline CHIM checks the actor's actual location and moves late actors to the validated marker. Combat/dialogue can defer execution. Visits finish after arrival, stays after their duration, and duties require an AI-reported outcome. Repetition starts only after releasing the previous activity. Overlapping travel windows are rejected; multiple recurring schedules require matching intervals.

CHIM Off blocks new schedule commands. An already installed travel package can continue until it is released. Cancellation/deletion waits for the game's release acknowledgement; Retry resends a stalled operation. Timeline changes invalidate old occurrences after release. Load the corresponding game and server saves together. Dynamic references and ambiguous same-name AI duties are rejected. A valid marker does not prove navmesh reachability: verify travel, waiting, teleport and package restoration in Skyrim before release.

The paired protocol uses `BackgroundCmd@actor@Schedule/run/token/operation/destination/issuedDays` and `util_npc_schedule` replies containing `run/token/actor/operation/result/marker`. Operations are validate, travel, check, ensure and release. The client restores only its owned travel override and link, and the server accepts only the pending operation's matching token, actor and clock epoch. Compile CHIMSchedule.psc alongside AIAgentAIMind.psc when building the client payload.
## Speech trace diagnostics

Search `AIAgent.log` for `[SPEECH_TRACE]` and an `utterance_id` from HerikaServer's `log/chim.log`. Records use the existing optional ScriptQueue ID field; older server lines without an ID still play normally but cannot be correlated end to end.

`received` means the parsed line reached the speech manager. `queued` and `dequeued` show queue admission and waiting time. Downloads have start/end markers and byte counts. `playback_started` is logged only after the audio engine accepts playback. `completed`, `interrupted`, `cancelled`, `skipped`, and `failed` report distinct outcomes. `subtitle_only_completed` means the existing audio-failure fallback displayed timed subtitles instead. `queue_removed` means the line was removed from the queue; active playback may still finish. A cancellation request alone does not prove playback stopped.

The existing speech callback is not proof of audible output. Missing stages remain unknown rather than being classified as success. Trace records contain IDs and bounded metadata, not dialogue or audio. Existing log levels and support-log collection apply.

Durations use the local monotonic clock. Do not compare its absolute value with the server's monotonic clock. No new network requests, protocol fields, settings, or game-state events are introduced.
## Relationship history

Recent Context and Context History display `relationship` Eventlog entries from
HerikaServer. New relationships show a signed affinity; later affinity changes
show a signed difference and explanation. Initial profile setup is omitted.
These entries do not enter ordinary conversation prompts.

Use the paired relationship update in HerikaServer when testing against a
database previously used by the RefID feature. It accepts unique legacy NPC
profile hashes without rewriting stored identity data. Ambiguous or unknown NPC
targets are rejected rather than answered by the Narrator. This client change
does not install the separate RefID feature; mixed-version RefID compatibility
still needs its own validation before release.
