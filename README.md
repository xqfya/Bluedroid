# BlueDroid Emulator

BlueDroid is a desktop manager for multiple real Android phones. It is written
in C/C++ with a Dear ImGui interface: Direct3D 11 on Windows, SDL2 + OpenGL 3
on Linux and macOS.

The default backend launches the official Android Emulator with an Android 13
Google APIs system image. Phones run in hidden emulator windows; BlueDroid
draws the live Android capture inside its own phone frame.

The interface is a custom Dear ImGui shell rendered through Direct3D 11 on
Windows or SDL2 + OpenGL 3 on Linux/macOS. Windows builds also offer optional
DX11 HD post-processing shaders and per-user appearance settings.

## Highlights

- Multiple Android phones, each with its own AVD, ADB port, Android data, and
  APK library.
- Custom borderless UI with rounded corners, top-bar dragging, a single custom
  color palette (no preset themes), UI scale 100-150% with `Ctrl+0` emergency
  reset, fonts, and DX11 HD shaders on Windows (Standard / HD / Ultra HD).
- First-run setup shortcut into performance presets, FPS cap, GPU mode, close
  behavior, parked audio muting, and per-phone orientation overrides.
- GitHub auto-updater: checks `version.json`, downloads newer release assets
  silently at startup or on demand from General settings.
- Hidden emulator processes: no console flashes, no taskbar/alt-tab windows.
- Persistent phone data across stop/start and manager restarts.
- APK drag/drop or file picker, real byte-based upload, and `pm install`.
- Per-phone still previews, star/filter/search, grid/list views, multi-select,
  templates, and theme settings.
- Parking with Back keeps the phone running; Stop/Exit are explicit actions.
- Tray behavior: window X hides BlueDroid by default; General settings can make
  it exit all instead.
- Anti-AFK sends only harmless DPAD keys; keep-awake sends only WAKEUP.

## Quick start

Dear ImGui is a Git submodule under `resources/imgui`. Clone with submodules:

```sh
git clone --recurse-submodules https://github.com/xqfya/Bluedroid.git
cd Bluedroid
```

If you already cloned without them:

```sh
git submodule update --init --recursive
```

### Build prerequisites

- All platforms: CMake 3.16+, a C++17 compiler, and Git with submodule support.
- Linux: SDL2 dev, OpenGL dev, and libcurl dev packages. On Debian/Ubuntu:
  `sudo apt install build-essential cmake libsdl2-dev libgl1-mesa-dev libcurl4-openssl-dev`.
  On Fedora: `sudo dnf install gcc-c++ cmake SDL2-devel mesa-libGL-devel libcurl-devel`.
- macOS: Xcode Command Line Tools, then `brew install sdl2 curl cmake`.
- Windows: MSYS2 MinGW-w64 `g++`/`gcc` and `make`.

### Build

Windows (MinGW Makefiles):

```sh
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
make -j4
./bin/BlueDroidEmulator.exe
```

Linux/macOS (default generator):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/bin/BlueDroidEmulator
```

The output name is `BlueDroidEmulator` everywhere; only the Windows binary has
an `.exe` suffix.

### Platform notes

- Windows renderer: Dear ImGui over Direct3D 11, including DX11 HD shaders.
- Linux/macOS renderer: Dear ImGui over SDL2 + OpenGL 3.
- Capture: Windows uses hidden-window capture with an ADB fallback; Linux/macOS
  use ADB screencap (`adb exec-out screencap`).
- Emulator processes stay hidden on every platform, and each phone keeps its own
  AVD, ADB port, Android data, and APK library.
- Live FPS counter and periodic still card previews work on all platforms;
  `Ctrl+0` still resets the UI scale to 100% if it becomes unusable.

### Downloads and updates

Each GitHub release publishes a native package for the exact device architecture:

| Device | Release asset |
| --- | --- |
| Windows x64 | `BlueDroid-vX.Y.Z-win-x64.zip` |
| Windows ARM64 | `BlueDroid-vX.Y.Z-win-arm64.zip` |
| Linux x64 | `BlueDroid-vX.Y.Z-linux-x64.AppImage` |
| Linux ARM64 | `BlueDroid-vX.Y.Z-linux-arm64.AppImage` |
| macOS Apple Silicon | `BlueDroid-vX.Y.Z-macos-arm64.dmg` |
| macOS Intel | `BlueDroid-vX.Y.Z-macos-x64.dmg` |

`version.json` is published alongside the packages. The auto-updater selects the
asset whose name matches this OS and CPU before falling back to older release
naming, so every device receives its own build. macOS DMGs are ad-hoc signed;
if Gatekeeper blocks the first launch, right-click the app and choose **Open**.

On first run BlueDroid creates its per-user data root and, if needed, downloads
the Android SDK directly from Google's official XML manifests:

| Platform | Data root |
| --- | --- |
| Windows | `%LOCALAPPDATA%\BlueDroid` |
| Linux | `$XDG_DATA_HOME/BlueDroid` (or `~/.local/share/BlueDroid`) |
| macOS | `~/Library/Application Support/BlueDroid` |

On Windows it prefers an existing `C:\AndroidSDK`; otherwise it uses
`<data root>\sdk`. On Linux and macOS the local SDK lives under `<data root>/sdk`.
No JDK, `sdkmanager.bat`, or `avdmanager.bat` is required.

## Default configuration

The portable `emulator.ini` is copied to `<data root>/config/emulator.ini` on
first run (see the data-root table above). Edit the copy for future launches.

```ini
vmBackend=android
cpuCores=4
ramMB=4096
screenWidth=1080
screenHeight=1920
dpi=320
androidVersion=13
systemImage=android-33
instanceName=BlueDroid
enablePlayStore=true
enableGoogleServices=true
rootAccess=false
dataDir=./data
theme=Custom
uiScalePercent=100
fpsCap=60
gpuMode=0
minimizeToTray=true
muteParkedAudio=false
performancePreset=Balanced
```

`vmBackend=android` is the real backend. `vmBackend=mock` starts the demo
renderer without an SDK. `vmBackend=qemu` is accepted as an alias for the real
Android backend in this build.

## Documentation

- [User Guide](docs/USER_GUIDE.md) - daily controls, phones, APK installs, tray.
- [Storage and Isolation](docs/STORAGE.md) - every path, per-phone data rules,
  APK libraries, backups, and manual restore.
- [Architecture](docs/ARCHITECTURE.md) - process/thread model and code layout.
- [Setup](docs/SETUP.md) - first run, manual SDK setup, building.
- [Troubleshooting](docs/TROUBLESHOOTING.md) - logs and common failures.
- [Development](docs/DEVELOPMENT.md) - build targets, test hooks, source map.
