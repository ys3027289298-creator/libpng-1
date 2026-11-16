#!/bin/sh
# runtest.sh - one-shot verification of contrib/avifread.
#
#   1. builds avif2png against the libpng build in ../../build-base
#   2. runs the conversion self-test (rounding rules / error bounds)
#   3. decodes all bundled AVIF samples (8/10-bit, 420/444, alpha, HDR-PQ)
#   4. verifies the output numerically against a floating-point reference
#      computed from the decoded YUV planes (requires ffmpeg + python3)
#   5. reproduces every failure path through the PNG error callback
#   6. measures the memory high-water mark on a 1024x1024 10-bit image
#
# usage: sh runtest.sh     (from contrib/avifread)

set -e
cd "$(dirname "$0")"
AVIF2PNG=./avif2png
T=tests
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "=== 1. build ==="
make >/dev/null
echo "build OK"

echo
echo "=== 2. conversion self-test (documented rounding rules) ==="
$AVIF2PNG --selftest

echo
echo "=== 3. decode bundled samples to PNG16 ==="
for f in $T/avif8_444_709.avif $T/avif8_420_709.avif \
         $T/avif10_444_2020pq.avif $T/avif8_alpha.avif $T/avif10_alpha.avif
do
   out="$WORK/$(basename "$f" .avif).png"
   $AVIF2PNG "$f" "$out"
done

echo
echo "=== 4. numeric verification vs float reference (same YUV planes) ==="
python3 -I $T/verify_csc.py $T/avif8_444_709.avif    "$WORK/avif8_444_709.png"    444 709  8
python3 -I $T/verify_csc.py $T/avif8_420_709.avif    "$WORK/avif8_420_709.png"    420 709  8
python3 -I $T/verify_csc.py $T/avif10_444_2020pq.avif "$WORK/avif10_444_2020pq.png" 444 2020 10

echo
echo "=== 5. failure paths (all reported via the PNG error callback) ==="
expect_error()  # <label> <cmd...>: command must fail via the error callback
{
   label=$1; shift
   if out=$("$@" 2>&1); then
      echo "$label: UNEXPECTED SUCCESS"; exit 1
   fi
   echo "$out" | grep -m1 png-error-callback || {
      echo "$label: error NOT reported via callback"; exit 1; }
}
echo "--- 5a. non-AVIF input:"
expect_error 5a $AVIF2PNG ../../pngtest.png "$WORK/x.png"
echo "--- 5b. truncated file:"
head -c 200 $T/avif8_444_709.avif > "$WORK/trunc.avif"
expect_error 5b $AVIF2PNG "$WORK/trunc.avif" "$WORK/x.png"
echo "--- 5c. corrupt AV1 bitstream:"
cp $T/avif8_444_709.avif "$WORK/corrupt.avif"
python3 -I - "$WORK/corrupt.avif" <<'EOF'
import struct, sys
p = sys.argv[1]
d = bytearray(open(p, "rb").read())
off = 0
while off + 8 <= len(d):
    size, typ = struct.unpack_from(">I4s", d, off)
    if typ == b"mdat":
        for i in range(off + 8, off + 40):
            d[i] = 0xAA
        break
    off += size
open(p, "wb").write(bytes(d))
EOF
expect_error 5c $AVIF2PNG "$WORK/corrupt.avif" "$WORK/x.png"
echo "--- 5d. AV1 decoder missing (PNGAVIF_NO_DAV1D=1):"
expect_error 5d env PNGAVIF_NO_DAV1D=1 $AVIF2PNG $T/avif8_444_709.avif \
    "$WORK/x.png"

echo
echo "=== 6. memory high-water mark (1024x1024 10-bit HDR) ==="
ffmpeg -hide_banner -loglevel error -f lavfi -i \
    testsrc2=size=1024x1024:duration=1 -frames:v 1 -pix_fmt yuv420p10le \
    -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
    -c:v libaom-av1 -still-picture 1 -cpu-used 8 "$WORK/big10.avif" -y
$AVIF2PNG "$WORK/big10.avif" "$WORK/big10.png" | grep -E "avif:|mem:"

echo
echo "=== all checks passed ==="
