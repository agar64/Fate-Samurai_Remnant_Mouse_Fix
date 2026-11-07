# Fate/Samurai Remnant — Raw Mouse Camera Input Fix

A lightweight `dinput8.dll` proxy mod for the Windows PC version of **Fate/Samurai Remnant** (Steam AppId `1902690`, Build `15597839`) that eliminates the camera's speed deadzone, negative mouse acceleration, 8-bit quantization stepping, and 1-tick polling latency.

---

## Root Cause Summary

Static and decompilation analysis of `FateSamuraiRemnant.exe` identified four compounding stages in the game's mouse-to-camera pipeline (documented in detail in [`docs/investigation.md`](docs/investigation.md)):

1. **1-Tick Polling Order Latency (`FUN_142357780` at RVA `0x2357780`):**
   - Each tick, the mouse-pad update function calls `FUN_142356d90` (which reads `lX` and `lY` from `mouse + 0x64` / `mouse + 0x68`) **before** calling `FUN_14141edb0` (which calls `IDirectInputDevice8::GetDeviceState` to populate `mouse + 0x64` / `mouse + 0x68`). Every frame therefore uses the previous tick's mouse delta.
2. **Mouse-to-Stick Velocity Normalization & Saturation (`FUN_142356d90` at RVA `0x2356d90`):**
   - DirectInput relative mouse counts (`lX`, `lY`) are divided by a hardcoded constant `D = 255.0f` (`elem + 0xA0`) and clamped to `[-1.0, +1.0]` into virtual pad axes `4` (Mouse X) and `5` (Mouse Y). Any single-tick movement above `255` counts saturates at `±1.0`, causing negative mouse acceleration on fast flicks.
3. **Signed 8-Bit (`int8_t`) Quantization (`FUN_141f2cfb0` at RVA `0x1f2cfb0`):**
   - All pad axes are quantized to a signed 8-bit integer (`[-127, 127]`) and divided back by `127.0f` at `pad + 0xBC8 + axis * 4`.
4. **Hardcoded `68 / 127` (`~53.5%`) Speed Deadzone in Camera Update (`FUN_141e142c0` at RVA `0x1e142c0`):**
   - The camera update reads Mouse X (`axis 4` at `0x1e14452`) and Mouse Y (`axis 5` at `0x1e145a9`), multiplies by `-127.0f`, truncates to integer `iVar10 = abs((int)(-axis * 127.0f))`, and **discards the entire movement if `iVar10 < 0x44` (`68`)**:
     ```c
     if (67 < iVar10) {
         uVar16 = min(max((float)(iVar10 - 68) / 59.0f, min_val), 1.0f);
         yaw_delta = yaw_speed * fVar32 * uVar16 * sign;
     }
     ```
   - Because `yaw_speed` (`+0x48cb7c`) and `pitch_speed` (`+0x48cb78`) are multiplied **after** the `iVar10 >= 68` threshold check, raising in-game mouse sensitivity increases camera speed once moving without reducing the physical mouse speed required to cross the deadzone.

---

## How the Fix Works

1. **Fixes the 1-Tick Polling Order (`FUN_142356d90` / `FUN_14141edb0`):**
   - Polls DirectInput `GetDeviceState` immediately before `FUN_142356d90` consumes `lX`/`lY` so the current frame's physical mouse movement is applied on the same frame.
2. **Unclamps DirectInput `lX`/`lY` Deltas (`FUN_14141edb0` at RVAs `0x141eeaa`, `0x141eed8`):**
   - Prevents the window-edge virtual cursor clamp from overwriting the raw relative counts at `mouse + 0x64` (`lX`) and `mouse + 0x68` (`lY`), while keeping the virtual cursor (`mouse + 0x90` / `mouse + 0x94`) clamped to the window for menu navigation.
3. **Feeds Continuous Unquantized Mouse Deltas (`FUN_140062df0` at RVA `0x62df0`):**
   - For `axis 4` (Mouse X) and `axis 5` (Mouse Y), returns `raw_counts * (127.0f / (255.0f * 59.0f)) * SensitivityScale` whenever the engine's own window focus, client-area, and pad-enable gates are active.
4. **Bypasses the `68 / 127` Speed Deadzone (`FUN_141e142c0` at RVAs `0x1e14469`, `0x1e145ae`):**
   - Replaces the integer truncation and `< 0x44` deadzone branch for Mouse X and Mouse Y with direct floating-point multiplication by `yaw_speed` / `pitch_speed`, jumping directly to the game's Horizontal and Vertical Camera Inversion checks.

---

## Installation & Removal

### Option A: Standalone Installation (No Ultimate ASI Loader)
Copy the two files from `build\dist\` into your `Fate/Samurai Remnant` game folder (next to `FateSamuraiRemnant.exe`):
- `dinput8.dll`
- `fsr_mouse_fix.ini`

### Option B: Alongside `FateSamuraiRemnantFix` / Ultimate ASI Loader
If you are using Ultimate ASI Loader (`dinput8.dll`) for plugins such as Lyall's `FateSamuraiRemnantFix.asi`:
1. Keep Ultimate ASI Loader's `dinput8.dll` and `FateSamuraiRemnantFix.asi` in the game folder.
2. Copy the following two files from `build\dist\` into the game folder:
   - `FSRMouseFix.asi`
   - `fsr_mouse_fix.ini`

### Uninstallation
Delete `FSRMouseFix.asi` (or `dinput8.dll`) and `fsr_mouse_fix.ini` from the game folder. No game files are ever modified on disk.

---

## Configuration (`fsr_mouse_fix.ini`)

| Option | Default | Description |
| :--- | :---: | :--- |
| `EnableFix` | `1` | Master toggle (`1` = enable camera fix, `0` = passthrough only). |
| `SensitivityScale` | `1.0` | Additional multiplier on top of the in-game *Mouse Sensitivity* setting. |
| `FixPollingOrder` | `1` | Polls DirectInput before `FUN_142356d90` reads `lX`/`lY` to remove 1-tick latency. |
| `UnclampDirectInput` | `1` | Keeps raw `lX`/`lY` unclamped by the virtual cursor bounding box. |
| `ScaleByFrameStep` | `0` | Multiplies mouse delta by the 60Hz frame-step counter (`fVar32`). Leave at `0` for frame-rate-independent 1:1 physical distance response. |
| `EnableLogging` | `1` | Writes diagnostic logs to `%TEMP%\fsr_mouse_fix.log`. |

---

## Building from Source

### Requirements
- Windows 10/11 x64
- Visual Studio 2022 (Desktop development with C++ / MSVC v143 x64 toolset) **or** MSYS2 MinGW-w64 UCRT64 (`g++`)

### Build Command
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```
Output binaries are written to `build\dist\dinput8.dll` and `build\dist\fsr_mouse_fix.ini`.

---

## Compatibility & Safety

- **Target Version:** Validated for Steam Build `15597839` (`FateSamuraiRemnant.exe` PE timestamp `0x66D51D08`).
- **Fail-Safe Byte Verification:** Before modifying a single byte in memory, `dinput8.dll` verifies the exact instruction bytes at all 7 target RVAs after SteamStub unpacking completes. If a game update changes any target instruction, the mod logs the mismatch to `%TEMP%\fsr_mouse_fix.log` and aborts patching cleanly without crashing.
- **Preserved Features:** In-game Mouse Sensitivity slider, Horizontal/Vertical Camera Inversion, menu cursor & mouse buttons, scroll wheel (`axis 6`/`7`), Alt+Tab / window focus gating, and controller right-stick camera (`axis 2`/`3`) are fully preserved.
