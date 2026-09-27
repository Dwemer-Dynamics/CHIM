# Making custom CHIM plugins

Choose the smallest integration surface before changing CHIM itself.

| Goal | Start with |
|---|---|
| Add prompt context, provider-side behavior or stored extension state | [HerikaServer custom plugin guide](https://github.com/Dwemer-Dynamics/HerikaServer/blob/unstable/docs/custom-plugins.md) |
| Read another Skyrim mod's state and send it to the server | [CHIM-Custom](https://github.com/Dwemer-Dynamics/CHIM-Custom), including its `SkyrimPlugin/` source and server hooks |
| Connect an external tool/service | [CHIM-MCP](https://github.com/Dwemer-Dynamics/CHIM-MCP) and [CHIM-Twitch-Bot](https://github.com/Dwemer-Dynamics/CHIM-Twitch-Bot) as separate integration examples |
| Add a game-side command | CHIM's `Plugin/Commands.cpp`, `Plugin/Papyrus.cpp` and `AIAgent/Source/Scripts/AIAgentFunctions.psc` at the target revision |

Examples are maintained projects, not a promise that every interface is stable across CHIM releases. Pin and test compatible client/server versions. Keep custom plugin source in its own repository; the server's `ext/<name>/` is an installation destination.

## Game-side integration

1. Inspect the current Papyrus declarations and native registrations together. `AIAgentFunctions.sendMessageToActor` accepts an actor; name-only routing is unsuitable when actors share a name.
2. For external actions, inspect the `ExtCmd` branch in `Plugin/Commands.cpp`. Commands shaped as `ExtCmd<BridgeScript>_<Action>` attempt a static `DispatchExternalCommand` call with NPC name, full command and parameter strings. Check the current fallback and function-result flow before implementing the bridge.
3. Keep your bridge and optional mod detection separate from CHIM's built-in scripts. Do not replace `AIAgentFunctions.pex` or claim a new native function exists without the matching DLL registration.
4. Test missing dependencies, invalid/unloaded targets, save/load, cancellation and the result reported to the server. Engine actions must respect the client thread/actor lifetime rules.

Older server examples may mention `SPG_CommandReceived` or `SPGPapFunctions`; verify them against current native/Papyrus code instead of copying their historical signatures.

## Shipping a paired server extension

Current CHIM discovers server packages under `Data/CHIM/server-plugins/<package>/<version>.dwpkg` through `Plugin/ServerPluginSync.cpp`. These contain server files, not game DLLs. The server verifies and installs them with `lib/plugin_package_manager.php`.

[CHIM-Custom's README](https://github.com/Dwemer-Dynamics/CHIM-Custom/blob/main/README.md) documents its current `scripts/build-dwpkg.ps1` and `scripts/build-release.ps1` workflow. Use it as a packaging example in that repository; those scripts are not part of CHIM itself. Ship game files through the ordinary mod layout and server files through the supported server package format. See the [server guide](https://github.com/Dwemer-Dynamics/HerikaServer/blob/unstable/docs/custom-plugins.md) for manifests, checksums, hooks, persistent paths and migration tests.

Include a README and scoped agent instructions in your own plugin, with source links and compatible versions. Never bundle API keys, user databases, saves or generated dialogue. A package upload/installation result is separate from proof that the extension works in Skyrim.
