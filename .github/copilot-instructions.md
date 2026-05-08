# CHIM Copilot Instructions

- This repository is the merged source of truth for CHIM.
- `AIAgent/` contains the Skyrim mod package content: the ESP, Papyrus source in `AIAgent/Source/Scripts/`, compiled Papyrus in `AIAgent/Scripts/`, PrismaUI, interface files, optional patches, sound assets, textures, fomod packaging, and the packaged plugin DLL in `AIAgent/SKSE/Plugins/AIAgent.dll`.
- `Plugin/` contains the native C++ SKSE plugin that builds `AIAgent.dll`.

- Do not treat `aiagent-aiff` or `HerikaAI-NG` as active source-of-truth repos. Use `AIAgent/` and `Plugin/` paths and CHIM naming in code, docs, and reviewer-facing text.
- The default branch is `main`.

- This repository intentionally tracks shipped assets and build products that are part of the mod package, including `.dll`, `.pex`, `.swf`, `.wav`, and `AIAgent.esp`. Do not remove or ignore them unless the change is an explicit packaging decision.
- Local/generated plugin development artifacts should stay uncommitted. `Plugin/build/`, `Plugin/.vs/`, and `Plugin/install/` are generated. `Plugin/.vscode/launch.json` is intentionally tracked.

- If you change native plugin code in `Plugin/*.cpp` or `Plugin/*.h`, keep the packaged `AIAgent/SKSE/Plugins/AIAgent.dll` aligned with the intended shipped build.
- If you change Papyrus source in `AIAgent/Source/Scripts/`, keep the corresponding shipped `.pex` files in `AIAgent/Scripts/` aligned when the change is meant to ship.
- If you change PrismaUI or in-game UI behavior, check both `AIAgent/PrismaUI/views/CHIM/` and the native bridge code in `Plugin/PrismaUIBridge.cpp`.
- If you change interface/menu assets, check `AIAgent/Interface/` and `AIAgent/Optional/`.
- If you change installer or package layout, check `AIAgent/fomod/`.

- Build the native plugin from `Plugin/` on Windows using Visual Studio 2022, CMake, and vcpkg:

```powershell
cd Plugin
cmake --preset release
cmake --build build/release
```

- If `cmake --preset release` fails because the build directory was copied from an older repo path, delete `Plugin/build/release/` and configure again.
- After plugin changes, validate by building the release preset successfully.
- After Papyrus or packaging changes, validate that the shipped files under `AIAgent/` still match the intended release contents.

- Prefer focused edits over broad cleanup. Trust these instructions first and only search more widely when the local context is incomplete.
