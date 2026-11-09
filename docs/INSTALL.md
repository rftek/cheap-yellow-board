# Build Environment and Installation

How to set up a PC from scratch to build every application in this repository, flash an ESP32 Cheap Yellow Display (CYD), and publish new boot images. Everything here was used and verified on the machine that built this project. Exact versions are listed at the end.

## At a glance

| You need | Required? | Why |
|---|---|---|
| A **CYD board** (ESP32-2432S028R) and a USB data cable | Yes | The hardware. It shows up as a USB serial port. |
| **USB-serial driver** (CH340) | Usually | Windows needs it to see the board's COM port. Some boards use a CP2102 instead. |
| **Git** | Yes | To clone the repository and the Bluepad32 submodule. |
| **PlatformIO Core** (`pio`) | Yes | Builds everything. It downloads ESP-IDF, the compiler and esptool itself. |
| **Python 3** | Indirectly | PlatformIO runs on Python. Its installers set up their own environment. |
| **Visual Studio Code** + PlatformIO IDE extension | **No**, optional | The easiest way to install PlatformIO, plus an editor and build buttons. Every build and flash in this project also works from a terminal. |
| ESP-IDF, compilers, CMake, esptool | No separate install | PlatformIO fetches them automatically on the first build. |
| LVGL | No separate install | Fetched at build time by the ESP-IDF component manager for `apps/stopwatch` and `apps/gear_roll`. |
| A Wi-Fi network, and a private credentials file | For the launcher | Only the launcher has credentials compiled in. Apps get them from the board at run time. |
| A GitHub account | To publish images | Changes reach `main` through pull requests; GitHub Actions then builds the apps and publishes their binaries. |
| GitHub CLI (`gh`) | No | Not used. CI uses it on GitHub's side only. |

**High level:** install Git, install PlatformIO (through VS Code or on its own), clone the repository to a short path, create your private Wi-Fi file, then build the launcher in `launcher/` and flash it once with esptool. After that, the board's launcher downloads every other application from GitHub by itself. You only build an application locally when you change it.

## 1. Install the USB driver and find the board's port

1. Plug the CYD in with a data cable. Some cables only carry power.
2. Open **Device Manager → Ports (COM & LPT)**. You should see a port such as **USB-SERIAL CH340 (COM11)**. The number varies by PC; use yours wherever these docs say `COM11`.
3. If nothing appears, or the device has a warning icon, install the **CH340/CH341** driver from the chip maker (WCH). If your board has a Silicon Labs chip instead, install the **CP210x** driver.

## 2. Install Git

