#!/usr/bin/env python3
# compare_png16.py - compare two 16-bit PNGs pixel-exactly.
#
# Both files are converted to raw 16-bit RGB(A) with ffmpeg and compared
# component-wise.  Prints max abs diff (in 16-bit ulps), mean abs diff and
# PSNR.  Exit status 0 if max diff <= tolerance (default 0).
#
# usage: compare_png16.py a.png b.png [tolerance]

import subprocess
import sys


def raw(path, pix_fmt):
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
         "-pix_fmt", pix_fmt, "-"],
        check=True, capture_output=True)
    return out.stdout


def main():
    a_path, b_path = sys.argv[1], sys.argv[2]
    tol = int(sys.argv[3]) if len(sys.argv) > 3 else 0

    fmt = "rgba64le"
    a = raw(a_path, fmt)
    b = raw(b_path, fmt)
    if len(a) != len(b):
        # try plain RGB (one of them has no alpha)
        fmt = "rgb48le"
        a = raw(a_path, fmt)
        b = raw(b_path, fmt)
    if len(a) != len(b):
        print("FAIL: size mismatch (%d vs %d bytes)" % (len(a), len(b)))
        return 1

    n = len(a) // 2
    va = memoryview(a).cast("H")
    vb = memoryview(b).cast("H")
    max_diff = 0
    total = 0
    sse = 0
    for i in range(n):
        d = abs(va[i] - vb[i])
        if d > max_diff:
            max_diff = d
        total += d
        sse += d * d
    mean = total / n
    import math
    psnr = 10 * math.log10(n * 65535.0 * 65535.0 / sse) if sse else float("inf")
    print("compare(%s): %s vs %s" % (fmt, a_path, b_path))
    print("  components=%d max|diff|=%d ulp16 mean|diff|=%.4f PSNR=%.2f dB "
          "tolerance=%d -> %s" %
          (n, max_diff, mean, psnr, tol,
           "PASS" if max_diff <= tol else "FAIL"))
    return 0 if max_diff <= tol else 1


if __name__ == "__main__":
    sys.exit(main())
