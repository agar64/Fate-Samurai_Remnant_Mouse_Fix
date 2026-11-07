
# Fate/Samurai Remnant — Raw Mouse Input Mod

## Objective

Investigate and, if technically feasible, create a Windows mod for the PC version
of Fate/Samurai Remnant that fixes the apparent negative mouse acceleration
affecting camera movement.

The desired result is consistent camera movement based on relative mouse
displacement, without unwanted acceleration, smoothing, delayed response, or
dead-zone-like behavior.

Increasing the game's sensitivity is not, by itself, a solution.

## Mandatory workflow

1. Investigate before implementing. Begin with a read-only assessment and
   produce an evidence-based plan.
2. Never assume the game engine, compiler, executable architecture, input
   library, or available mod loader. Identify them from the actual files.
3. Separate confirmed facts, hypotheses, and untested assumptions in notes.
4. Prefer the smallest, least invasive fix that addresses the demonstrated
   cause. Consider existing game settings and configuration before binary
   hooks or executable patches.
5. Do not implement a hook until the relevant input path and interception
   point have been identified with evidence.
6. Explain the proposed implementation and its risks before making
   consequential changes.
7. Make incremental changes, compile reproducibly, inspect the diff, and
   provide a test plan for each change.
8. Keep an investigation log so a new conversation can resume without
   repeating the entire analysis.

## Safety and file handling

- Never overwrite, patch, rename, or delete the installed game executable,
  original DLLs, or game data.
- Never deploy builds automatically into the game installation directory.
- Treat local game binaries as read-only evidence.
- Do not commit proprietary game binaries, extracted proprietary assets,
  or large generated files to Git.
- Keep experimental code and generated output inside this workspace.
- Do not run downloaded executables, scripts, or unknown DLLs without
  explaining what they do and obtaining approval.
- Do not perform unrelated cleanup or change global Windows settings.
- Preserve a reversible path to uninstall the mod and restore the original
  game state.

## Technical investigation

Determine, where possible:

- Game version and executable architecture (x86 or x64).
- Executable metadata, imported DLLs, relevant loaded modules, and
  available modding or plugin mechanisms.
- Whether the game uses Win32 cursor APIs, legacy mouse messages, Raw Input,
  DirectInput, or an engine-specific input layer.
- Whether camera input uses relative movement, cursor-position differences,
  cursor recentering, frame-time scaling, interpolation, smoothing,
  acceleration curves, or a combination.
- Whether the sensitivity setting changes a simple scale factor or also
  affects filtering, thresholds, or input sampling.
- Whether configuration options or existing hooks already solve the issue.

Use static and dynamic analysis as appropriate. Never invent function names,
memory addresses, offsets, signatures, or API call results. Record how each
important conclusion was established.

## Raw Input requirements

Raw Input is a possible implementation route, not a predetermined solution.

- Understand the game's existing input and camera-update path before
  introducing another input source.
- Account for RegisterRawInputDevices registration and potential conflicts
  with existing device registrations.
- If Raw Input is appropriate, use relative mouse deltas rather than
  differences between successive screen-space cursor positions.
- Avoid adding duplicate mouse movement, cursor warping, or an additional
  sensitivity multiplier.
- Preserve appropriate mouse capture and focus behavior.
- Do not silently discard legacy input until the replacement has been
  verified to work.

Prefer an existing engine-level input function or a narrow, well-understood
hook when it is safer than replacing the whole input subsystem.

## Compatibility requirements

Preserve:

- Horizontal and vertical camera movement and their inversion settings.
- The intended sensitivity control, including a predictable range.
- Mouse buttons, keyboard input, and menu interaction.
- Window focus changes, Alt+Tab, mouse capture, and cursor release.
- Controller operation when the mod is active.
- Normal behavior when the mod is disabled or unloaded.

Avoid frame-rate-dependent movement and avoid changing unrelated gameplay.

## Verification

Establish a baseline before changing anything.

Test and document:

- Slow and fast movements over the same physical mouse distance.
- Repeated movements in both axes and at several sensitivity settings.
- The same tests before and after the modification.
- Menu navigation, mouse buttons, focus loss and regain, and controller use.
- Behavior with the mod absent and with the mod installed.
- Crashes, input duplication, cursor locking problems, and regressions.

Where practical, use repeatable measurements or logs rather than relying
only on subjective impressions.

Do not claim success until the patch has been built and its behavior has
been tested in the actual game.

## Deliverables

1. Investigation notes and an explanation of the root cause.
2. A documented implementation with source code.
3. Reproducible build instructions and required dependencies.
4. A mod package that is easy to install and remove.
5. A compatibility statement, known limitations, and test results.
