# BBCF Improvement Mod

[English](README.md) | [中文](README.zh-CN.md)

`BBCF Improvement Mod` is a feature extension mod for *BlazBlue: Centralfiction*. It injects through `dinput8.dll` and provides training tools, frame analysis, replay takeover, custom palettes, additional game options, and experimental features.

This project is not affiliated with Arc System Works or any of its partners.

## Features

- Training mode enhancements, including hitbox display, frame data, frame history, combo information, dummy controls, and recording slots.
- A TAS-style frame editor based on the stable v7.2 codebase.
- Independent frame-by-frame P1 and P2 input editing through the game's final BattleInput processing loop.
- Base-state saving, rewind, re-recording, preview playback, and presentation playback.
- Replay takeover tools and local replay loading.
- Custom `.cfpl` palettes and compatible `.hpl` effect files.
- Configurable overlay localization and additional graphics and game options.

## Installation

1. Download a release package.
2. Copy `dinput8.dll` and `settings.ini` into the game directory containing `BBCF.exe`.
3. Copy `palettes.ini` as well if you use custom palettes.
4. Start the game. The mod creates its log, overlay, and palette directories as needed.

To uninstall the mod, remove or rename `dinput8.dll`. The mod does not permanently modify the game executable or other game files.

> Use the mod with a legally purchased and up-to-date copy of the game. Online compatibility and gameplay impact are the user's responsibility.

## Overlay Controls

- Press `F1` by default to show or hide the main overlay. The hotkey can be changed in `settings.ini`.
- Drag empty areas of a window to move it. Double-click a title bar to collapse the window.
- The palette editor can be resized from its lower-right corner; double-clicking the corner fits the window to its contents.
- Hold `Ctrl` while clicking a slider or numeric control to enter a value directly.
- Delete `menus.ini` from the game directory to reset overlay window positions.

## TAS Frame Editor

The TAS editor is intended for training mode only. Inputs are injected into both final BattleInput slots, so P1 and P2 can be edited independently without relying on the currently selected main controller.

### Input Format

Both input fields use numpad notation:

- Directions use `7 8 9 / 4 5 6 / 1 2 3`; `5` is neutral.
- Each direction digit consumes one frame. `66` means forward for two frames, while `656` means forward, neutral, forward.
- Button letters are attached to the preceding direction frame. For example, `5C` means neutral plus C, `623C` means 6, 2, then 3+C, and `28D` means 2, then 8+D.
- If the P1 and P2 commands have different lengths, the shorter side is padded with neutral `5` frames.
- Movie length is not artificially capped; it is limited only by available memory and the platform's addressable container size.
- Spaces, commas, and hyphens are accepted as separators in command text.
- Buttons are represented by `A`, `B`, `C`, and `D`. Multiple buttons can be attached to the same direction frame.

### Keyboard Shortcuts

The TAS shortcuts work while the TAS window is active and can be changed in `settings.ini`:

| Key | Default setting | Action |
| --- | --- | --- |
| `I` | `TasParseKeybind=I` | Parse the current P1/P2 command text. This is the same as clicking `Parse input`. |
| `L` | `TasAdvanceKeybind=L` | Advance by the configured `Frame count`. In edit mode, append or re-record those frames from the current movie position. |
| `J` | `TasRewindKeybind=J` | Rewind by the configured `Frame count`, reload the base state, replay to the target, and truncate later Movie frames. |

The frame count is entered in the TAS window and must be a positive number. Movie length is not artificially capped. The shortcuts are especially useful when testing a combo without repeatedly moving the mouse between the input fields and the frame controls. During hidden Presentation playback, `I` stops the playback instead of parsing input; the normal parse action resumes after the TAS UI is visible again.

### Creating a Combo

1. Enter training mode, open the TAS window, and click `Enter TAS mode`.
2. At the start of the combo, click `Save base state`.
3. Enter the next P1/P2 commands and click `Parse input`.
4. Set `Frame count`, then click `Advance N frames`.
5. The game advances by the selected number of frames and appends the parsed inputs to the Movie.
6. Repeat the process until the combo is complete.

`Input progress` shows how many parsed command frames have been consumed. `Movie frames` shows the total number of frames currently in the Movie.

### How Editing Works

The editor uses three related pieces of data:

- **Command input** is the parsed result of the P1/P2 text fields. It is a temporary sequence waiting to be added to the Movie.
- **Movie input** is the recorded frame sequence that has already been appended. It is the sequence used by Preview and Presentation playback.
- **Base state** is the native training-mode snapshot used as the deterministic starting point for playback, rewind, and re-recording.

`Save base state` should be used at the exact moment where the combo is meant to begin. Saving it again replaces the previous starting point and resets the edit cursor to the beginning. Loading the base state also resets the playback position to frame zero and increments the rerecord count.

When `Advance N frames` or the `L` shortcut is used for the first time, the next frames are appended to the Movie. When editing an existing Movie after Preview or Rewind, the same action removes all frames after the current edit position and writes the new frames there. This makes it possible to replace the final part of a combo without rebuilding the earlier portion.

