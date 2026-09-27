# CHIM Agent Notes

## Start here

- This repository owns CHIM's Skyrim client: `Plugin/` builds the SKSE (Skyrim Script Extender) DLL; `AIAgent/` is the installable mod payload.
- Read the [agent guide](AIAgent/docs/CHIM/agent-guide.md) for source locations, configuration and diagnostics, [building guide](AIAgent/docs/CHIM/building.md) before building, and [custom plugins guide](AIAgent/docs/CHIM/custom-plugins.md) before adding an integration.
- The backend is [HerikaServer](https://github.com/Dwemer-Dynamics/HerikaServer). Provider credentials, prompts, memory and database changes belong there, not in the client.
- Inspect the checkout's branch, changes and nearest instructions first. Open source PRs against `unstable` unless directed otherwise; preserve unrelated edits and keep generated DLLs, PEX files and archives out of source PRs.
- Installed files are not a complete build checkout. Match the installed version to its source revision before diagnosing behavior from newer code.
- Preserve saves, user configuration and load order. Building, deploying and testing in Skyrim are separate operations; report which actually ran.
- The canonical offline guides live in `AIAgent/docs/CHIM/`. Update them in place and retain the FOMOD required-folder entry; do not create a second copy of their content.

## Paired settings and input

- The in-game Settings hub must stay aligned with the HerikaServer Global Settings, Profiles, and CHIM NPC APIs.
- When HerikaServer adds, removes, renames, or changes a Global Settings/Profile field, update and validate `AIAgent/PrismaUI/views/CHIM/config_manager.*` in the paired change.
- Keep CHIM NPC editing routed through the existing `npc_manager.*` view and `chim_npc_manager.php` API instead of creating a second editor.
- Validate keyboard capture is enabled only while an editable control owns focus and is released when the Settings hub closes or switches tabs.
