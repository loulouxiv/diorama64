#!/usr/bin/env python3
"""scripts/quest_logs.py — the app's log from the headset (logcat tags Diorama64, Diorama64-out and the OpenXR
runtime's), live or as it is now, optionally saved to a file to send back.

  python3 scripts/quest_logs.py                    # live, until Ctrl-C
  python3 scripts/quest_logs.py --dump --save local/quest.log
  python3 scripts/quest_logs.py --clear            # empty the headset's log first (then live)"""
import argparse, pathlib, subprocess, sys
from quest_common import adb, need_device

TAGS = ["Diorama64:V", "Diorama64-out:V", "OpenXR:V", "VrApi:I", "AndroidRuntime:E", "DEBUG:V", "libc:E", "*:S"]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", action="store_true", help="what is there now, then exit")
    ap.add_argument("--clear", action="store_true", help="clear the log first")
    ap.add_argument("--save", type=pathlib.Path, help="also write it to this file")
    ap.add_argument("--serial")
    args = ap.parse_args(argv)
    a = adb()
    serial = need_device(a, args.serial)
    if args.clear:
        subprocess.run([a, "-s", serial, "logcat", "-c"])
    cmd = [a, "-s", serial, "logcat", "-v", "time", *(["-d"] if args.dump else []), *TAGS]
    out = args.save.open("w") if args.save else None
    try:
        with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, errors="replace") as p:
            for line in p.stdout:
                sys.stdout.write(line)
                if out:
                    out.write(line)
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()
            print(f"saved {args.save}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