If the parsed command is shorter than the requested frame count, the remaining frames are neutral. If the requested count is longer than the available command sequence, the editor continues with neutral input. P1 and P2 are always scheduled independently, and a missing side is padded with neutral input.

### Complete Editing Example

1. Enter a training match and activate TAS mode.
2. Save the combo starting position with `Save base state`.
3. Enter `623C` for P1 and `5` for P2, then press `I` or click `Parse input`.
4. Set `Frame count` to the number of frames to consume and press `L` or click `Advance N frames`.
5. Continue parsing and advancing until the first version of the combo is complete.
6. Use `Preview playback` to watch the Movie from the saved base state. Preview freezes at the final Movie frame.
7. Enter the next command, parse it with `I`, and press `L` to append frames from the frozen endpoint.
8. To replace an earlier section, press `J` to rewind by the selected count, then parse the replacement input and press `L` to re-record from that point.
9. Use `Reset parsed input` when the parsed command sequence should be neutralized while keeping the source text available for later parsing. Use `Reset movie` only when the entire recorded Movie should be cleared.

### Preview and Presentation

`Preview playback` is for continuing to build an unfinished combo:

1. The saved base state is loaded.
2. The current Movie is played continuously.
3. Playback freezes at the final Movie frame.
4. The TAS window becomes usable again so more commands can be parsed and appended with `Advance N frames`.

`Presentation playback` is for watching the completed combo:

1. The saved base state is loaded.
2. 60 neutral `5` frames are played as a lead-in.
3. The Movie is played continuously.
4. 240 neutral `5` frames are played as a lead-out.
5. Normal game processing resumes when playback ends.

The TAS UI is hidden during presentation playback. Use the configured TAS parsing hotkey to stop playback.

### Rewind, Reset, and Movie Files

- `Rewind N frames` reloads the base state, replays to the selected earlier frame, removes the later Movie frames, and freezes at the rewind point for re-recording.
- `Reset movie` stops playback, reloads the base state, clears the Movie, and returns the edit position to frame zero.
- `Reset parsed input` keeps the Movie, base state, and source text, but changes all currently parsed P1/P2 frames to neutral `5` and resets the parsing cursor. Click `Parse input` again to regenerate them from the text fields.
- `Resume game` removes the frame freeze and lets the game run normally.
- `Import` and `Export` read and write `tas_movie.txt` in the game directory. After importing a Movie, save a matching base state before editing or previewing it.

## Frame History

Frame History displays separate state and invulnerability rows for both characters. Non-idle frames are color-coded:

- Green: startup
- Red: active
- Blue: recovery
- Yellow: blockstun
- Purple: hitstun
- Cyan: special states such as dash

Additional markers identify head, body, feet, projectile, throw, and other invulnerability categories.

## Custom Palettes

After the first launch, custom palettes are stored under `BBCF_IM/Palettes/`. Place `.cfpl` or `.hpl` files in the corresponding character directory to load and select them in-game.

Legacy `.hpl` effects use names such as `Nyx_Izanami_effect01.hpl`. Files ending in `_effectbloom.hpl` enable the Bloom effect. Use `palettes.ini` to assign custom palettes to in-game palette slots.

## Crash Reports and Troubleshooting

When a crash occurs, the mod creates `BBCF_IM/CrashReports/Crash_<timestamp>/`, containing:

- `crash.dmp`: crash dump and embedded log data
- `logs.txt`: the recent log ring buffer
- `crash_context.txt`: exception details and mod metadata

When reporting a problem, provide the complete `Crash_<timestamp>` directory as an archive.

Useful troubleshooting steps:

1. Remove or rename `dinput8.dll` temporarily to check whether the issue is caused by the mod.
2. Confirm that `settings.ini` exists in the game directory.
3. Disable software that injects into or overlays D3D, such as recording and performance-monitoring tools.
4. If the display is black or incorrect, check that the rendering dimensions in `settings.ini` match the game settings.
5. Delete `menus.ini` to reset missing or misplaced overlay windows.

## Development Build

The current project uses the Visual Studio `v143` toolset and Windows SDK. Open [BBCF_IM.sln](BBCF_IM.sln) and select:

```text
Configuration: ReleaseDeploy
Platform:      Win32
Toolset:       v143
```

The build output is `bin/Release/dinput8.dll`. `ReleaseDeploy` uses the static runtime and is suitable for distribution.

## Credits

Thanks to the following contributors and to the BBCF PC community for development, testing, feedback, and support:

- GrimFlash
- KoviDomi
- Neptune
- Rouzel
- Dormin
- NeoStrayCat
- KDing
- PC_volt
- MorphRed
- Tadatys (sublimacija)

Additional thanks to Atom0s for the DirectX 9 Hooking article and to Durante for the dsfix source code.

## Disclaimer

```text
BBCF Improvement Mod is not affiliated with Arc System Works or its partners.
Do not use this project for malicious purposes, to gain an unfair online advantage,
or to unlock unreleased or unpurchased content. Use it only with a legally owned
copy of the official game.

Use this mod at your own risk. The maintainers are not responsible for any damage
or other consequences caused by using this software.
```