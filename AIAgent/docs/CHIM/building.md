# Building CHIM

## Native plugin

Build from a clone of [CHIM](https://github.com/Dwemer-Dynamics/CHIM), not an extracted release. Use Windows, a Visual Studio C++ toolchain supporting C++23, Windows SDK, CMake 3.21 or newer, Ninja and bootstrapped vcpkg. Set `VCPKG_ROOT` to that vcpkg checkout. Dependency versions/registry pins are in `Plugin/vcpkg.json`, `Plugin/vcpkg-configuration.json` and `.gitmodules`; keep the recorded submodule revisions.

Run in an **x64 Developer PowerShell** from the CHIM repository root:

```powershell
git submodule update --init --recursive
# Use a scratch destination: the CMake helper copies the DLL into its Data folder.
$env:CMAKE_SKYRIM_FOLDER = Join-Path $env:TEMP 'chim-build-stage'
New-Item -ItemType Directory -Force (Join-Path $env:CMAKE_SKYRIM_FOLDER 'Data') | Out-Null
Push-Location Plugin
cmake --preset release -DBUILD_TESTING=ON
cmake --build build/release
ctest --test-dir build/release --output-on-failure
Pop-Location
```

Stop on a failed command before continuing. The `release` preset uses Ninja, `cl.exe` and `x64-windows-static-md`. The output is `Plugin/build/release/AIAgent.dll`; the SkyrimScripting CMake helper also copies the output into the selected `Data/SKSE/Plugins` directory. Do not point it at a live game installation unless that deployment is intended. The scratch Data directory is a build destination, not a playable Skyrim installation.

`Plugin/CMakeLists.txt` enables CommonLib's compatible prebuilt path; dependency/toolchain changes may require a source dependency build. Do not replace pinned libraries with arbitrary current versions to make a build pass. A configuration failure is not a reason to change Skyrim compatibility defaults.

The root README also lists parent-monorepo wrappers (`scripts/build-chim-plugin.ps1`, `scripts/deploy.ps1`). They are maintainer tooling outside this public checkout, not prerequisites for the commands above.

## Papyrus and other payloads

Papyrus sources are under `AIAgent/Source/Scripts/`; installed compiled scripts are under `AIAgent/Scripts/`. Native CMake does not compile Papyrus or build `AIAgent.esp`. For a changed script, use the Skyrim Creation Kit Papyrus compiler with the correct game/SKSE and referenced mod source imports; verify every external type before compiling. Keep the `.psc` signatures aligned with registrations in `Plugin/Papyrus.cpp`. Install the resulting `.pex` only into the intended test mod. The maintainer's import list contains local dependencies and is not a portable build command.

PrismaUI sources are shipped under `AIAgent/PrismaUI/`. Check JavaScript syntax and exercise the affected interface, including keyboard focus, when changing it. ESP changes require the matching game/editor tooling and separate save/load testing.

## Before a draft PR

1. Run `git diff --check` and the checks relevant to the changed layer. The CTest targets cover selected native policies, not all game behavior.
2. For deployment testing, record source commit, DLL hash and x64 architecture; confirm the active mod manager uses that file. Test SE/AE/VR variants affected by the change, or report them untested.
3. Keep generated DLLs, PDBs, PEX files, archives, credentials and local configuration out of source PRs. Target `unstable` unless a maintainer directs otherwise.

## Documentation packaging check

Copy the canonical `AIAgent/docs/CHIM/` files unchanged into the archive payload. Verify the FOMOD's required `docs/CHIM` folder and `AIAgent/README.md` survive every installer choice. Extract a staged archive and follow all relative documentation links. This is a packaging check, not proof of an installed game session.
