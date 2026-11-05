#!/usr/bin/env python3
"""scripts/headless_check.py — phase 1 check of the Linux headless harness: records a run driven by a scripted pad
(apps/linux_headless/walk.pad), replays it in a fresh save folder, and checks that the replay plays every recorded tick
with no state-hash divergence, draws exactly as many draws per frame as the recording, and gives the same audio.
Needs the US ROM (local/baserom.us.z64 or --rom) and a build (python3 scripts/linux_build.py).
Output (recording, WAVs, logs) goes to local/headless-check/."""
import argparse, pathlib, re, shutil, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(exe, args, log):
    r = subprocess.run([str(exe), *args], cwd=ROOT, capture_output=True, text=True)
    log.write_text(r.stdout + r.stderr)
    lines = [l for l in r.stdout.splitlines() if l.startswith("AR_")]
    return r.returncode, lines


def field(lines, tag, key):
    for l in lines:
        if l.startswith(tag + " "):
            m = re.search(rf"\b{key}=(\S+)", l)
            if m:
                return m.group(1)
    return None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build/linux", help="build folder (default build/linux)")
    ap.add_argument("--rom", default="local/baserom.us.z64")
    ap.add_argument("--frames", type=int, default=600)
    args = ap.parse_args(argv)
    exe = ROOT / args.build / "headless" / "sm64ar_headless"
    if not exe.is_file():
        print(f"error: no {exe} (build it: python3 scripts/linux_build.py)", file=sys.stderr)
        return 1
    if not (ROOT / args.rom).is_file():
        print(f"error: no ROM at {args.rom} (the US ROM, .z64: scripts/rom_to_z64.py converts other byte orders)", file=sys.stderr)
        return 1
    out = ROOT / "local" / "headless-check"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    common = ["--rom", args.rom, "--frames", str(args.frames), "--stats-every", "1", "--log-hash"]
    pad = ROOT / "apps" / "linux_headless" / "walk.pad"
    rc1, rec = run(exe, [*common, "--save", out / "rec-save", "--pad-file", pad, "--record", out / "walk.sarec",
                         "--audio-out", out / "rec.wav"], out / "record.log")
    rc2, rep = run(exe, [*common, "--save", out / "rep-save", "--sarec", out / "walk.sarec",
                         "--audio-out", out / "rep.wav"], out / "replay.log")
    problems = []
    if rc1 or rc2:
        problems.append(f"exit codes: record {rc1}, replay {rc2} (logs in {out})")
    frames_rec = [l for l in rec if l.startswith("AR_FRAME ")]
    frames_rep = [l for l in rep if l.startswith("AR_FRAME ")]
    if len(frames_rec) != args.frames or frames_rec != frames_rep:
        bad = next((i for i, (a, b) in enumerate(zip(frames_rec, frames_rep)) if a != b), min(len(frames_rec), len(frames_rep)))
        problems.append(f"draw counts differ from frame {bad} ({len(frames_rec)} recorded, {len(frames_rep)} replayed)")
    ticks = field(rec, "AR_RECORD", "ticks")
    played, checked, diverged = (field(rep, "AR_REPLAY", k) for k in ("played", "checked", "diverged"))
    if not ticks or played != ticks or checked != ticks or diverged != "0" or field(rep, "AR_REPLAY", "ended_early"):
        problems.append(f"replay: recorded {ticks} ticks, played {played}, checked {checked}, diverged {diverged}")
    if field(rec, "AR_SUMMARY", "hash") != field(rep, "AR_SUMMARY", "hash"):
        problems.append("final state hashes differ")
    wav_rec, wav_rep = (out / "rec.wav").read_bytes() if (out / "rec.wav").exists() else b"", \
                       (out / "rep.wav").read_bytes() if (out / "rep.wav").exists() else b""
    if not wav_rec or wav_rec != wav_rep:
        problems.append("the replay's audio differs from the recording's")
    peak = int(field(rep, "AR_AUDIO", "peak") or 0)
    if peak < 1000:
        problems.append(f"the audio is (nearly) silent: peak {peak}")
    print(f"recorded {ticks} ticks; replay played {played}, checked {checked}, diverged {diverged}; "
          f"{len(frames_rep)} frames with matching draw counts; audio {field(rep, 'AR_AUDIO', 'seconds')} s, peak {peak}")
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    if not problems:
        print(f"OK (output in {out.relative_to(ROOT)})")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
