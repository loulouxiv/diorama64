#!/usr/bin/env python3
"""scripts/compare_images.py — compares the frame_NNNN.png of two harness runs (the Vulkan backend against the Metal one,
Quest port phase 2): PSNR, share of pixels that differ visibly, and a diff image per frame.

Make the two sets with the same recording, frames and look. On a Mac (Metal), from a build of the macOS harness:
  build/macos/harness/bin/sm64ar_harness --offscreen --out local/cmp-metal --sarec local/walk.sarec \\
      --frames 200 --png-at 60,120,199 [--look roomlit=1 --msaa 4]
On Linux (Vulkan):
  build/linux/headless/sm64ar_headless --device vulkan --out local/cmp-vulkan --sarec local/walk.sarec \\
      --frames 200 --png-at 60,120,199 [--look roomlit --msaa 4]
Then:
  python3 scripts/compare_images.py local/cmp-metal local/cmp-vulkan
Needs Pillow (python3 -m pip install Pillow). Exit status 1 when a frame is below --min-psnr."""
import argparse, math, pathlib, sys


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", type=pathlib.Path)
    ap.add_argument("b", type=pathlib.Path)
    ap.add_argument("--min-psnr", type=float, default=30.0, help="dB below which a frame fails (default 30)")
    ap.add_argument("--tolerance", type=int, default=8, help="a pixel differs when a channel differs by more (default 8)")
    ap.add_argument("--diff-dir", type=pathlib.Path, help="where to write diff_NNNN.png (default: B/diff)")
    args = ap.parse_args(argv)
    try:
        from PIL import Image, ImageChops
    except ImportError:
        print("error: needs Pillow: python3 -m pip install Pillow", file=sys.stderr)
        return 2
    names = sorted(p.name for p in args.a.glob("frame_*.png") if (args.b / p.name).is_file())
    if not names:
        print(f"error: no frame_*.png in both {args.a} and {args.b}", file=sys.stderr)
        return 2
    diff_dir = args.diff_dir or args.b / "diff"
    diff_dir.mkdir(parents=True, exist_ok=True)
    worst = math.inf
    for n in names:
        a = Image.open(args.a / n).convert("RGB")
        b = Image.open(args.b / n).convert("RGB")
        if a.size != b.size:
            print(f"{n}: sizes differ ({a.size} vs {b.size})")
            worst = 0.0
            continue
        d = ImageChops.difference(a, b)
        hist = d.histogram()                       # 3 x 256 bins
        sq = sum(c * (i % 256) ** 2 for i, c in enumerate(hist))
        mse = sq / (a.size[0] * a.size[1] * 3)
        psnr = math.inf if mse == 0 else 10 * math.log10(255 ** 2 / mse)
        mask = d.convert("L").point(lambda v: 255 if v > args.tolerance else 0)
        share = mask.histogram()[255] / (a.size[0] * a.size[1])
        d.point(lambda v: min(255, v * 4)).save(diff_dir / n.replace("frame_", "diff_"))
        worst = min(worst, psnr)
        print(f"{n}: PSNR {psnr:.1f} dB, {100 * share:.2f}% of pixels differ by more than {args.tolerance}")
    ok = worst >= args.min_psnr
    print(f"{'OK' if ok else 'FAIL'}: {len(names)} frame(s), worst PSNR {worst:.1f} dB (minimum {args.min_psnr}); diffs in {diff_dir}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
