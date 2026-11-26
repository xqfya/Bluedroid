# Troubleshooting

Always start with the runtime log:

```text
%LOCALAPPDATA%\BlueDroid\logs\emulator.log
```

It records setup stages, SDK paths, AVD creation, emulator launch, boot
milestones, ADB states, capture FPS, APK upload/install, and stop/exit actions.

## Quick checklist

- Phone data should be under `avd\<AVD>.avd\userdata-qemu.img.qcow2`.
- Each phone should have a unique AVD name and even port.
- Each phone's APK library should be under `data\phone_<port>\apks`.
- The active AVD `config.ini` should contain
  `disk.dataPartition.path=<root>\avd\<AVD>.avd\userdata-qemu.img`, not
  `<temp>`.
- `emulator.exe`, `adb.exe`, and the Android 13 Google APIs image must exist in
  the chosen SDK.

## Build problems

### `cannot open output file bin\BlueDroidEmulator.exe: Permission denied`

An old BlueDroid process still holds the executable. Stop it and rebuild:

```powershell
Get-Process BlueDroidEmulator,emulator,qemu-system-x86_64,adb -ErrorAction SilentlyContinue | Stop-Process -Force
cd build
make -j4
```

### Missing headers or libs

Use MSYS2 MinGW-w64 with CMake and make. The project links `d3d11`, `dxgi`,
`d3dcompiler`, `dwmapi`, `ole32`, `comdlg32`, `psapi`, and `winhttp`.

## Setup problems

### SDK setup never finishes

Check the log for manifest, download, or extract errors. The installer reads
Google's HTTPS manifests and downloads three ZIPs directly. The system image is
large, so slow networks can take many minutes.

Workaround: install the SDK manually, then remove the partial local SDK or put
a complete SDK at `C:\AndroidSDK`. See [Setup](SETUP.md#2-manual-sdk-setup).

### BlueDroid says SDK is not ready

Verify:

```text
<sdk>\emulator\emulator.exe
<sdk>\platform-tools\adb.exe
<sdk>\system-images\android-33\google_apis_playstore\x86_64
```

Then restart BlueDroid so `AppSetup::ensure()` rechecks.

## Phone boot problems

### AVD creation failed

BlueDroid writes `<AVD>.ini` and `<AVD>.avd\config.ini` directly. Look for
`Native AVD creation failed` or a filesystem/permission error in the log.
Confirm `%LOCALAPPDATA%\BlueDroid\avd` is writable.

### Manager says virtualization is unavailable

Before each emulator launch BlueDroid silently runs
`<sdk>\emulator\emulator-check.exe accel`. If VT-x/AMD-V or Windows Hypervisor
Platform is not usable, the Manager banner tells you to enable virtualization
in BIOS or Windows Hypervisor Platform, then restart BlueDroid. Check the log
for `Emulator acceleration check failed` to see the checker output.

### Boot stuck at 12/48/72 percent

Those are real ADB milestones. Check the log for repeated `device offline` or
`device not found`.

- Wait longer on first boot; Android can take several minutes.
- Stop the phone, then reopen it.
- If the AVD is corrupted, back up data, then delete the phone and create it
  again.

### Emulator starts but no Android screen

BlueDroid captures the hidden Qt emulator window with `PrintWindow`. Check the
log for `Capturing emulator window`. If capture fails, it falls back to ADB
screencap, which is much slower.

Do not move or close the hidden emulator process manually. Use `Stop phone` or
`Stop all`.

## Input problems

### Taps or swipes feel delayed

Input goes through a persistent hidden `adb shell`. Restart the phone; the shell
pipe is recreated. The log should not show repeated shell restarts during normal
use.

### Typed characters are duplicated or wrong

Printable text is sent only through `WM_CHAR`, not as key events. If text is
wrong, check that no other keyboard hook or emulator window is receiving input.
Avoid external key simulators while BlueDroid is active.

### Anti-AFK types letters

It should not. Anti-AFK uses only DPAD keycodes 19-23. Disable it in
`Settings > General > Anti-AFK keys` if you need completely idle input.

## APK install problems

### APK picker says only `.apk` files

The file must end in `.apk`. BlueDroid does not unpack ZIPs.

### Progress bar appears frozen

Progress is real bytes written to ADB. It can pause while Android switches from
upload to `pm install`. The stage text should change from `Uploading APK` to
`Installing APK`. Check the log for `APK upload complete` and `pm install`
output.

### Install fails

Common causes:

- Phone was not fully booted/online.
- APK is corrupt or unsupported by Android 13/x86_64.
- App signature conflicts with an existing package; use a signed update of the
  same package.
- ADB is offline.

Check the log for `APK install ...` and `APK install failed`.

### APK file disappears after install

It should not. The APK is copied to:

```text
%LOCALAPPDATA%\BlueDroid\data\phone_<port>\apks
```

If missing, the destination folder was deleted manually or the phone was
deleted. Reinstall the APK.

## Data and persistence problems

### Apps reset after restarting a phone

Check the AVD `config.ini`:

```ini
disk.dataPartition.path=<root>\avd\<AVD>.avd\userdata-qemu.img
```

If it is `<temp>`, open the phone once; BlueDroid rewrites it before launch.

Also confirm `userdata-qemu.img.qcow2` exists and is changing size/time when the
phone runs.

### Two phones seem to share data

Each phone must have a unique `avd` and `port` in `config\emulators.ini`. Check:

```text
[phone]
id=...
name=...
avd=BD_cram
port=5554
```

No two phones should have the same `avd` or `port`. BlueDroid repairs duplicates
on load, but a phone that was already running with bad identity should be
stopped and reopened.

### Stopping a phone erased data

Stop should not erase data. Delete phone does. Check whether `Delete phone`,
`Delete all`, or manual folder removal happened. Restorable data exists only if
you created a backup under `backups\`.

### Need to restore a backup

There is no restore button yet. See
[Storage and Isolation](STORAGE.md#restoring-a-backup).

## Tray/window problems

### X closes the window but phones keep running

That is intentional. Use the tray menu `Exit all and quit` or the manager's
`Exit all` button to stop everything.

### Window does not come back from tray

Double-click the BlueDroid tray icon, or right-click it and choose
`Open BlueDroid`. If the icon is missing, restart BlueDroid; it will reattach to
running phones by port.

## UI problems

### Popup looks dark or different from the theme

Popups use the active manager theme colors and animate in. If the theme looks
wrong, check `Settings > Appearance` and restart the manager. Themes are saved
in `config\emulators.ini`.

### Card hover animation jitters

The card layout slot is fixed; only the card artwork lifts/expands. Jitter is
usually low frame rate or rapid mouse movement. It is cosmetic.

### HD shaders look too sharp or dark

Open `Settings > Appearance` and lower `Sharpness` / `Vignette`, or choose
`Standard` render quality. The log records shader compile failures; in that case
BlueDroid falls back to direct rendering automatically.

### Top bar does not move the window

Drag between the left title text and the header buttons. Header buttons and the
top-right Minimize / Maximize / Close controls are intentionally excluded from
the drag region. A maximized window restores when dragged down from its top bar.

## Still stuck

Collect:

- BlueDroid version/build time
- SDK path and system image path
- phone name, AVD, and port
- last 200 lines of `logs\emulator.log`
- exact steps to reproduce

Then file an issue or ask with that information.
