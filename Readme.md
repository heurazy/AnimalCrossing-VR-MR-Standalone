# Animal Crossing VR/MR Standalone

Standalone **Meta Quest 3 / Quest 3S** VR and mixed-reality build for **Animal Crossing (Nintendo GameCube, USA Rev 0 / GAFE01)**.

This project is based on DolphinXR and adds a game-specific Quest frontend, OpenXR VR rendering, tabletop mixed reality, Quest Touch input, and Animal Crossing camera/culling fixes.

> **No ROM or Nintendo game data is included.** You must supply your own legally obtained dump of **Animal Crossing USA Rev 0 (Game ID: GAFE01, revision 0)**.

## Download

Install the APK from the latest GitHub Release on your Quest 3 / Quest 3S.

The application starts in a custom Animal Crossing VR launcher:

1. Press **Select ROM**.
2. Choose your own Animal Crossing USA Rev 0 disc image.
3. Press **Launch Animal Crossing VR**.
4. Use the gear icon for VR settings.

The selected ROM is remembered using Android's persistent document permission and is not copied into the application.

## Main features

- Standalone Quest Android APK; no PC required after installation.
- Native OpenXR stereo presentation with Quest head tracking and 6DoF.
- **Tabletop mixed-reality mode enabled by default**.
- Quest passthrough / AR enabled by default in tabletop mode.
- Passthrough automatically turns off in normal immersive VR camera mode.
- Right-stick click toggles **Tabletop MR ↔ immersive VR camera** at runtime.
- Two-hand tabletop manipulation:
  - hold both grips to move the diorama;
  - rotate the hand-to-hand line to rotate the board;
  - spread/pinch hands to zoom.
- Smoothed Animal Crossing acre transitions for the stabilized tabletop camera.
- Extended renderer far clip and game-specific actor/display-list culling fixes.
- Animal Crossing camera stabilization so the diorama does not follow the original game camera.
- Runtime OpenXR/Meta hand mesh in tabletop mode, rendered white.
- Hand clipping/occlusion against the tabletop surface.
- Single fused VR presentation for 2D menus/HUD instead of duplicated left/right UI.
- Quest Touch → GameCube controller mapping installed automatically on first use.
- Custom lightweight launcher instead of the normal Dolphin game library UI.

## Supported game image

The launcher intentionally validates the selected image and accepts only:

- **Game:** Animal Crossing
- **Region/version:** USA Rev 0
- **Game ID:** `GAFE01`
- **Revision:** `0`

Supported disc-image containers include `.gcm`, `.iso`, `.ciso`, `.gcz`, `.wia`, and `.rvz`.

## Controls

Default Quest Touch mapping:

- Left stick: GameCube Main Stick / movement
- Right stick: GameCube C-Stick
- A / B / X / Y: corresponding GameCube face buttons
- Left trigger: L
- Right trigger: R
- Left Menu button: Start
- **Right stick click: toggle Tabletop MR / immersive VR camera**
- OpenXR haptics: controller vibration

The default mapping is installed once. User remaps are preserved afterwards.

## Install on Quest

Enable Developer Mode and USB debugging on the headset, then either use a sideloading tool or ADB:

```powershell
adb install -r .\AnimalCrossingVR-Quest3-v0.1.0.apk
```

The APK does not require the ROM to be bundled with it. Select the ROM from the launcher after installation.

## Build from source

### Requirements

- Windows 10/11
- Git
- Android SDK
- Android NDK `29.0.14206865`
- CMake `3.22.1`
- JDK 17+

Clone recursively:

```powershell
git clone --recursive https://github.com/heurazy/AnimalCrossing-VR-MR-Standalone.git
cd AnimalCrossing-VR-MR-Standalone
```

Build the sideloadable Quest APK:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-quest.ps1
```

The output is written to:

```text
dist\AnimalCrossingVR-Quest3.apk
```

The public build uses Dolphin's `questReleaseNoMinify` variant, which keeps the release package ID while using a debug signing key for direct sideloading. It is not intended for Play Store submission.

## Project architecture

- **VR / emulation runtime:** DolphinXR / Dolphin Emulator
- **Quest renderer:** Vulkan + OpenXR
- **Animal Crossing code reference:** ACreTeam/ac-decomp (`GAFE01_00`)
- **Target ABI:** Android `arm64-v8a`
- **Primary target:** Meta Quest 3 / Quest 3S

The project is a specialized DolphinXR runtime, not a native ARM64 recompilation of the entire GameCube game. `ac-decomp` is used as an authoritative reference for game-specific structures, camera behavior, and patches.

## Source provenance

This repository was developed from DolphinXR commit:

```text
85c4266b29eb308d173fefa0f21f5cb77df3516a
```

DolphinXR itself is derived from Dolphin Emulator. See `COPYING`, `LICENSES/`, and individual source-file SPDX headers for licensing details.

Animal Crossing and all related trademarks, game code, artwork, audio, and assets are property of their respective rights holders. This repository does **not** distribute Nintendo copyrighted game data.

## Status

This is an experimental community preview. Expect game-specific VR/MR rendering edge cases and report reproducible issues with headset model, game revision, and steps to reproduce.
