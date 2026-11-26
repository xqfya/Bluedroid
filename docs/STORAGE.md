# Storage and Phone Isolation

BlueDroid separates **manager state**, **Android phone data**, **APK libraries**,
**previews**, and **backups**. This document describes exactly what lives where
and what survives a restart.

## 1. Runtime root

All runtime files live under one per-user root:

```text
%LOCALAPPDATA%\BlueDroid
```

Fallback order used by `AppSetup`:

1. `LOCALAPPDATA\BlueDroid`
2. `USERPROFILE\AppData\Local\BlueDroid`
3. Current working directory (only if neither environment variable exists)

The portable files `emulator.ini` and `emulators.ini` in the application folder
are copied into the root on first run. After that, the copies under
`config\` are authoritative. `emulator.ini` now also stores the global gamer
settings: `theme`, `uiScalePercent`, `fpsCap`, `gpuMode`, `minimizeToTray`,
`muteParkedAudio`, and `performancePreset`. UI-only preferences and per-phone
overrides and the custom color palette (`useCustomColors`, `windowBgColor`,
`titleBarColor`, `panelColor`, `navBarColor`, `buttonColor`, `borderColor`,
`textColor`, `textMutedColor`, `successColor`, `dangerColor`) stay in
`emulators.ini`.

### Root layout

```text
%LOCALAPPDATA%\BlueDroid\
  config\
    emulator.ini          global emulator settings
    emulators.ini         saved phone list and per-phone hardware overrides
  avd\
    BD_cram\
    BD_cram.ini
    BD_cram.avd\
      config.ini                 AVD hardware + data partition path
      userdata-qemu.img          Android /data base image
      userdata-qemu.img.qcow2    live Android changes (apps, accounts, settings)
      cache.img
      sdcard.img                 if present
      snapshots\                 reserved; BlueDroid launches with -no-snapshot
    BD_drophi\
    BD_drophi.avd\
      ...
  data\
    phone_5554\
      apks\
        aptoide.apk              saved APK copy for this phone
        installed.txt            filename + install timestamp log
      userdata-qemu.img          per-phone VmSpec path; Android backend uses the AVD image above
    phone_5556\
      apks\
      ...
  thumbs\
    BD_cram.ppm                  still preview card image
    BD_drophi.ppm
  backups\
    BD_cram_20261125_153000\
      config.ini
      userdata-qemu.img
      userdata-qemu.img.qcow2
      sdcard.img
      apks\                      copy of that phone's APK library
  logs\
    emulator.log
  sdk\                           local SDK when C:\AndroidSDK is unavailable
  runtime\                       reserved; no JDK is needed
  temp\                          scratch files, per-port capture frames
  screenshot.ppm                 last manual screenshot
```

There is no shared `data\data.img` used by the Android backend. A stale file
with that name from an older build is ignored.

### Total storage needed

Actual usage varies with apps/games and how many backups you keep, but the
approximate budget is:

| Item | Typical size | Notes |
| --- | --- | --- |
| BlueDroid exe + config + logs | under 100 MB | Negligible. |
| Android Emulator package | ~1.1 GB | `emulator\`. |
| ADB platform-tools | ~20 MB | `platform-tools\`. |
| Android 13 Google APIs image | ~3.3 GB | `system-images;android-33;google_apis_playstore;x86_64`. Shared by all phones. |
| New AVD base files | ~100-200 MB | Base userdata, cache, encryption key, config. |
| Per-phone writable data | up to 6 GiB | `userdata-qemu.img.qcow2`; grows as apps/accounts/game data are added. |
| Per-phone APK library | sum of saved APK sizes | Example: `aptoide.apk` is about 22 MB. |
| Per-phone still preview | under 1 MB | `thumbs\<AVD>.ppm`. |
| Backup set | data used + APK library | One timestamped folder per backup operation. |
| Temp/capture scratch | under 100 MB | Usually much smaller. |

Observed local install for reference:

```text
C:\AndroidSDK                  about 4.5 GB (Java-era total was ~4.7 GB)
  emulator                      about 1.1 GB
  platform-tools                about 17 MB
  system-images                 about 3.3 GB

%LOCALAPPDATA%\BlueDroid\avd
  BD_cram.avd                   about 6.7 GB
  BD_drophi.avd                 about 6.0 GB
  BD_Work_Phone.avd             about 3.9 GB
