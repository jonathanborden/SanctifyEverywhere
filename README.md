# SanctifyEverywhere

A mod for **Deadzone: Rogue** that brings the weapon-blessing (Sanctify) mechanic to every zone — not just Zone 4.

## What it does

In the base game, the **Nanite Forge** (sanctify anvil) only appears in Zone 4. This mod makes the full blessing experience available in Zones 1–3:

- **Spawns a working Nanite Forge** next to every fabricator in Zones 1–3
- **Enables weapon blessings** at those forges, exactly like Zone 4 (consumes Nanite Vials)
- **Adds the Nanite Vial counter to the HUD** in every zone, so you can always see your vial count
- Zone 4 is untouched — the mod detects it and stays out of the way
- Forges appear within a few seconds of each level loading; no key presses needed

## Installation

1. Download and extract the zip.
2. Copy `dwmapi.dll` into the game's binaries folder:
   ```
   <Steam>\steamapps\common\Deadzone Rogue\Valhalla\Binaries\Win64\
   ```
3. Launch the game normally. That's it.

## Uninstallation

Delete `dwmapi.dll` from the `Win64` folder above.

## How it works (and safety notes)

- The mod is a **proxy DLL** (`dwmapi.dll`): the game loads it automatically at startup, it forwards all real DWM calls to Windows, and runs the mod logic alongside the game.
- **No game files are modified** and nothing is written to your save in any unusual way — the mod only spawns the same forge actor and HUD widget the game itself uses in Zone 4.
- Deadzone: Rogue is a single-player/co-op game without kernel anti-cheat; still, use at your own risk.
- Some antivirus tools may flag an unsigned proxy DLL as suspicious — that's a false positive inherent to this injection technique. Build from source if you'd rather trust your own compiler.

## Compatibility

- Built and tested against Deadzone: Rogue (Unreal Engine 5.5.4), game update of 2026-06-04.
- The mod resolves engine structures dynamically at runtime (no hardcoded code addresses), so it is designed to survive small game patches. A large update could still break it — if the forge stops appearing after a patch, check for a mod update.

## Building from source

Requirements: Visual Studio 2022 (Community is fine) with the C++ desktop workload.

```bat
build.bat
```

or from a *x64 Native Tools Command Prompt*:

```bat
cl.exe /O2 /LD /EHsc /std:c++17 src\dllmain.cpp src\proxy.cpp src\mod.cpp src\tick_hook.cpp ^
  /Fe:build\dwmapi.dll /link /DLL /DEF:src\dwmapi.def user32.lib kernel32.lib
```

Output: `build\dwmapi.dll`.

### Diagnostics

Logging is disabled in release builds. To enable it, set `SANCTIFY_LOGGING` to `1` in `src/mod.h` and rebuild — the mod will then write `SanctifyMod.log` (and `SanctifyMod_proxy.log`) next to the DLL in the game's `Win64` folder.

## License

MIT — see [LICENSE](LICENSE).
