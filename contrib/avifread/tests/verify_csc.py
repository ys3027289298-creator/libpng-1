#!/usr/bin/env python3
# verify_csc.py - authoritative end-to-end check of the avifread pipeline.
#
# Decodes the AV1 bitstream to raw YUV planes with ffmpeg (libdav1d - the
# same decoder the module uses), recomputes the RGB conversion in floating
# point with the documented matrix and rounding rules, and compares against
# the PNG16 produced by avif2png.  Any difference beyond 1 ulp16 indicates
# a bug in the module (container parsing, plane handling, upsampling or
# fixed-point conversion), not a reference-decoder disagreement.
#
# usage: verify_csc.py file.avif ours.png {444|420} {709|2020} {8|10}

import math
import struct
import subprocess
import sys

W = H = None


def probe_size(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=p=0", path],
        check=True, capture_output=True, text=True)
    w, h = out.stdout.strip().split(",")
    return int(w), int(h)


def planes(path, layout, depth):
    fmt = {("444", 8): "yuv444p", ("420", 8): "yuv420p",
           ("444", 10): "yuv444p10le", ("420", 10): "yuv420p10le"}[(layout, depth)]
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
         "-pix_fmt", fmt, "-"], check=True, capture_output=True).stdout
    w, h = probe_size(path)
    cw, ch = (w, h) if layout == "444" else ((w + 1) // 2, (h + 1) // 2)
    bps = 1 if depth == 8 else 2
    ys, cs = w * h * bps, cw * ch * bps
    y = raw[:ys]
    u = raw[ys:ys + cs]
    v = raw[ys + cs:ys + 2 * cs]
    return (w, h, cw, ch, y, u, v, bps)


def get(plane, stride_w, x, y, bps):
    i = y * stride_w + x
    if bps == 1:
        return plane[i]
    return plane[2 * i] | (plane[2 * i + 1] << 8)


def main():
    avif, ours, layout, matrix, depth_s = sys.argv[1:6]
    depth = int(depth_s)
    w, h, cw, ch, Y, U, V, bps = planes(avif, layout, depth)

    ours_raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", ours, "-f", "rawvideo",
         "-pix_fmt", "rgb48le", "-"], check=True, capture_output=True).stdout

    kr, kb = {"709": (0.2126, 0.0722), "2020": (0.2627, 0.0593)}[matrix]
    kg = 1.0 - kr - kb
    maxval = 1023 if depth == 10 else 255
    yrng, crng = (876.0, 896.0) if depth == 10 else (219.0, 224.0)
    yoff = 64 if depth == 10 else 16
    coff = (maxval + 1) / 2.0

    max_diff = 0
    sse = 0
    n = 0
    for yy_ in range(h):
        cy = yy_ if layout == "444" else yy_ // 2
        for xx in range(w):
            cx = xx if layout == "444" else xx // 2
            yv = get(Y, w, xx, yy_, bps)
            uv = get(U, cw, cx, cy, bps)
            vv = get(V, cw, cx, cy, bps)
            fy = (yv - yoff) * 65535.0 / yrng
            fu = (uv - coff) * 65535.0 / crng
            fv = (vv - coff) * 65535.0 / crng
            ref = (fy + 2 * (1 - kr) * fv,
                   fy - kb * 2 * (1 - kb) / kg * fu - kr * 2 * (1 - kr) / kg * fv,
                   fy + 2 * (1 - kb) * fu)
            for k in range(3):
                r = min(65535.0, max(0.0, ref[k]))
                got = struct.unpack_from("<H", ours_raw,
                                         (yy_ * w + xx) * 6 + 2 * k)[0]
                d = abs(got - r)
                if d > max_diff:
                    max_diff = d
                sse += d * d
                n += 1
    psnr = 10 * math.log10(n * 65535.0 * 65535.0 / sse) if sse else float("inf")
    ok = max_diff <= 1.0
    print("verify %s (%s %s %d-bit): max|diff|=%.3f ulp16 PSNR=%.2f dB -> %s"
          % (avif, layout, matrix, depth, max_diff, psnr,
             "PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
