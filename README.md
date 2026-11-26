# BlueDroid Emulator

BlueDroid is a Windows desktop manager for multiple real Android phones. It is
written in C/C++ and uses a Dear ImGui + Direct3D 11 interface over Win32.

The default backend launches the official Android Emulator with an Android 13
Google APIs system image. Phones run in hidden emulator windows; BlueDroid
draws the live Android capture inside its own phone frame.

The interface is a custom Dear ImGui shell rendered through Direct3D 11, with
optional HD post-processing shaders and per-user appearance settings.

## Highlights

- Multiple Android phones, each with its own AVD, ADB port, Android data, and
  APK library.
- Custom borderless UI with rounded corners, top-bar dragging, a single custom
  color palette (no preset themes), UI scale 100-150% with `Ctrl+0` emergency
  reset, fonts, and DX11 HD shaders (Standard / HD / Ultra HD).
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

```sh
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
make -j4
./bin/BlueDroidEmulator.exe
```

On first run BlueDroid creates `%LOCALAPPDATA%\BlueDroid` and, if needed,
downloads the Android SDK directly from Google's official XML manifests. It
prefers an existing `C:\AndroidSDK`; otherwise it uses
`%LOCALAPPDATA%\BlueDroid\sdk`. No JDK, `sdkmanager.bat`, or `avdmanager.bat`
is required.

## Default configuration

The portable `emulator.ini` is copied to
`%LOCALAPPDATA%\BlueDroid\config\emulator.ini` on first run. Edit the copy for
future launches.

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
