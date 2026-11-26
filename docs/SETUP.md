# Setup

BlueDroid needs three SDK directories for the real Android backend:

1. `platform-tools` (ADB).
2. `emulator`.
3. The Android 13 x86_64 system image at
   `system-images\android-33\google_apis_playstore\x86_64`.

Setup reads Google's official XML manifests over HTTPS and downloads the latest
Windows x64 ZIPs directly. It does not require a JDK, `sdkmanager.bat`, or
`avdmanager.bat`.

## 1. Normal first run

Build and launch:

```sh
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
make -j4
./bin/BlueDroidEmulator.exe
```

On first launch, BlueDroid:

- creates `%LOCALAPPDATA%\BlueDroid` and its subfolders,
- copies portable `emulator.ini` and `emulators.ini` into `config\`,
- checks for `C:\AndroidSDK`,
- otherwise checks `%LOCALAPPDATA%\BlueDroid\sdk`,
- if neither is usable, reads the official repository and system-image
  manifests in the background,
- downloads and extracts `platform-tools`, `emulator`, and
  `system-images;android-33;google_apis_playstore;x86_64` directly into the
  local SDK,
- sets `ANDROID_SDK_ROOT`, `ANDROID_HOME`, and `ANDROID_AVD_HOME` for itself and
  all child processes.

No console windows or terminal flashes appear during setup or phone launches.
Progress is shown in the manager banner and in `logs\emulator.log`.

You do not need to create an AVD manually. BlueDroid creates one per phone on
first open.

When setup completes on a fresh install, a welcome prompt offers a shortcut to
Settings > General, where you can pick a performance preset, FPS cap, GPU mode,
close behavior, and parked-audio muting before creating your first phone. See
the [User Guide](USER_GUIDE.md#6-settings).

## 2. Manual SDK setup

Use this if automatic installation fails or you want to control the SDK.

### Existing system SDK

Place a usable SDK at:

```text
C:\AndroidSDK
```

### Local SDK

Or let BlueDroid use:

```text
%LOCALAPPDATA%\BlueDroid\sdk
```

Extract the latest Windows x64 ZIPs named by these official manifests:

```text
https://dl.google.com/android/repository/repository2-1.xml
https://dl.google.com/android/repository/sys-img/google_apis_playstore/sys-img2-1.xml
```

Use the archive URLs for `platform-tools`, `emulator`, and
`system-images;android-33;google_apis_playstore;x86_64`. Extract them so the
final directories are:

```text
<sdk>\platform-tools\adb.exe
<sdk>\emulator\emulator.exe
<sdk>\emulator\emulator-check.exe
<sdk>\system-images\android-33\google_apis_playstore\x86_64\system.img
```

BlueDroid creates AVD descriptors and `config.ini` files natively under
`%LOCALAPPDATA%\BlueDroid\avd`; no AVD manager command is required.

### Verify

After SDK setup, these should exist:

```text
<sdk>\emulator\emulator.exe
<sdk>\platform-tools\adb.exe
<sdk>\system-images\android-33\google_apis_playstore\x86_64
```

Then start BlueDroid and add a phone. The first open can take several minutes
while Android boots.

## 3. Configuration

Edit the active config, not necessarily the portable one:

```text
%LOCALAPPDATA%\BlueDroid\config\emulator.ini
```

Key settings:

```ini
vmBackend=android
cpuCores=4
ramMB=4096
screenWidth=1080
screenHeight=1920
dpi=320
androidVersion=13
systemImage=android-33
```

`vmBackend=android` is the real backend. `vmBackend=qemu` is accepted as an
alias for it. `vmBackend=mock` runs the demo renderer only.

Per-phone hardware overrides are stored in:

```text
%LOCALAPPDATA%\BlueDroid\config\emulators.ini
```

Templates assigned when creating a phone (`Global`, `Balanced`, `Compact`,
`Tablet`, `Performance`) fill those overrides.

## 4. Building from source

Requirements:

- MSYS2 MinGW-w64 `g++`/`gcc`
- CMake
- make

```sh
cd C:\Users\Admin\Downloads\dnSpy-master\dnSpy-master
mkdir build
cd build
cmake .. -G "MinGW Makefiles"
make -j4
```

Executable:

```text
build\bin\BlueDroidEmulator.exe
```

The build links Direct3D 11, DXGI, D3D compiler, DWM, OLE32, COMDLG32, PSAPI,
and WinHTTP (used for manifest and ZIP downloads).

## 5. Fresh install / reset

To reset only manager state, remove:

```text
%LOCALAPPDATA%\BlueDroid\config\emulators.ini
```

To remove all phone data, SDK downloads, previews, and logs, remove the entire:

```text
%LOCALAPPDATA%\BlueDroid
```

This is destructive. Back up phones first from the card menu.

## 6. Next steps

- [User Guide](USER_GUIDE.md)
- [Storage and Isolation](STORAGE.md)
- [Troubleshooting](TROUBLESHOOTING.md)
