# CHIM

CHIM is the unified monorepo-facing home for the Skyrim client mod assets and the native SKSE plugin that power the CHIM and Herika stack.

This repo replaces the split source-of-truth model that previously lived across:

- `aiagent-aiff` for the client mod, Papyrus, PrismaUI, interface files, and packaged assets
- `HerikaAI-NG` for the native plugin that builds `AIAgent.dll`

## Repository Layout

- `AIAgent/`
  - Former `aiagent-aiff` content.
  - Contains the ESP, Papyrus source and compiled scripts, PrismaUI, interface files, optional patches, SKSE payloads, and packaged client assets.
- `Plugin/`
  - Former `HerikaAI-NG` content.
  - Contains the native C++ plugin sources, CMake presets, build output, and the code that produces `AIAgent.dll`.

## Build and Deploy

From the monorepo root:

```powershell
.\scripts\build-chim-plugin.ps1
.\scripts\deploy.ps1
```

`build-chim-plugin.ps1` now builds from `CHIM/Plugin`.

`deploy.ps1` now sources CHIM client assets and Papyrus files from `CHIM/AIAgent`, and sources the plugin build from `CHIM/Plugin`.

Runtime deploy destinations remain the same as before, so local installs and HerikaServer deploys do not need to change.

## Source Of Truth

For CHIM and Herika-related builds and deploys inside this monorepo:

- use `CHIM/AIAgent` instead of `aiagent-aiff`
- use `CHIM/Plugin` instead of `HerikaAI-NG`

The old top-level repos can remain as legacy references during migration, but the active deployment pipeline now targets `CHIM`.
