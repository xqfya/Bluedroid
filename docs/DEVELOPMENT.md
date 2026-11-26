# Development

This document is for maintaining or extending BlueDroid. For daily use, start
with the [User Guide](USER_GUIDE.md).

## 1. Source map

```text
src/
  main.cpp                    Win32 entry, single instance, bootstrap
  ui/window.cpp               MainWindow, ImGui scenes, DX11 HD shaders, dialogs,
                              APK UI, input
  core/
    app_setup.cpp             runtime folders, env vars, manifest-driven SDK install
    config.cpp                global emulator.ini model
    phone_profile.cpp         emulators.ini load/save and identity repair
    vm_factory.cpp            backend selection
    mock_vm.cpp               demo renderer backend
    qemu_vm.cpp               legacy stub
  android/
    android_vm.cpp            real emulator/ADB backend
    adb.cpp                   legacy stub client
    gms.cpp                   legacy stub GMS layer
    image_manager.cpp         legacy stub image manager
    device_profile.c          sample build.prop generator
  util/
    logger.cpp                shared log file
    ppm_writer.c              still screenshot writer
include/emulator/             public headers
resources/ui.h                ImGui header/theme helper
resources/imgui/              vendored Dear ImGui + DX11/Win32 backends
data/build.prop               sample only
```

## 2. Build

```sh
cd C:\Users\Admin\Downloads\dnSpy-master\dnSpy-master\build
make -j4
```

Executable:

```text
build\bin\BlueDroidEmulator.exe
```

If linking fails with permission denied, stop the running process first:

```powershell
Get-Process BlueDroidEmulator -ErrorAction SilentlyContinue | Stop-Process -Force
```

## 3. Important invariants

Preserve these when changing code:

1. **Real Android backend.** Do not add a mock phone UI. Phones come from the
   official Android Emulator and Google APIs system image.
2. **No fake progress.** `bootPercent()` may return `-1`; the UI must show an
   indeterminate bar, never a timer animation. APK progress must come from real
   bytes/stages.
3. **No console flashes.** Child processes use `CREATE_NO_WINDOW`, hidden
   startup, and hidden `adb shell` pipes.
4. **Phone isolation.** AVD name, port, userdata image, and APK library are
   unique per phone. Do not reintroduce a shared `data.img`.
5. **Parking is not stopping.** Back arrows park; only menu Stop, Stop all,
   Exit all, and tray stop/exit terminate phones.
6. **ASCII UI labels.** The current font set has no symbol fallback. Icons are
   drawn with ImDrawList or written as plain ASCII text.
7. **No app-launch keyboard shortcuts.** Keyboard input is for the active
   Android phone only.
8. **Anti-AFK is harmless.** DPAD keys only. Keep-awake is WAKEUP only.
9. **Font atlas.** The DX11 backend builds/uploads the ImGui font atlas. Do not
   call `ImFontAtlas::Build()` manually.

## 4. UI architecture

`MainWindow` has three modes: `Manager`, `Emulator`, `Settings`.

Rendering is a Win32 message loop plus per-frame `renderFrame()`. The UI uses
ImGui with a Direct3D 11 backend. `resources/ui.h` provides the themed header
bar and animation helpers; `src/ui/window.cpp` contains a small `ui::`
compatibility layer for manager/settings widgets.

Popup styling is centralized in `PopupTheme` so card menus, APK picker, rename,
delete, and message popups match the active theme.

Card hover animation uses ImGui state storage and eases the rendered card
artwork without changing the grid slot, preserving hit-testing stability.

## 5. VM backend details

`createVirtualMachine()` maps:

| Config value | Backend |
| --- | --- |
| `android` | `AndroidVirtualMachine` |
| `qemu` | `AndroidVirtualMachine` alias |
| anything else | `MockVirtualMachine` |

### AndroidVirtualMachine responsibilities

- SDK discovery and native AVD descriptor/config creation.
- Rewriting AVD `config.ini` hardware and storage keys.
- Silent `emulator-check.exe accel` pre-flight and Manager warning.
- Launching/attaching/terminating emulator processes with the selected
  `-gpu host` or `-gpu swiftshader` mode.
- Boot status from ADB.
- Hidden window capture via `PrintWindow`, ADB fallback, at the configured
  30/60/120 FPS cap.
- Guest media-volume muting for phones parked in the Manager.
- Persistent hidden input shell.
- APK upload/install.

Frame data is guarded by `fbMtx_`. Status strings are guarded by `statusMtx_`.
APK job records are guarded by their own mutexes.

## 6. Adding a phone feature

Typical flow for a new per-phone operation:

1. Add an action case in `MainWindow::handleCardAction()` or a toolbar handler.
2. Find the profile by ID with `findProfile()`.
3. If the operation needs Android, get/create the VM through `vms_` and
   `makeVmForProfile()`.
4. Keep the AVD/port stable so data remains attached to the same phone.
5. Persist UI state with `persist()`.
6. Log start/success/failure to `Logger`.

For new storage, use `AppSetup::phoneDataDir(port)` so it is isolated per phone.
Never write shared state into `AppSetup::dataDir()` root unless it is truly
global.

## 7. APK install internals

`startApkInstall()` creates one `ApkInstallJob` with one `ApkInstallDevice` per
target phone. Threads wait for VM `Running` and ADB online, then call
`AndroidVirtualMachine::installApk()`.

The upload uses base64 through a hidden `adb.exe` process because raw binary
stdin on Windows stops at `0x1A`. Progress is counted from bytes represented by
successfully written encoded blocks.

Remote temp APKs are named per port and removed on success, cancel, and
failure. The host copy is stored in `data\phone_<port>\apks`.

## 8. Test hooks

`MainWindow` exposes debug helpers used for smoke tests. In production they are
harmless but should not be bound to normal keys.

A hidden Win32 message `WM_APP + 9` dispatches:

| wParam | Action |
| --- | --- |
| 1 | Toggle anti-AFK |
| 2 | Toggle keep-awake |
| 3 | Open settings |
| 4 | Close settings |
| 5 | Open first phone |
| 6 | Park current phone |
| 7 | Stop all phones |
| 8 | Exit all |
| 9 | Toggle list view |
| 10 | Cycle status filter |
| 11 | Open settings tab by `lParam` |
| 12 | Cycle font size |
| 13 | Open APK picker from `BD_TEST_APK` |
| 14 | Direct-install `BD_TEST_APK` to first phone |

Example smoke test:

```powershell
$env:BD_TEST_APK = "C:\Users\Admin\Downloads\aptoide.apk"
# post WM_APP+9 with wParam 14 to the BlueDroid window
```

Verify with:

```text
%LOCALAPPDATA%\BlueDroid\logs\emulator.log
%LOCALAPPDATA%\BlueDroid\data\phone_5554\apks
C:\AndroidSDK\platform-tools\adb.exe -s emulator-5554 shell pm list packages -3
```

Clean up test packages with `adb uninstall <package>`.

## 9. Documentation map

- README.md: overview and quick start.
- USER_GUIDE.md: daily controls and behavior.
- STORAGE.md: paths, isolation, backup/manual restore.
- ARCHITECTURE.md: runtime layers and threading.
- SETUP.md: first run and manual SDK setup.
- TROUBLESHOOTING.md: common failures and log checklist.
- DEVELOPMENT.md: this file.
