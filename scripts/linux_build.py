#!/usr/bin/env python3
"""scripts/linux_build.py — the Linux host build (Quest port, phase 0): prepares the patched sm64coopdx tree, fetches
Lua, builds the upstream desktop game once (generated sources and lang/, mods/, palettes/, dynos/ resources), then
builds the static libraries (sm64_game, sm64_adapter, ar_core, ...) with CMake + Ninja into build/linux.
No ROM is needed to build: the game reads its assets from the ROM at run time.

  python3 scripts/linux_build.py               # gcc/g++ (or $CC/$CXX)
  python3 scripts/linux_build.py --clang       # clang/clang++, into build/linux-clang
"""
import argparse, os, sys
from sm64ar_common import ROOT, missing_dependencies, run


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clang", action="store_true", help="build with clang/clang++ (build/linux-clang)")
    ap.add_argument("--build-type", default="Release", help="CMAKE_BUILD_TYPE (default: Release)")
    args = ap.parse_args(argv)
    if sys.platform != "linux":
        print("error: this script builds on Linux; on a Mac use ./build.py", file=sys.stderr)
        return 1
    problem = missing_dependencies()
    if problem:
        print(f"error: {problem}", file=sys.stderr)
        return 1
    for s in ("sm64_prepare.py", "fetch_lua.py", "sm64_oracle.py"):
        if run([sys.executable, ROOT / "scripts" / s], check=False):
            return 1
    out = ROOT / "build" / ("linux-clang" if args.clang else "linux")
    cfg = ["cmake", "-S", ROOT, "-B", out, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.build_type}"]
    if args.clang:
        cfg += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
    if run(cfg, check=False) or run(["ninja", "-C", out, f"-j{os.cpu_count()}"], check=False):
        return 1
    print(f"built the Linux libraries in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
