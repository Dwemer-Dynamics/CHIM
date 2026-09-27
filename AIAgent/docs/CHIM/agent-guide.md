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
