# Architecture

BlueDroid is a Win32 application with a Dear ImGui front end and a plug-in-like
virtual machine interface. The real Android backend launches the official
Android Emulator and communicates with it through ADB.

## 1. High-level layers

```text
+---------------------------------------------------------------+
| Win32 window WndProc / message loop                            |
+---------------------------------------------------------------+
| MainWindow ImGui scenes                                        |
|   Manager | Emulator | Settings | dialogs | APK progress       |
+---------------------------------------------------------------+
| Theme/header layer (resources/ui.h)                            |
| ImGui + Direct3D 11 renderer                                   |
+---------------------------------------------------------------+
| InputManager | preview capture | CPU/RAM sampling              |
+---------------------------------------------------------------+
| IVirtualMachine                                                |
|   AndroidVirtualMachine | MockVirtualMachine                   |
+---------------------------------------------------------------+
| AppSetup | ConfigManager | PhoneProfile state | Logger         |
+---------------------------------------------------------------+
| Android SDK tools: emulator.exe, emulator-check.exe, adb.exe    |
+---------------------------------------------------------------+
```

## 2. Startup sequence

1. `wWinMain` installs the single-instance guard. A second launch posts
   `WM_APP + 2` to the existing window and exits.
2. `AppSetup::ensure()` creates the runtime directory tree, migrates portable
   ini files, sets SDK/AVD environment variables, and starts SDK installation
   if no usable SDK exists.
3. `ConfigManager` loads `config\emulator.ini`.
4. `MainWindow::create()` loads `config\emulators.ini`, previews, and the ImGui
   renderer.
5. `MainWindow::reattachRunningPhones()` tries to attach to any previously
   running `emulator-<port>` devices.
6. The window opens on the manager. No phone is auto-started.

## 3. UI modes

`MainWindow::UiMode`:

| Mode | Purpose |
| --- | --- |
| `Manager` | Phone cards, filters, multi-select, APK controls, settings entry. |
| `Emulator` | Live Android frame, toolbar, fullscreen, screenshot, FPS. |
| `Settings` | General, Instances, and Appearance tabs. |

The back arrow in Settings/Emulator parks the phone view and returns to the
manager. It does not stop the emulator.

## 4. VM abstraction

`IVirtualMachine` exposes create/start/pause/resume/stop/reset, state, error and
status strings, `bootPercent()`, framebuffer access, and input injection.

### AndroidVirtualMachine

`createVirtualMachine("android")` returns `AndroidVirtualMachine`. The string
`qemu` is currently an alias for the same real Android backend.

`AndroidVirtualMachine::create()`:

- finds the SDK,
- creates or updates the AVD,
- applies per-phone hardware and storage paths,
- allocates the framebuffer at the configured resolution.

`start()` either attaches to an existing device by serial or launches:

```text
emulator.exe -avd <AVD> -port <port> -gpu host -no-snapshot
```

The emulator has a real native window, but BlueDroid hides it:

- `WS_EX_TOOLWINDOW`, `WS_EX_APPWINDOW` removed
- `SW_SHOWNOACTIVATE`, then moved offscreen
- sibling Qt tool windows hidden

Child processes are created with `CREATE_NO_WINDOW` and hidden startup flags,
so no console or terminal flashes.

### MockVirtualMachine

Renders a simple animated framebuffer for UI development. It does not run
Android and is selected with `vmBackend=mock`.

## 5. Capture and display

The capture thread prefers `PrintWindow` on the hidden emulator window. This is
much faster than taking an ADB PNG screenshot every frame. If the window cannot
be captured, `captureAdbFrame()` is the slower fallback.

Frames are written into an RGBA framebuffer guarded by a mutex. The UI uploads
the frame to a Direct3D 11 texture and draws it letterboxed inside the
BlueDroid phone area, preserving Android aspect ratio with black bars.

The FPS counter is computed from real `frameCounter()` changes.

Still card previews are separate small PPM files. They are not Android
snapshots and are refreshed only periodically or on park/stop.

## 6. Boot progress

Boot progress is real:

- ADB connection state
- `getprop sys.boot_completed`
- `getprop init.svc.bootanim`
- Android service startup state

`bootPercent()` returns 0-100 when the backend knows the milestone, or `-1`
when it does not. The UI shows an indeterminate bar for `-1`; it never
animates a fake percentage.

## 7. Input path

Input is forwarded through a persistent hidden `adb shell` process:

- touch down/up and swipe -> `input touchscreen ...`
- hardware keys -> `input keyevent ...`
- printable text -> `input text ...`

The shell is kept open to avoid process launch latency on taps/swipes. It is
restarted automatically if it goes stale.

Printable characters arrive through `WM_CHAR` and are sent once. They are not
also sent as key events, which prevents doubled or mangled text.

Anti-AFK and keep-awake are implemented in `MainWindow::update()`:

- Anti-AFK: harmless DPAD keycodes 19-23 every few seconds to running phones.
- Keep-awake: WAKEUP keycode 224 every ~15 seconds.

Neither opens apps nor types letters.

## 8. APK install pipeline

Parallel install jobs are owned by `MainWindow`. Each target phone gets an
`ApkInstallDevice` record with real byte/stage progress.

`AndroidVirtualMachine::installApk()`:

1. Copies the APK into `data\phone_<port>\apks`.
2. Appends to `installed.txt`.
3. Opens a hidden `adb.exe ... shell toybox base64 -d > remote` process.
4. Streams base64 blocks from the saved APK copy.
5. Counts progress from real APK bytes successfully written.
6. Runs `pm install -r -t <remote>`.
7. Deletes the remote temp APK on every outcome.

Base64 is used because Windows `adb.exe` treats stdin as text and stops at byte
`0x1A` in raw APK binaries.

## 9. Data isolation

Each logical phone has a unique `id`, `avd`, and `port`:

- `avd` selects `avd\<AVD>.avd\userdata-qemu.img`.
- `port` selects `emulator-<port>` and `data\phone_<port>`.

`config.ini` is rewritten so `disk.dataPartition.path` points at that AVD's own
permanent userdata image instead of `<temp>`. Duplicate AVD/port values loaded
from a hand-edited profile file are repaired.

See [Storage and Isolation](STORAGE.md) for the full path table and data
life cycle.

## 10. Threading model

| Thread | Work |
| --- | --- |
| UI thread | Win32 messages, ImGui draw, user actions, state persistence. |
| Capture thread per VM | `PrintWindow` or ADB frame capture, framebuffer updates. |
| Boot/status work | ADB polling launched by the VM backend. |
| APK worker per target | Upload and install for one phone. |
| Setup thread | Background manifest fetch, SDK ZIP download, and extraction. |

Shared state is protected with mutexes: framebuffer, status strings, input
pipe, APK job records, and setup status.

## 11. Logging

All subsystems write through `Logger` to:

```text
%LOCALAPPDATA%\BlueDroid\logs\emulator.log
```

Use it to trace boot, install, capture, and stop behavior.
