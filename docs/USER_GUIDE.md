# User Guide

This guide covers normal daily use of BlueDroid. For exact disk paths, see
[Storage and Isolation](STORAGE.md).

## 1. Manager screen

BlueDroid opens to the manager. It does not auto-start a phone at launch.

### Window frame

BlueDroid uses its own borderless window frame with rounded corners instead of
the native Windows title bar.

- Drag the top bar to move the window, just like a native title bar. Header
  buttons remain clickable.
- Drag the window edges to resize it. Maximized windows restore when you drag
  the top bar down.
- The top-right controls are Minimize, Maximize/Restore, and Close.
- By default, Close hides BlueDroid to the tray and keeps phones running. Turn
  off `Minimize to tray on close` in Settings > General to make X exit all and
  quit instead. `Exit all` always stops everything.

### Status banner

The header shows SDK setup progress. If emulator acceleration is unavailable,
it shows a warning to enable VT-x/AMD-V in BIOS or Windows Hypervisor Platform
and restart BlueDroid. Setup errors stay visible until the problem is fixed.

### Top toolbar

| Control | Action |
| --- | --- |
| `+ New emulator` | Opens inline name editor and template picker. |
| `Install APK` | Opens a Windows file picker for a `.apk`. You can also drag/drop an APK anywhere in the window. |
| `Stop all` | Stops every running/parked phone. Android data is kept. |
| `Exit all` | Stops everything and quits BlueDroid. |
| `Settings` | Opens the settings scene. |

### Search and filters

- Search matches name, AVD, port, and template.
- Filters: `All`, `Running`, `Stopped`, `Paused`, `Starred`.
- Views: grid or list.
- `Select` enables multi-select; then `All`, `None`, and action buttons operate
  on the selected phones.
- Starred phones sort first.
- The manager shows at most six grid columns.
- Cards show a still preview, name, status dot, star, three-dot menu, and
  optional live CPU/RAM badges.

### Card hover behavior

Hovering a card eases the whole card outward with a soft accent glow. Moving
the mouse away eases it back. Popups use the active theme colors and animate in
instead of showing ImGui's darker default popup surface.

## 2. Phone card menu

The three-dot menu contains:

| Menu item | Meaning |
| --- | --- |
| `Open` / `Return to phone` | Opens that Android display. |
| `Rename...` | Changes the display name only; data and AVD stay the same. |
| `Star phone` / `Remove star` | Toggles star sorting. |
| `Pause` / `Resume` | Changes that phone's UI state. |
| `Backup data` | Copies that phone's Android data and APK library into `backups\`. |
| `Stop phone` | Terminates that phone's emulator process. Data remains. |
| `Delete phone` | Removes the profile, AVD, userdata, previews, and APK library. Destructive. |

Only `Stop phone`, `Stop all`, `Exit all`, and the tray stop/exit commands stop
phones. The back arrow only parks.

## 3. Opening and parking a phone

Clicking a card opens the Android phone view:

- If the phone is stopped, BlueDroid launches its hidden emulator.
- Boot progress comes from real milestones (`adb shell getprop ...`), not a
  fake timer. A real percentage is shown when available; otherwise the bar is
  indeterminate.
- The Android display is captured from the hidden emulator window and drawn
  inside the BlueDroid phone area, preserving the phone aspect ratio with black
  bars.
- Live capture FPS is shown in the status bar when enabled.

Use the top-left Back arrow to park:

- The Android phone keeps running.
- BlueDroid returns to the manager.
- Parked phones continue anti-AFK/keep-awake handling and preview refresh.

If BlueDroid is restarted while a phone is still running, it reattaches to the
existing `emulator-<port>` and does not reboot Android.

## 4. Emulator toolbar

The bottom overlay contains:

| Button | Android action |
| --- | --- |
| `Back` | Keycode 4, Android Back. |
| `Home` | Keycode 3, Android Home. |
| `Recent` | Keycode 187, Android Recents. |
| `Full` | Toggles BlueDroid fullscreen. |
| `Shot` | Saves `screenshot.ppm`. |
| `Settings` | Opens settings; phone keeps running. |

The bottom `Back` button sends Android Back. It is different from the manager's
top-left Back arrow, which only parks.

### Keyboard and touch

- Mouse clicks/drags map to Android touch coordinates scaled to the logical
  screen size.
- Printable characters are delivered through `WM_CHAR` only, so text is not
  doubled or mangled.
- `F11` toggles fullscreen while the phone is active.
- `Esc` exits fullscreen when fullscreen is active.
- There are no app-launch keyboard shortcuts.

## 5. Installing APKs

1. Click `Install APK` or drag/drop a `.apk` onto BlueDroid.
2. The picker shows the detected filename and size.
3. Choose `All phones` or `Specific phones`. The specific list has its own
   search box.
4. Click `Install`.

A bottom-right progress panel appears for each running install:

- Overall progress is based on real APK bytes written to the ADB upload pipe.
- Per-phone rows show that phone's real stage, such as preparing, uploading,
  installing, success, or failure.
