# Fate/Samurai Remnant — Mouse Camera Fix

A lightweight mod that fixes the sluggish, unresponsive mouse camera controls in the PC (Steam) version of **Fate/Samurai Remnant**.

---

## What Does This Mod Do?

In the vanilla game, mouse camera movement suffers from several engine issues that make looking around feel unresponsive and jittery:
- **Large Speed Deadzone:** Slow or subtle mouse movements are completely ignored by the camera (~54% deadzone).
- **Negative Acceleration:** Fast mouse swipes hit an artificial cap and slow down or stutter instead of turning smoothly.
- **Input Lag:** Mouse movement has an extra 1-frame polling delay.

**This fix replaces that behavior with smooth, true 1:1 raw mouse camera control.**

### What Is Preserved?
- In-game mouse sensitivity slider and camera inversion options (Invert X / Invert Y) still work normally.
- Gamepad / controller camera controls are completely unaffected.
- Menus, mouse cursor, and mouse buttons work just like in the original game.
- 100% safe: **No original game files are modified or overwritten.**

---

## Installation

Find your game directory:
> In Steam, **right-click** *Fate/Samurai Remnant* &rarr; **Manage** &rarr; **Browse local files**.

Choose **one** of the two options below depending on your setup:

### Option 1: Standard Installation (Default)
If you are **not** using any other mods:
1. Download and extract the release zip.
2. Copy these two files into your game folder (where `FateSamuraiRemnant.exe` is located):
   - `dinput8.dll`
   - `fsr_mouse_fix.ini`
3. Launch the game!

---

### Option 2: If Using Other Mods (Ultimate ASI Loader / Lyall's Fix)
If you already use mods like [*FateSamuraiRemnantFix*](https://github.com/Lyall/FateSamuraiRemnantFix "Check it out!") (ultrawide/resolution fix) that already include a `dinput8.dll`:
1. Keep the existing `dinput8.dll` in your game folder.
2. Copy these two files from the release zip into your game folder:
   - `FSRMouseFix.asi`
   - `fsr_mouse_fix.ini`
3. Launch the game!

---

## Configuration (Optional)

You can tweak the mod settings at any time by opening `fsr_mouse_fix.ini` in Notepad:

- **`SensitivityScale`** *(Default: `1.0`)*:  
  An extra multiplier for camera speed. If the in-game sensitivity slider doesn't go high or low enough for your mouse DPI, change this value (for example: `1.5` for faster, `0.7` for slower).
- **`EnableFix`** *(Default: `1`)*:  
  Set to `0` if you want to temporarily disable the camera fix without removing the mod files.

---

## How to Uninstall

Simply delete the mod files from your game folder:
- Delete `dinput8.dll` (or `FSRMouseFix.asi` if installed as an ASI)
- Delete `fsr_mouse_fix.ini`

Your game will immediately return to its original unmodded state.

---

## Troubleshooting

- **Check if the mod is running:**  
  Press `Win + R`, type `%TEMP%`, and press Enter. Open `fsr_mouse_fix.log` in Notepad. It will show whether the fix successfully hooked into the game.
- **Mouse cursor visible during gameplay:**  
  Make sure the game window is focused by clicking inside the game screen.
