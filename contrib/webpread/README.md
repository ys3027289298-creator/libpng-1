# webpread — WebP reading adapter for the libpng row-callback pipeline

The "old pipeline" is a libpng progressive reader: the application
installs `info`/`row`/`end` callbacks with
`png_set_progressive_read_fn()` and feeds bytes to `png_process_data()`
(see `example.c`).  It only accepts PNG.  This directory adds a
codec-layer adapter that lets the same pipeline, unmodified, also read
WebP images: the application pushes bytes to `webp_adapt_push()`
instead of `png_process_data()` and keeps its callbacks, its
`png_progressive_combine_row()` usage and its error handling exactly
as they are.

```
app bytes ──► webp_adapt_push() ──┬─ PNG sig ─► png_process_data() (passthrough)
                                  └─ RIFF/WEBP ─► VP8L decode ─► PNG re-encode
                                                  ─► png_process_data()
```

## Guarantees and limits

* **No new dependencies.**  The adapter uses only libpng itself and
  zlib, which libpng already requires.  It does *not* use libwebp.
* **No public API changes.**  `png.h` and the library sources are
  untouched (the adapter lives entirely in `contrib/webpread/`); the
  adapter's own interface in `webpread.h` is additive and private to
  the application.
* **The PNG state machine is never disturbed.**  PNG input is forwarded
  byte-for-byte.  For WebP input, `png_process_data()` is only ever
  called with a complete, valid PNG datastream synthesized *after* the
  whole WebP image decoded cleanly — a corrupt WebP stream causes zero
  bytes to be fed to the PNG state machine.
* **Row-callback semantics are preserved.**  Rows are delivered through
  the application's own `row` callback as 8-bit RGBA, non-interlaced,
  one callback per row, combined with `png_progressive_combine_row()`
  as usual.

## Supported WebP subset (lossless VP8L, RFC 9649)

Supported: RIFF/WEBP container (incl. VP8X wrapper without animation),
VP8L header, simple and normal canonical prefix codes, LZ77 backward
references (incl. the 2-D distance mapping), color cache, and the
subtract-green transform.

Anything else is rejected with an **explicit status code**
(`enum webp_adapt_status` in `webpread.h`), never a crash or a silent
failure:

| status | meaning |
|---|---|
| `TRUNCATED` | stream ended mid-structure (byte counts in the message) |
| `BAD_CONTAINER` | RIFF/chunk layout invalid (sizes, duplicates, no image chunk) |
| `BAD_BITSTREAM` | VP8L entropy data invalid (bad signature, incomplete prefix code, LZ77 overrun, …) |
| `UNSUPPORTED_COMPRESSION` | lossy VP8 bitstream |
| `UNSUPPORTED_TRANSFORM` | predictor / color / color-indexing transform |
| `UNSUPPORTED_FEATURE` | animation, meta prefix codes, nonzero VP8L version |
| `SIZE_LIMIT` | exceeds `WEBP_ADAPT_MAX_INPUT` / `WEBP_ADAPT_MAX_PIXELS` |
| `OOM` / `INTERNAL` | allocation failure / invariant violation |

Corrupt input is reported at the exact layer that detects it:
container-level truncation gives `TRUNCATED`, bit-level corruption
inside a complete VP8L payload gives `BAD_BITSTREAM`.  The detail
string (`webp_adapt_message()`) names the failing structure and
offset.  Robustness is fuzz-verified (all truncations plus random
corruption, under ASan/UBSan): every case terminates promptly with a
clean status code.

## Memory upper bound

Peak adapter heap, regardless of image content:

```
VP8L payload      ≤ WEBP_ADAPT_MAX_INPUT   (default 64 MiB, SIZE_LIMIT above)
pixel buffer      = 4 · W · H bytes,  W·H ≤ WEBP_ADAPT_MAX_PIXELS
                    (default 2^24 px → ≤ 64 MiB, SIZE_LIMIT above)
prefix-code tables ≤ 5 · 2328 · 2 B    ≈ 23 KiB
color cache        ≤ 2^11 · 4 B         = 8 KiB
row + IDAT staging ≤ 4·W + 1 + 32 KiB   ≤ 97 KiB
zlib deflate state                      ≈ 260 KiB
```

So with the default caps the adapter stays below ≈ **129 MiB** in the
worst case, and ≈ `input_size + 4·W·H + 0.4 MiB` for any concrete
image.  Both caps are compile-time macros (`webpread.h`).  Measured:
2.7 MiB peak RSS for the whole sample suite; 37.7 MiB total RSS for a
2000×2000 image (of which the adapter share is ≈21 MiB: 5 MiB input +
16 MiB pixels; the remaining ≈16 MiB is the test driver's own RGBA
canvas, i.e. application-side memory any reader of a 2000×2000 image
needs anyway).

## Files

* `webpread.h`, `webpread.c` — the adapter (container dispatch, VP8L
  decoder, PNG re-encoder).
* `webpreadtest.c` — test driver: a progressive reader in the
  `example.c` pattern; prints `RESULT` lines with row counts and RGBA
  CRC32.  `--direct` bypasses the adapter (old-pipeline behavior).
* `mkwebpsample.c` — generates the small test samples (valid,
  corrupt, unsupported) plus `.expected` files with row counts and
  CRC32s.
* `run-tests.sh` — one-shot build + generate + verify; everything
  lands in `artifacts/webp-adapter/`.

## Running

```sh
sh contrib/webpread/run-tests.sh          # full suite, exits nonzero on failure
sh contrib/webpread/run-tests.sh --clean  # remove artifacts/webp-adapter
```

The suite checks: valid decodes against generator-computed checksums
and (if `artifacts/webp-venv` with Pillow is present) against real
libwebp; streaming robustness (7-byte pushes); explicit status codes
for every corrupt/unsupported sample; old-vs-new pipeline behavior on
WebP input; byte-identical PNG results with and without the adapter;
and the repository's own `pngtest` as a PNG regression.
