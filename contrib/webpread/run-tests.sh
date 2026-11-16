#!/bin/sh
# run-tests.sh
#
# Copyright (c) 2026 libpng contributors
#
# This code is released under the libpng license.
# For conditions of distribution and use, see the disclaimer
# and license in png.h
#
# One-shot verification for the webpread WebP reading adapter:
# builds libpng and the adapter, generates the WebP samples, runs the
# adapter and the pre-adapter (direct) pipeline over every sample,
# checks row counts and pixel checksums, and runs the repository's own
# pngtest as a PNG regression check.
#
# Usage:  sh contrib/webpread/run-tests.sh
# Everything is written to artifacts/webp-adapter/ (removed on --clean).

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
WORK="$ROOT/artifacts/webp-adapter"
BUILD="$WORK/build"
BIN="$WORK/bin"
SAMPLES="$WORK/samples"
OUT="$WORK/out"
VENV="$ROOT/artifacts/webp-venv"

if [ "${1:-}" = "--clean" ]; then
   rm -rf "$WORK"
   echo "removed $WORK"
   exit 0
fi

mkdir -p "$BUILD" "$BIN" "$SAMPLES" "$OUT"

echo "== 1. building libpng (repo's own build) =="
if [ ! -f "$BUILD/libpng18.so" ] && [ ! -f "$BUILD/libpng18.a" ]; then
   cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
      -DPNG_TESTS=ON >"$OUT/cmake.log" 2>&1
   cmake --build "$BUILD" -j"$(nproc)" >"$OUT/build.log" 2>&1
fi
echo "   libpng build: $BUILD"

echo "== 2. building adapter, driver and sample generator =="
cc -O2 -Wall -Wextra -I"$ROOT" -I"$BUILD" \
   "$ROOT/contrib/webpread/webpread.c" \
   "$ROOT/contrib/webpread/webpreadtest.c" \
   -L"$BUILD" -lpng18 -lz -Wl,-rpath,"$BUILD" \
   -o "$BIN/webpreadtest"
cc -O2 -Wall -Wextra \
   "$ROOT/contrib/webpread/mkwebpsample.c" -lz -o "$BIN/mkwebpsample"
echo "   $BIN/webpreadtest, $BIN/mkwebpsample"

echo "== 3. generating samples =="
"$BIN/mkwebpsample" "$SAMPLES" >"$OUT/samples.log"
cat "$OUT/samples.log"

fail=0
check() { # check <description> <expected> <actual>
   if [ "$2" = "$3" ]; then
      echo "   PASS: $1"
   else
      echo "   FAIL: $1 (expected [$2], got [$3])"
      fail=1
   fi
}
field() { # field <name> <result-line>
   echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1
}

run() { # run <mode-args> <file> -> echoes the RESULT line
   "$BIN/webpreadtest" $1 "$2" | sed -n 's/^RESULT //p'
}

echo "== 4. adapter results (new pipeline) =="
: >"$OUT/adapter-results.txt"
for f in valid-literal.webp valid-lz77-cache.webp valid-vp8x.webp \
         corrupt-truncated.webp corrupt-sigflip.webp corrupt-bitflip.webp \
         unsupported-lossy.webp unsupported-transform.webp \
         unsupported-anim.webp garbage.bin empty.bin; do
   line=$(run "" "$SAMPLES/$f")
   echo "   RESULT $line"
   echo "$line" >>"$OUT/adapter-results.txt"
done

echo "== 5. pre-adapter results (old pipeline, --direct) =="
: >"$OUT/direct-results.txt"
for f in valid-literal.webp valid-lz77-cache.webp; do
   line=$(run "--direct" "$SAMPLES/$f")
   echo "   RESULT $line"
   echo "$line" >>"$OUT/direct-results.txt"
done

echo "== 6. checks =="

# 6.1 valid samples: status, row count and pixel checksum vs .expected
for f in valid-literal.webp valid-lz77-cache.webp valid-vp8x.webp; do
   line=$(grep "^file=$SAMPLES/$f " "$OUT/adapter-results.txt" || true)
   exp=$(cat "$SAMPLES/$f.expected")
   exp_rows=$(field rows "$exp")
   exp_crc=$(field crc32 "$exp")
   check "$f status" "OK" "$(field status "$line")"
   check "$f rows" "$exp_rows" "$(field rows "$line")"
   check "$f crc32" "$exp_crc" "$(field crc32 "$line")"
done

# 6.2 valid-vp8x must decode to the same pixels as valid-literal
l1=$(grep "^file=$SAMPLES/valid-literal.webp " "$OUT/adapter-results.txt")
lx=$(grep "^file=$SAMPLES/valid-vp8x.webp " "$OUT/adapter-results.txt")
check "vp8x == literal crc32" "$(field crc32 "$l1")" "$(field crc32 "$lx")"

