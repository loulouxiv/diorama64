#!/usr/bin/env python3
"""scripts/quest_install.py — installs Diorama64 on a Meta Quest over adb, pushes your ROM to the app's folder on the
headset (/sdcard/Android/data/diorama64.quest/files/baserom.us.z64; never into the APK), and starts it.

  python3 scripts/quest_install.py                       # the debug APK, the ROM from local/baserom.us.z64
  python3 scripts/quest_install.py --rom ROM.n64         # any byte order: converted and checked (SHA-1 of the US ROM)
  python3 scripts/quest_install.py --release --no-start
The headset must be in developer mode, connected (USB-C or adb over Wi-Fi) and allow debugging."""
import argparse, pathlib, subprocess, sys, tempfile
from quest_common import ACTIVITY, APK_DEBUG, APK_RELEASE, DEVICE_FILES, PACKAGE, ROOT, adb, need_device

sys.path.insert(0, str(ROOT / "scripts"))
import rom_to_z64  # noqa: E402


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apk", type=pathlib.Path, help="the APK (default: the debug one, or --release)")
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--rom", type=pathlib.Path, default=ROOT / "local" / "baserom.us.z64")
    ap.add_argument("--no-rom", action="store_true", help="leave the ROM on the headset as it is")
    ap.add_argument("--no-start", action="store_true")
    ap.add_argument("--serial", help="the adb serial of the headset when several are connected")
    args = ap.parse_args(argv)
    apk = args.apk or (APK_RELEASE if args.release else APK_DEBUG)
    if not apk.is_file():
        print(f"error: no APK at {apk}: build it with python3 scripts/quest_build.py{' --release' if args.release else ''}", file=sys.stderr)
        return 1
    a = adb()
    serial = need_device(a, args.serial)
    run = lambda *cmd: subprocess.run([a, "-s", serial, *cmd]).returncode
    print(f"installing {apk.name} on {serial}")
    # -g: grant the runtime permissions it declares (USE_SCENE, USE_ANCHOR_API) without a prompt in the headset
    if run("install", "-r", "-g", str(apk)):
        return 1
    if not args.no_rom:
        if not args.rom.is_file():
            print(f"error: no ROM at {args.rom} (--rom PATH: your Super Mario 64 US ROM)", file=sys.stderr)
            return 1
        data = rom_to_z64.to_z64(args.rom.read_bytes())
        want = rom_to_z64.expected_sha1()
        import hashlib
        if want and hashlib.sha1(data).hexdigest() != want:
            print(f"error: {args.rom} is not the Super Mario 64 (US) ROM (SHA-1 differs)", file=sys.stderr)
            return 1
        with tempfile.TemporaryDirectory() as d:
            z64 = pathlib.Path(d) / "baserom.us.z64"
            z64.write_bytes(data)
            if run("shell", "mkdir", "-p", DEVICE_FILES) or run("push", str(z64), f"{DEVICE_FILES}/baserom.us.z64"):
                return 1
        print(f"ROM pushed to {DEVICE_FILES}/baserom.us.z64")
    if not args.no_start:
        run("shell", "am", "force-stop", PACKAGE)
        if run("shell", "am", "start", "-n", ACTIVITY):
            return 1
        print("started: put the headset on; logs with python3 scripts/quest_logs.py")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
