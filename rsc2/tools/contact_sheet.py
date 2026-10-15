#!/usr/bin/env python3
"""Tile the last N frame dumps (RECOMP_FB_DUMP BMPs) into one PNG.
Usage: contact_sheet.py <dir> <out.png> [N=8] [cols=4]   (needs ImageMagick)"""
import glob, os, subprocess, sys
d, out = sys.argv[1], sys.argv[2]
n = int(sys.argv[3]) if len(sys.argv) > 3 else 8
cols = int(sys.argv[4]) if len(sys.argv) > 4 else 4
frames = sorted(glob.glob(os.path.join(d, "*.bmp")))[-n:]
if not frames:
    sys.exit("no frames")
subprocess.run(["magick", "montage", *frames, "-tile", f"{cols}x", "-geometry",
                "320x240+2+2", "-background", "black", out], check=True)
print(out, len(frames), "frames:", os.path.basename(frames[0]), "..", os.path.basename(frames[-1]))