- `Cancel` stops pending transfer work. The temporary remote APK is removed.
- Finished jobs can be closed.

The APK is saved into that phone's own `apks\` library and installed into that
phone's Android userdata. See [Storage](STORAGE.md#4-apk-storage-and-installation).

## 6. Settings

Settings uses a left tab rail and a back button. On a fresh install, a welcome
prompt appears once setup finishes and can jump straight into this screen.

### General

- **Performance preset**: `Compact`, `Balanced`, or `Performance` one-click
  cards. They rewrite the global hardware defaults in `config\emulator.ini` and
  affect new phones and profiles using Global defaults.
- **FPS cap**: `30`, `60`, or `120` frames per second.
- **Graphics renderer**: `Host (DirectX)` or `Software` (SwiftShader) for older
  GPUs. FPS cap and renderer apply the next time a phone starts.
- **Minimize to tray on close**.
- **Mute emulator audio when parked in Manager**.
- `Show FPS counter`
- `Anti-AFK keys`
- `Keep phones on`
- `Resource badges`
- `Preview refresh` interval, 1-30 minutes

Anti-AFK sends only DPAD_UP/DOWN/LEFT/RIGHT/CENTER key events every few seconds
to running phones. It never opens apps and never types letters. The active
phone is skipped while you are actively inputting.

Keep-awake sends only Android WAKEUP (keycode 224) to running parked phones so
screens do not sleep. It is separate from anti-AFK.

When `Mute emulator audio when parked` is enabled, BlueDroid mutes each Android
guest while it is not the active phone and restores the previous media volume
when you open it again.

### Instances

- Refresh previews now.
- Stop all phones.
- Exit all.
- Clear search/filters.
- View/filter summary.
- Per-phone override editor: choose a phone, then switch it between
  `Portrait 1080x1920` and `Landscape 1920x1080`, or return it to global
  defaults. Stop and reopen the phone to apply changes.

### Appearance

- Preset themes were removed. The release ships one built-in palette, stored as
  `theme=Custom` in `config\emulator.ini`.
- Custom palette with individual color pickers for accent, window, title bar,
  card panel, nav bar, buttons, border, text, muted text, success, and danger.
  The palette is enabled by default; editing any color keeps it enabled.
  **Reset palette to defaults** restores the shipped colors.
- UI scale slider from 100% to 150% for 1440p/4K displays. The scale applies
  when you release the slider, with a `Reset` button beside it. If the UI ever
  becomes too large to reach Settings, press `Ctrl+0` anywhere to reset the
  scale to 100%.
- Font family (Calibri or Segoe UI) and font size.
- Render quality: Standard, HD, or Ultra HD.
  - HD runs a Direct3D 11 post-processing shader over the custom UI.
  - Ultra HD adds 4x MSAA before the shader pass.
- Shader tuning: sharpness, contrast, saturation, glow, and vignette.
- Window shape: rounded-corners toggle, corner radius, and UI roundness.
- Reset appearance to defaults.
- Appearance and gamer settings are saved in both `config\emulators.ini` and
  `config\emulator.ini`; theme, UI scale, FPS cap, GPU mode, close behavior,
  audio, and performance preset live in `emulator.ini`.

If a shader setting looks wrong, choose `Standard` to bypass the shader pass
entirely. Standard still uses the same custom ImGui interface.

## 7. Tray and window close

- Clicking the window X hides BlueDroid to the system tray by default. Phones
  keep running. Disable that setting in General to make X quit everything.
- Double-click the tray icon or choose `Open BlueDroid` to restore the window.
- Tray menu:
  - `Open BlueDroid`
  - `Stop all phones`
  - `Exit all and quit`

Use `Exit all` when you actually want everything stopped.

## 8. Multi-phone behavior

- Each phone has its own AVD name, ADB port, Android data, and APK library.
- Two phones with the same display name still get different AVDs.
- Renaming a phone does not change its storage.
- Deleting a phone deletes its data; back up first if needed.
- Stopping a phone does not delete its data.
- Parked phones remain live and can receive anti-AFK/keep-awake events.

For the technical reason why phones cannot share data, see
[Storage and Isolation](STORAGE.md#3-android-userdata-isolation).

## 9. Updates

BlueDroid v1.0.0 includes a GitHub auto-updater.

- Once per launch, after the window is ready, BlueDroid fetches
  `version.json` from the repository and compares its `version` with the
  version compiled into the app.
- When the remote version is newer, it asks GitHub for the latest release,
  selects the first asset ending in `.zip` (then `.exe` / `.7z`, falling back
  to `assets[0]`), reads the release notes, and downloads the asset.
- Downloads are saved under `%LOCALAPPDATA%\BlueDroid\runtime\updates`. The
  updater does not replace the running EXE; it only stages the new file and
  tells you where it is.
- You can also open **Settings > General > Check for updates** to run the same
  check manually. Manual checks show a result even when you are up to date.
- Network failures are non-fatal. A startup failure is only logged; a manual
  check shows the error in a dialog.