# 6.3 streaming robustness: tiny pushes must give identical pixels
for f in valid-literal.webp valid-lz77-cache.webp; do
   big=$("$BIN/webpreadtest" --chunk 65536 "$SAMPLES/$f" | sed -n 's/^RESULT //p')
   small=$("$BIN/webpreadtest" --chunk 7 "$SAMPLES/$f" | sed -n 's/^RESULT //p')
   check "$f chunk7 crc32" "$(field crc32 "$big")" "$(field crc32 "$small")"
   check "$f chunk7 rows" "$(field rows "$big")" "$(field rows "$small")"
done

# 6.4 corrupt and unsupported inputs: explicit status codes
expect_status() { # expect_status <file> <status>
   line=$(grep "^file=$SAMPLES/$1 " "$OUT/adapter-results.txt")
   check "$1 -> $2" "$2" "$(field status "$line")"
}
expect_status corrupt-truncated.webp    TRUNCATED
expect_status corrupt-sigflip.webp      BAD_BITSTREAM
expect_status corrupt-bitflip.webp      BAD_BITSTREAM
expect_status unsupported-lossy.webp    UNSUPPORTED_COMPRESSION
expect_status unsupported-transform.webp UNSUPPORTED_TRANSFORM
expect_status unsupported-anim.webp     UNSUPPORTED_FEATURE
expect_status garbage.bin               PNG_ERROR
expect_status empty.bin                 INCOMPLETE

# 6.5 before/after: the old pipeline rejects WebP, the new one reads it
for f in valid-literal.webp valid-lz77-cache.webp; do
   before=$(grep "^file=$SAMPLES/$f " "$OUT/direct-results.txt")
   after=$(grep "^file=$SAMPLES/$f " "$OUT/adapter-results.txt")
   check "$f before(direct)" "PNG_ERROR" "$(field status "$before")"
   check "$f after(adapter)" "OK" "$(field status "$after")"
done

# 6.6 PNG parity: adapter passthrough == direct, byte-identical results
for png in "$ROOT/pngtest.png" "$ROOT/contrib/pngsuite/basn0g08.png"; do
   [ -f "$png" ] || continue
   via_adapter=$(run "" "$png")
   via_direct=$(run "--direct" "$png")
   check "$(basename "$png") parity crc32" \
      "$(field crc32 "$via_direct")" "$(field crc32 "$via_adapter")"
   check "$(basename "$png") parity rows" \
      "$(field rows "$via_direct")" "$(field rows "$via_adapter")"
   check "$(basename "$png") parity status" \
      "$(field status "$via_direct")" "$(field status "$via_adapter")"
done

# 6.7 repo's own test program as PNG regression
echo "== 7. pngtest (repo's own test) =="
if "$BUILD/pngtest" "$ROOT/pngtest.png" >"$OUT/pngtest.log" 2>&1; then
   echo "   PASS: pngtest pngtest.png"
else
   echo "   FAIL: pngtest pngtest.png (see $OUT/pngtest.log)"
   fail=1
fi

# 6.8 independent cross-check of the decoded pixels with Pillow/libwebp
echo "== 8. independent pixel cross-check (Pillow/libwebp, if present) =="
if [ -x "$VENV/bin/python" ]; then
   "$VENV/bin/python" -I - "$SAMPLES" >"$OUT/pil-check.log" 2>&1 <<'EOF' \
      && pil_ok=1 || pil_ok=0
import sys, zlib, pathlib
d = pathlib.Path(sys.argv[1])
ok = True
for exp in sorted(d.glob("valid-*.webp.expected")):
    want = dict(f.split("=", 1) for f in exp.read_text().split())
    from PIL import Image
    im = Image.open(exp.with_suffix("")).convert("RGBA")
    crc = "0x%08x" % zlib.crc32(im.tobytes())
    good = crc == want["crc32"] and im.height == int(want["rows"])
    ok &= good
    print("%s: libwebp crc32=%s rows=%d %s" %
          (exp.name, crc, im.height, "MATCH" if good else "MISMATCH"))
sys.exit(0 if ok else 1)
EOF
   cat "$OUT/pil-check.log"
   if [ "${pil_ok:-0}" = 1 ]; then
      echo "   PASS: Pillow/libwebp agrees with adapter checksums"
   else
      echo "   FAIL: Pillow/libwebp mismatch (see $OUT/pil-check.log)"
      fail=1
   fi
else
   echo "   SKIP: $VENV not found"
fi

# 6.9 memory: peak RSS of a full decode run
echo "== 9. memory =="
"$BIN/webpreadtest" "$SAMPLES/valid-literal.webp" \
   "$SAMPLES/valid-lz77-cache.webp" "$SAMPLES/valid-vp8x.webp" \
   "$ROOT/pngtest.png" >/dev/null
peak=$("$BIN/webpreadtest" "$SAMPLES/valid-literal.webp" \
   "$SAMPLES/valid-lz77-cache.webp" "$SAMPLES/valid-vp8x.webp" \
   "$ROOT/pngtest.png" | sed -n 's/^PEAK_RSS kb=//p')
echo "   peak RSS decoding all valid samples + pngtest.png: ${peak} KiB"

echo
if [ "$fail" = 0 ]; then
   echo "ALL CHECKS PASSED"
else
   echo "SOME CHECKS FAILED"
fi
exit "$fail"
