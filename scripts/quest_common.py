"""scripts/quest_common.py — what the Quest scripts share: the Android SDK, adb, the app's package and folders."""
import os, pathlib, shutil, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
APP = ROOT / "apps" / "quest"
PACKAGE = "diorama64.quest"
ACTIVITY = f"{PACKAGE}/android.app.NativeActivity"
DEVICE_FILES = f"/sdcard/Android/data/{PACKAGE}/files"     # the app's external files folder: the ROM goes here
NDK_VERSION = "27.3.13750724"                               # apps/quest/app/build.gradle ndkVersion
APK_DEBUG = APP / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk"
APK_RELEASE = APP / "app" / "build" / "outputs" / "apk" / "release" / "app-release.apk"
SDK_PLACES = ("~/Android/Sdk", "~/Library/Android/sdk", "/opt/android-sdk", "/usr/lib/android-sdk")


def android_sdk():
    """The Android SDK folder: $ANDROID_HOME, $ANDROID_SDK_ROOT, else a usual place; None if there is none."""
    for p in (os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"), *SDK_PLACES):
        if not p:
            continue
        d = pathlib.Path(p).expanduser()
        if (d / "platform-tools").is_dir() or (d / "ndk").is_dir():
            return d
    return None


def adb():
    sdk = android_sdk()
    cand = sdk / "platform-tools" / ("adb.exe" if os.name == "nt" else "adb") if sdk else None
    if cand and cand.is_file():
        return str(cand)
    found = shutil.which("adb")
    if not found:
        sys.exit("error: adb not found: install the Android SDK platform-tools (sdkmanager platform-tools) or put adb on PATH")
    return found


def device_serials(adb_path):
    r = subprocess.run([adb_path, "devices"], capture_output=True, text=True)
    return [l.split()[0] for l in r.stdout.splitlines()[1:] if l.strip().endswith("device")]


def need_device(adb_path, serial=None):
    """The serial of the one headset to talk to (or the one asked for); exits with advice when there is none."""
    serials = device_serials(adb_path)
    if serial:
        if serial not in serials:
            sys.exit(f"error: {serial} is not connected (adb devices lists: {', '.join(serials) or 'nothing'})")
        return serial
    if not serials:
        sys.exit("error: no headset found by adb. Enable developer mode (Meta Horizon app on your phone), connect the headset with "
                 "USB-C, put it on and allow USB debugging, then check with: adb devices")
    if len(serials) > 1:
        sys.exit(f"error: several devices connected ({', '.join(serials)}): pick one with --serial")
    return serials[0]
