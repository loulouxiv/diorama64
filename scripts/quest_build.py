#!/usr/bin/env python3
"""scripts/quest_build.py — builds the Meta Quest APK (apps/quest): checks the Android SDK, NDK, Java and CMake,
prepares the game sources and resources (sm64_prepare, fetch_lua, the upstream desktop build), then runs Gradle.

  python3 scripts/quest_build.py              # debug APK
  python3 scripts/quest_build.py --release    # release APK (optimised; signed with the debug key, for sideloading)
  python3 scripts/quest_build.py --install    # then install it (scripts/quest_install.py)

The SDK is found through $ANDROID_HOME (or a usual place); it needs the NDK named in apps/quest/app/build.gradle:
  sdkmanager "ndk;27.3.13750724" "platforms;android-34" "build-tools;34.0.0" "platform-tools"
No ROM is needed to build, and none goes into the APK."""
import argparse, pathlib, re, shutil, subprocess, sys
from quest_common import APK_DEBUG, APK_RELEASE, APP, NDK_VERSION, ROOT, android_sdk


def version_of(cmd, pattern):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True)
    except OSError:
        return None
    out = r.stdout + r.stderr
    m = re.search(pattern, out)
    return tuple(int(x) for x in m.group(1).split(".")) if m else None


def check(sdk):
    problems = []
    if not sdk:
        problems.append("no Android SDK: set ANDROID_HOME (install the command-line tools from developer.android.com)")
    elif not (sdk / "ndk" / NDK_VERSION).is_dir():
        problems.append(f'no NDK {NDK_VERSION} in {sdk}: sdkmanager "ndk;{NDK_VERSION}"')
    java = version_of(["java", "-version"], r'version "(\d+)')
    if not java or java[0] < 17:
        problems.append("Java 17 or newer is needed (the Android Gradle plugin): install a JDK")
    cmake = shutil.which("cmake")
    cv = version_of(["cmake", "--version"], r"cmake version (\d+\.\d+)") if cmake else None
    if not cv or cv < (3, 28):
        problems.append("CMake 3.28 or newer is needed (pip install cmake, or your package manager)")
    if not shutil.which("ninja"):
        problems.append("ninja is needed (pip install ninja, or your package manager)")
    return problems, cmake


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--release", action="store_true", help="the release APK")
    ap.add_argument("--install", action="store_true", help="install it afterwards (quest_install.py)")
    args = ap.parse_args(argv)
    sdk = android_sdk()
    problems, cmake = check(sdk)
    if problems:
        for p in problems:
            print(f"error: {p}", file=sys.stderr)
        return 1
    for s in ("sm64_prepare.py", "fetch_lua.py", "sm64_oracle.py"):
        if subprocess.run([sys.executable, str(ROOT / "scripts" / s)]).returncode:
            return 1
    # the Android Gradle plugin takes CMake (and ninja beside it or on PATH) from cmake.dir/bin
    cmake_dir = pathlib.Path(cmake).resolve().parent.parent.as_posix()
    (APP / "local.properties").write_text(f"sdk.dir={sdk.as_posix()}\ncmake.dir={cmake_dir}\n")
    gradlew = APP / ("gradlew.bat" if sys.platform == "win32" else "gradlew")
    task = "assembleRelease" if args.release else "assembleDebug"
    if subprocess.run([str(gradlew), "--no-daemon", task], cwd=APP).returncode:
        return 1
    apk = APK_RELEASE if args.release else APK_DEBUG
    print(f"built {apk} ({apk.stat().st_size / 1048576:.0f} MB)")
    if args.install:
        return subprocess.run([sys.executable, str(ROOT / "scripts" / "quest_install.py"), "--apk", str(apk)]).returncode
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