```

The qcow2 userdata overlay is sparse/growing: the configured partition is 6 GiB,
but a newer phone may start much smaller and grow only as Android writes data.
Windows will show the file growing over time.

### Recommended free disk space

| Setup | Minimum comfortable free space |
| --- | --- |
| SDK only, no phones | 5-6 GB |
| One normal phone | 12-15 GB |
| Two phones | 20-25 GB |
| Four phones | 40-50 GB |
| Each additional phone | 6-10 GB |
| Each backup | roughly one phone's current data + APK library |

Keep at least a few GB extra for Windows paging, temp files, and Android updates
inside the guest.

## 2. How one phone is identified

A saved phone in `config\emulators.ini` has:

| Field | Purpose |
| --- | --- |
| `id` | Logical BlueDroid phone ID, used by the UI. |
| `name` | Display name. Renaming does not change storage. |
| `avd` | Physical AVD name, e.g. `BD_cram`. This is the Android data boundary. |
| `port` | Even ADB port starting at 5554, e.g. 5554 -> `emulator-5554`. |
| `paused` | UI state only; does not delete data. |
| hardware overrides | CPU, RAM, resolution, DPI, template. |

### AVD and port rules

- New phones get the first free `id` and first free even `port`.
- Initial AVD name is `BD_` + sanitized display name.
- If that AVD name already exists, BlueDroid appends `_2`, `_3`, etc.
- On load, duplicate/missing IDs, AVD names, or ports are repaired and saved.
  This prevents two logical phones from silently sharing one AVD or one ADB
  serial.

Because rename only changes `name`, renaming a phone never moves or loses its
Android data.

## 3. Android userdata isolation

The real backend launches:

```text
emulator.exe -avd <AVD> -port <port> -gpu host -no-snapshot
```

Before launch, BlueDroid rewrites that AVD's `config.ini`:

```ini
disk.dataPartition.path=<root>\avd\<AVD>.avd\userdata-qemu.img
```

Why this matters: Android's default `disk.dataPartition.path = <temp>` means a
temporary data partition. BlueDroid replaces it with the AVD-owned permanent
image and removes older duplicate lines. It also applies hardware settings:

```ini
hw.lcd.width
hw.lcd.height
hw.lcd.density
hw.ramSize
hw.cpu.ncore
vm.heapSize
```

The important data files are:

- `userdata-qemu.img` - base Android `/data` image.
- `userdata-qemu.img.qcow2` - overlay containing changes made while the phone
  runs. Installed apps, Google account state, app settings, and game progress
  are here.

Each AVD has its own pair. Phone A can never read Phone B's userdata because
the paths are different and the emulator is started with a different `-avd` and
`-port`.

### Data life cycle

| Action | What happens to data |
| --- | --- |
| Add phone | Profile saved; AVD is created on first open. |
| Open phone | AVD created if missing, config paths fixed, emulator launched or attached. |
| Back / park | Emulator keeps running. No data is cleared. |
| Restart manager while phone runs | BlueDroid reattaches to `emulator-<port>` by ADB. |
| Stop phone | Emulator processes are terminated. Userdata remains on disk. |
| Rename phone | Data untouched. |
| Delete phone | AVD directory, AVD ini, preview, and `data\phone_<port>` are deleted. This is destructive. |
| Reset | Current implementation only resets frame counters; it does not wipe Android data. |

## 4. APK storage and installation

When an APK is dropped onto BlueDroid or selected with **Install APK**:

1. BlueDroid shows a picker: all phones or specific phones.
2. For each target phone, it waits for ADB to be online.
3. The APK is copied to that phone's own library:

   ```text
   %LOCALAPPDATA%\BlueDroid\data\phone_<port>\apks\<sanitized-apk-name>.apk
   ```

4. A line is appended to `installed.txt` with the filename and timestamp.
5. The saved copy is uploaded as base64 through a hidden `adb.exe` process to:

   ```text
   /data/local/tmp/bluedroid_<port>_<apk-name>
   ```

6. BlueDroid runs:

   ```text
   adb -s emulator-<port> shell pm install -r -t /data/local/tmp/...
   ```

7. The temporary remote APK is removed on success, failure, or cancel.

Progress is computed from real APK bytes represented by blocks successfully
written into the ADB pipe. It is not a timer or mock animation.

The installed app itself lives in the AVD userdata (`userdata-qemu.img.qcow2`),
so it remains installed after stopping and reopening that phone. The host-side
APK copy is a per-phone library/record; it is not what Android boots from.

If the same APK name is installed again, the stored copy is overwritten only
when its size or last-write-time changed.

## 5. Backups

**Backup data** (card menu) or the toolbar backup path writes:

```text
%LOCALAPPDATA%\BlueDroid\backups\<AVD>_<YYYYMMDD_HHMMSS>\
```

Contents:

- `config.ini`
- `userdata-qemu.img`
- `userdata-qemu.img.qcow2`
- `sdcard.img` when present
- `apks\` - the full per-phone APK library

Prefer backing up a stopped phone so the qcow2 file is not changing during the
copy.

### Restoring a backup

There is no restore button yet. Manual restore:

1. Stop the target phone.
2. Copy `config.ini`, `userdata-qemu.img`, and `userdata-qemu.img.qcow2`
   from the backup folder into `%LOCALAPPDATA%\BlueDroid\avd\<AVD>.avd\`.
3. Copy the backup's `apks\` contents into
   `%LOCALAPPDATA%\BlueDroid\data\phone_<port>\apks\`.
4. Reopen the phone.

Back up before deleting a phone: deletion removes both the AVD directory and
the per-phone data directory.

## 6. Previews and screenshots

- `thumbs\<AVD>.ppm` is a small still image used on manager cards. It is
  refreshed periodically for parked phones and saved when parking/stopping.
  It is not an Android snapshot.
- `screenshot.ppm` at the root is the last manual screenshot from the emulator
  toolbar.
- Android snapshots are disabled with `-no-snapshot`; the `snapshots\`
  directory inside an AVD is reserved but not used for restore.

## 7. SDK and temp files

- `C:\AndroidSDK` is preferred when it already contains the emulator,
  platform-tools, and the Android 13 Google APIs image.
- Otherwise BlueDroid downloads manifest-selected ZIPs into `sdk\` using
  `temp\` for archives and staging. `runtime\` is reserved and no JDK is used.
- `ANDROID_SDK_ROOT`, `ANDROID_HOME`, `ANDROID_AVD_HOME`, `TMP`, and `TEMP`
  are set for BlueDroid and all hidden child processes.
- `ANDROID_AVD_HOME` is always the BlueDroid `avd\` folder, so the native AVD
  writer and emulator.exe do not write into a global `%USERPROFILE%\.android\avd`.

## 8. Logs

Runtime diagnostics are appended to:

```text
%LOCALAPPDATA%\BlueDroid\logs\emulator.log
```

The log includes AVD creation, emulator launch, boot milestones, capture FPS,
APK upload/install stages, stop/restart, and failure messages. When something
does not persist, check this first.
