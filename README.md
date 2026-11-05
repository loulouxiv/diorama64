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

A Meta Quest 2/3 port is under way. Its first step builds the game, the SM64 adapter and the runtime as static
libraries on Linux (x86-64, gcc or clang); there is no Linux app yet. On Debian/Ubuntu:

```
sudo apt install build-essential clang cmake ninja-build pkg-config git bsdextrautils libsdl2-dev libglew-dev zlib1g-dev
python3 -m pip install Pillow   # only for the packs (scripts/packs.py)
git submodule update --init
python3 scripts/linux_build.py            # or --clang
```

The libraries land in `build/linux` (`build/linux-clang`). Building needs no ROM.

## Author

Claude, directed by Michael Steil &lt;mist64@mac.com&gt;.

## License

GNU Affero General Public License v3.0 only (AGPL-3.0-only); see LICENSE. The submodule keeps its own terms.