Install [Git for Windows](https://git-scm.com/download/win) with the default options. Then, in a new terminal:

```bash
git config --global core.longpaths true
```

The repository uses short paths, but Git's own internals can still pass 260 characters.

## 3. Install PlatformIO: pick one

### Option A: VS Code and the PlatformIO IDE extension (recommended for most people)

1. Install [Visual Studio Code](https://code.visualstudio.com/).
2. In VS Code, open **Extensions**, search for **PlatformIO IDE**, and install it. On first start it installs **PlatformIO Core** into `%USERPROFILE%\.platformio\penv`. This takes a few minutes, and it finds or downloads a suitable Python itself.
3. When you open the repository folder, VS Code recommends this extension, from `.vscode/extensions.json`. That file also tells VS Code *not* to suggest the C/C++ extension pack or `pioarduino`, which would conflict with PlatformIO's own setup.

The `pio` command lives at `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`. These docs and the app READMEs call it by that full path, so it works even when `pio` isn't on your PATH. To type plain `pio` instead, add `%USERPROFILE%\.platformio\penv\Scripts` to your user PATH. The VS Code tasks in `.vscode/tasks.json` (Build, Upload, Monitor) ask for the project folder and the target, and assume `pio` is on the PATH.

### Option B: PlatformIO Core on its own (no VS Code)

1. Install [Python 3](https://www.python.org/downloads/) (3.10 or newer; 3.13 is used here, 3.12 in CI). Tick **Add python.exe to PATH**.
2. Install PlatformIO Core:

   ```bash
   python -m pip install --user platformio
   ```

   Alternatively, use PlatformIO's official installer script, which creates the same `%USERPROFILE%\.platformio\penv` environment as the VS Code extension. Then the paths in these docs work unchanged.

Either way, check it:

```bash
pio --version
```

Version 6.2.0 is used here. Any 6.x should work.

## 4. Get the source

Clone to a **short path**. The ESP-IDF build fails on Windows when a source path passes 260 characters, and Windows long-path support is not required if the path stays short. `C:\Users\<you>\Documents\cheap-yellow-board` works. Avoid deeply nested folders and app-virtualized locations under `AppData`.

```bash
git clone --single-branch --no-tags https://github.com/rftek/cheap-yellow-board.git
cd cheap-yellow-board
```

Clone once: every application is a folder in this one checkout. `--single-branch` fetches only `main`. A plain clone also downloads the `bruce` branch, about 190 MB, almost all of it Bruce's history. `--no-tags` leaves out the release tags, which share the app folder names and aren't needed to build.

What's where:

| Folder | Contents | Published by CI |
|---|---|---|
| `apps/<name>` | One application each; the [root README](../README.md#applications) lists them | Yes, on the release tagged `<name>` |
| `launcher` | Launcher (factory app), custom second-stage bootloader, partition table | No, because it contains your Wi-Fi credentials |
| `components` | `cyd_display` (display, fonts, touch) and `cyd_gamepad` (Xbox controller input), shared by the apps | Built into the apps |
| `external/bluepad32` | Bluepad32 submodule, with BTstack nested in it | Built into `xbox_controller` and `catmouse_pad` |
| `docs` | This documentation | No |
| `tools`, `.github/workflows` | CI and the app catalog | `manifest.json`, on the release tagged `catalog` |

Each app folder is a complete PlatformIO project. It uses the one shared copy of `components/cyd_display` at the repository root, through `EXTRA_COMPONENT_DIRS` in its `CMakeLists.txt`.

### Bluepad32, for `xbox_controller` and `catmouse_pad`

These two apps need the submodule, its BTstack patch, and two environment variables. In Git Bash, from the repository root:

```bash
git submodule update --init --recursive external/bluepad32
export BLUEPAD32_ROOT="$PWD/external/bluepad32"
export BTSTACK_ROOT="$BLUEPAD32_ROOT/external/btstack"
git -C "$BTSTACK_ROOT" apply "$BLUEPAD32_ROOT"/external/patches/*.patch
```

The patch is applied once; the variables must be set in every terminal you build these apps from. In PowerShell, set them with forward slashes, for example `$env:BLUEPAD32_ROOT = "C:/Users/<you>/Documents/cheap-yellow-board/external/bluepad32"` and `$env:BTSTACK_ROOT = "$env:BLUEPAD32_ROOT/external/btstack"`. The other apps don't need any of this.

## 5. Create your private Wi-Fi file (for the launcher)

Create `%USERPROFILE%\.cyd_private\wifi_credentials.h` (for example `C:\Users\<you>\.cyd_private\wifi_credentials.h`):

```c
#pragma once

#define WIFI_SSID "your-wifi-network"
#define WIFI_PASSWORD "your-wifi-password"
/* Optional: BirdNET server for the birdnet_viewer image. */
#define BIRDNET_BASE_URL "http://192.168.1.50"
```

The same template is at `launcher/main/wifi_credentials.example.h`. The file lives outside the repository on purpose: **never commit it**. The launcher build stops with a clear message if it's missing.

The launcher copies these settings into the board's NVS storage, where the apps read them. Apps can also change them on the device, for example with the CONNECT screen in `wifi_checker`.

## 6. Build

Every app builds the same way, from inside its folder:

```bash
cd apps/screen_check
"$USERPROFILE/.platformio/penv/Scripts/pio.exe" run -e esp32
```

In PowerShell:

```powershell
cd apps\screen_check
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32
```

The launcher builds the same way from `launcher`. `-e esp32` builds for the CYD, and it's the default, so plain `pio run` does the same. `-e esp32s3` builds for the Waveshare ESP32-S3-Touch-LCD-7, for the apps that list `esp32s3` in `supported-targets.txt`.

What to expect:

- **First build on a new PC:** PlatformIO downloads the pinned `espressif32@7.1.3` platform, with ESP-IDF 6.1.0, the Xtensa GCC 15.2 toolchain, CMake, Ninja and esptool. That's a large download, and it lands in `%USERPROFILE%\.platformio`.
- **First build of each app:** about 7 to 17 minutes, because each app has its own build folder (`build_dir` in its `platformio.ini`, `%USERPROFILE%\.platformio\cyd-*-build`) and compiles all of ESP-IDF. LVGL apps take the longest. Later builds take seconds to a couple of minutes.
- **LVGL apps** (`stopwatch`, `gear_roll`) download LVGL into the app's `managed_components/`, which is git-ignored, at the exact version in its `dependencies.lock`.
- **Output:** `firmware.bin` in the target's subfolder of that app's build folder, such as `%USERPROFILE%\.platformio\cyd-screen-check-build\esp32`. The launcher also produces `bootloader.bin`, `partitions.bin` and `ota_data_initial.bin`.
- **After editing `sdkconfig.defaults`:** delete the generated `sdkconfig.esp32` (or `sdkconfig.esp32s3`) in the app's folder, so the new defaults apply. Expect a full rebuild.

## 7. Flash a new board (once)

The launcher setup needs its own bootloader, partition table and a blank `otadata`. Flashing all of them explicitly with **esptool**, at these offsets, is the verified way. `pio run --target upload` in `launcher` was not tested for this layout, and it would write a stock bootloader. Build the launcher first (`cd launcher`, then `& $pio run -e esp32`), then:

```powershell
$py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$esptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$L = "$env:USERPROFILE\.platformio\cyd-launcher-build\esp32"
& $py $esptool --chip esp32 -p COM11 -b 230400 write_flash `
    0x1000 "$L\bootloader.bin" 0x8000 "$L\partitions.bin" `
    0xd000 "$L\ota_data_initial.bin" 0x10000 "$L\firmware.bin"
```

Do this once per board. Don't run `erase_flash`: it would also wipe the image slots and the settings in NVS. To update the launcher later, write only its app partition:

```powershell
& $py $esptool --chip esp32 -p COM11 -b 230400 write_flash 0x10000 "$L\firmware.bin"
```

Power-cycle the board. The launcher joins your Wi-Fi, lists every application in the app catalog that has a published build, and downloads the one you tap into one of its two slots. To watch the serial log:

```powershell
& $pio device monitor --port COM11 --baud 115200
```

Ctrl+] exits.

To put a locally built app into a slot without GitHub, write its `firmware.bin` to `0x150000` (slot A) or `0x2A0000` (slot B):

```powershell
& $py $esptool --chip esp32 -p COM11 -b 230400 write_flash 0x150000 path\to\firmware.bin
```

An application can also run on its own, without the launcher: `& $pio run -e esp32 --target upload --upload-port COM11` in that app's folder. This replaces everything on the board, including the launcher.

## 8. Publish a new or changed application

`main` is protected, so every change goes through a pull request. Push your change on a branch named as in the [README](../README.md#contributing) (such as `app/stopwatch-lap-times`) and open a pull request. `.github/workflows/build.yml` runs on GitHub Actions: `tools/ci_plan.py` picks the apps the change touches (all of them when `components/`, `external/`, `tools/` or the workflow change), and each is built with `pio run -e <target>` for every target in its `supported-targets.txt`. The pull request can merge only when those builds pass.

After the merge, the same workflow runs on `main`, builds those apps again, and uploads each image to the release tagged with the app's folder name, creating the release the first time and moving its tag to the commit it built. The esp32 image is `firmware.bin`; other targets are `firmware-<target>.bin`, such as `firmware-esp32s3.bin`. Then `tools/make_catalog.py` rewrites `manifest.json`, the app catalog, on the release tagged `catalog`. Changes that touch only Markdown files, `docs/` or `launcher/` build nothing. On its next start, the launcher shows the new build as an update.

To add a new application, copy an existing app folder (for example `apps/screen_check`, or `apps/stopwatch` for LVGL) to `apps/<name>`, change `project(...)` in `CMakeLists.txt` and `build_dir` in `platformio.ini`, list the targets it supports in `supported-targets.txt`, set `version.txt` to `1.0`, write the app in `main/`, replace the README's changelog with a `### 1.0` entry for the new app, and open a pull request. Every later change to the app bumps `version.txt` and adds a changelog entry; see [Versions](../README.md#versions). The `project(...)` name must be exactly the folder name, case included: it is the app's release tag and catalog name, and the launcher compares the name in a slot's image with the app it came from; on a mismatch it keeps offering the app as an update. Then add the app to the table in the root `README.md`. Boot images must stay under 1.31 MB, the slot size, and must never contain credentials, because their binaries are public.

The workflow needs the repository's Actions to have **read and write** permission for contents, so it can create releases. It asks for this with `permissions: contents: write`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `cc1.exe: fatal error: /?/C:/...` or other path errors | The project path is too long. Clone to a shorter folder. |
| esptool: `The chip stopped responding` | Retry at a lower baud rate, such as `-b 115200`. Use a short, good USB cable. |
| esptool can't open the port | Close any serial monitor, or other program, using the COM port. |
| A `sdkconfig.defaults` change has no effect | Delete `sdkconfig.esp32` in the app's folder and rebuild. |
| Build stops asking for `wifi_credentials.h` | Create it as in step 5. Only the launcher needs it. |
| `xbox_controller` or `catmouse_pad` fails in CMake on `BTstack_project.cmake` | Set `BLUEPAD32_ROOT` and `BTSTACK_ROOT` and check out the submodule, as in step 4. |
| Build fails on size | The esp32 build checks against the default 1 MB app partition. Keep standalone builds under 1 MB, or point the check at the launcher's partition table. |
| Touches land in the wrong place | Run the `touchscreen_cal` app from the launcher. Every app uses the saved calibration. |

## Tested versions

| Component | Version |
|---|---|
| Windows | 10 Pro 22H2 (build 19045), long-path support **off** |
| Git | 2.55.0.windows.5 |
| Visual Studio Code extension | PlatformIO IDE 3.3.4 |
| PlatformIO Core | 6.2.0 (Python 3.13.0 in its `penv`) |
| PlatformIO platform | espressif32 7.1.3 (pinned in every `platformio.ini`) |
| ESP-IDF | 6.1.0 (`framework-espidf` 4.60100.0) |
| Compiler | `toolchain-xtensa-esp-elf` 15.2.0+20251204 |
| CMake / Ninja | 3.30.2 / 1.9.0 (PlatformIO packages) |
| esptool | 4.11.0 |
| LVGL | 9.6.0 (ESP-IDF component manager, pinned by `dependencies.lock`) |
| USB-serial chip | WCH CH340 (USB ID 1A86:7523) |
| CI | GitHub Actions `ubuntu-latest`, Python 3.12, PlatformIO from `pip` |
