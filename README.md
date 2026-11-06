# Diorama64

Diorama64 plays **Super Mario 64** as an augmented-reality diorama on **Apple Vision Pro**. The level stands on your real
table as a small model that you walk around, while you play with a game controller. It is built on
[sm64coopdx](https://github.com/coop-deluxe/sm64coopdx) with a custom Metal renderer.

More info and videos: <https://www.pagetable.com/303>

Status: alpha.

## Disclaimer

Diorama64 is an unofficial fan project, not affiliated with or endorsed by Nintendo. It contains no game data; you need
your own Super Mario 64 (US) ROM.

## Requirements

- A Mac with Apple silicon.
- Xcode 26 or newer with the visionOS SDK (tested with Xcode 26.3).
- [Homebrew](https://brew.sh).
- Apple Vision Pro on visionOS 26 or later.
- An Apple Account.
- A Bluetooth game controller.

## Setup

```
git clone https://github.com/mist64/diorama64.git
cd diorama64
./build.py
```

`./build.py` checks your Mac, then asks for your ROM (any byte order), the HD texture and 3D model packs to add (Enter for
all, or `--packs`), your team and the headset. It builds Diorama64 and installs it on the headset. Your
ROM and the packs go inside the app. Running it again reuses the answers (`--reconfigure` asks again; `--help` lists
the flags).

On the headset, press **Open table** and look at your table. **Re-place table** in the window starts the search again.

## Settings

The app's window has four tabs (Level, World, Look, Audio). Every setting applies live and is saved.

## Linux build (Quest port, work in progress)

A Meta Quest 2/3 port is under way. On Linux (x86-64, gcc or clang) it builds the game, the SM64 adapter, the runtime
and a Vulkan renderer, plus two development harnesses:

- `sm64ar_headless`: the game without a window. `--device null` (default) counts the draws; `--device vulkan` draws them
  (on any Vulkan GPU, or lavapipe, Mesa's software renderer) and writes PNGs. Recordings, replays, audio dumps.
- `sm64ar_window`: the diorama in a window, with a free camera, played with a gamepad or the keyboard.

On Debian/Ubuntu:

```
sudo apt install build-essential clang cmake ninja-build pkg-config git bsdextrautils libsdl2-dev libglew-dev zlib1g-dev \
                 libvulkan-dev mesa-vulkan-drivers libshaderc-dev vulkan-validationlayers
python3 -m pip install Pillow   # packs (scripts/packs.py) and image comparisons
git submodule update --init
python3 scripts/linux_build.py            # or --clang; output in build/linux (build/linux-clang)
```

Building needs no ROM. Running does: put your US ROM at `local/baserom.us.z64` (`scripts/rom_to_z64.py` converts
`.v64`/`.n64`). Then:

```
build/linux/headless/sm64ar_window --look roomlit --msaa 4          # W/A/S/D/Q/E + right mouse: camera; arrows, X, C, Z, Enter: game
build/linux/headless/sm64ar_headless --help
build/linux/headless/sm64ar_headless --frames 300 --audio-out local/out.wav     # 10 s of Bob-omb Battlefield
build/linux/headless/sm64ar_headless --device vulkan --look roomlit --png-at 0,299 --out local/png
build/linux/headless/sm64ar_headless --pad-file apps/linux_headless/walk.pad --record local/walk.sarec
build/linux/headless/sm64ar_headless --sarec local/walk.sarec                   # replays, checking state hashes
python3 scripts/headless_check.py         # records, replays and compares (hashes, draw counts, audio)
python3 scripts/compare_images.py A B     # Vulkan PNGs against Metal ones (see the script for the commands)
```

## Meta Quest 2 / 3 (work in progress)

The Quest app (`apps/quest`) builds and installs, but shows nothing yet: the game, its sound and the app's lifecycle run;
the picture (OpenXR) comes next. To try it you need a headset in developer mode (Meta Horizon app on your phone), a USB-C
cable, the Android SDK with its command-line tools, Java 17+, CMake 3.28+ and ninja:

```
sdkmanager "ndk;27.3.13750724" "platforms;android-34" "build-tools;34.0.0" "platform-tools"
export ANDROID_HOME=~/Android/Sdk               # where the SDK is
python3 scripts/quest_build.py                  # apps/quest/app/build/outputs/apk/debug/app-debug.apk (--release: optimised)
python3 scripts/quest_install.py                # installs it, pushes local/baserom.us.z64 to the headset, starts it
python3 scripts/quest_logs.py --save local/quest.log   # the app's log (Ctrl-C to stop)
```

Your ROM goes only to the app's folder on the headset (`/sdcard/Android/data/diorama64.quest/files/`), never into the
APK. The log reports the OpenXR runtime's extensions, the game's start, and every 5 s its ticks, draws and audio.

## Author

Claude, directed by Michael Steil &lt;mist64@mac.com&gt;.

## License

GNU Affero General Public License v3.0 only (AGPL-3.0-only); see LICENSE. The submodule keeps its own terms.
