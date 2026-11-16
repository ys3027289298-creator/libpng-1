# contrib/avifread — AVIF still-image reader for the libpng processing layer

Reads AVIF still images (AV1-in-HEIF — the format HDR source material is
commonly delivered in) and hands rows to the application in libpng's
existing 16-bit row format.

## Design constraints and how they are met

| Constraint | Solution |
|---|---|
| PNG path unchanged | 100% new files under `contrib/avifread/`; `git diff` on tracked files is empty; the full ctest suite (38/38) passes identically before and after. |
| No public interface signature changes | `png.h`/`pngconf.h` untouched. The module has its own header (`pngavif.h`) and is not part of the library ABI. |
| No new dependencies | The ISOBMFF/AVIF container is parsed in pure C. The AV1 bitstream is decoded by `libdav1d`, loaded with `dlopen()` **at run time only** — no build-time or link-time dependency, no new package. If the decoder is absent, the read fails cleanly through `png_error()`. |
| PNG-compatible error system | Every failure is reported with `png_error()`, every remark with `png_warning()`, on the caller's `png_structp` — the application's existing PNG error callback receives AVIF failures exactly like PNG failures (same longjmp contract). Allocations go through `png_malloc()`. |
| Changes only in the codec/processing layer | The module is a container parser + decoder glue + pixel-format converter. No build-system, API, or PNG-codec changes. |

## Row format

Identical to `png_read_row()` output for 16-bit RGB/RGBA with no
transforms: big-endian 16-bit components, `width * num_channels`
components per row, one callback per row, top to bottom. Rows can be
passed straight to `png_write_row()` (no `png_set_swap()`).

## Bit-depth conversion: rounding rules and error bounds

All errors are stated in ulp16 (1/65535 of full scale).

| Conversion | Rule | Error |
|---|---|---|
| 8-bit → 16-bit | `out = v*257` (bit replication `(v<<8)|v`) | **exactly 0** (v*257 == round(v·65535/255)) |
| 10-bit → 16-bit | `out = (v·65535 + 511) / 1023` (round to nearest; no ties are possible) | **≤ 0.5 ulp16** (measured max 0.4985, exhaustive over all 1024 values) |
| YUV→RGB matrix | Q16 fixed-point coefficients in the *source* bit-depth domain (exact 16/128 or 64/512 studio offsets), int64 accumulation, single round-half-up at the final shift: `out = clip((Σ cᵢ·(inᵢ−offᵢ) + 32768) >> 16)` | ≤ 1.5 ulp16 (R, B), ≤ 2 ulp16 (G) theoretical; **measured ≤ 0.503 ulp16** over a dense sweep of all matrices × ranges × depths |
| Alpha | luma of the alpha auxiliary image, scaled as above (no matrix, no range rescale — AVIF alpha is used as coded) | 0 (8-bit) / ≤ 0.5 ulp16 (10-bit); **measured 0** on the bundled samples |

Supported matrices (CICP `matrix_coefficients`): 0 (identity/GBR), 1
(BT.709), 4 (FCC), 5/6 (BT.601), 7 (SMPTE 240M), 9/10 (BT.2020). Absent
or unspecified (2) colr → BT.709 studio range (a `png_warning` is issued
only when the colr box is missing entirely; truly unsupported matrices
also warn and fall back to BT.709). Studio vs full range honours the colr
`full_range_flag`. 4:2:0/4:2:2 chroma is upsampled nearest-neighbour
(documented processing-layer choice; 4:4:4 and grayscale need none).

`avif2png --selftest` reproduces the measured column of the table.

## Memory upper bound

For a `w`×`h` image (`w·h` ≤ `PNGAVIF_MAX_PIXELS`, default 2²⁸) with
`B` bytes per source sample (1 for 8-bit, 2 for 10-bit):

```
M ≤ file_size                      (whole input, ≤ 256 MiB cap)
  + item_bytes                     (contiguous AV1 item copy, ≤ file_size, ×2 with alpha)
  + 2 · ⌈w·h·1.5⌉ · B              (dav1d frame buffers: n_threads=1, max_frame_delay=1)
  + w·h · B                        (alpha plane, if present)
  + w · channels · 2               (one output row, ≤ 8·w)
  + ~1 MiB                         (decoder tables, libpng, box metadata)
```

Measured (process peak RSS, including the PNG writer and all libraries):

| Image | Peak RSS | Formula estimate |
|---|---|---|
| 64×48 8-bit RGBA | 4.1 MiB | ~1.1 MiB fixed-dominated |
| 1024×1024 10-bit RGB (BT.2020/PQ) | 7.5 MiB | 16 KiB file + 6.0 MiB frames + 6 KiB row + ~1.2 MiB fixed ≈ 7.3 MiB |

## Failure paths (reproduction)

All of the following end in the application's PNG error callback
(`[png-error-callback]` lines below are printed by the app's callback in
`avif2png.c`) and a non-zero exit; `sh runtest.sh` replays them:

```sh
# 1. non-AVIF input
./avif2png ../../pngtest.png out.png
#   [png-error-callback] AVIF: invalid box size        -> exit 1

# 2. truncated file
head -c 200 tests/avif8_444_709.avif > trunc.avif
./avif2png trunc.avif out.png
#   [png-error-callback] AVIF: invalid box size        -> exit 1

# 3. corrupt AV1 bitstream (sequence header overwritten)
#    (see runtest.sh step 5c for the exact byte patch)
./avif2png corrupt.avif out.png
#   [png-error-callback] AVIF: AV1 bitstream rejected by decoder -> exit 1

# 4. AV1 decoder missing
PNGAVIF_NO_DAV1D=1 ./avif2png tests/avif8_444_709.avif out.png
#   [png-error-callback] AVIF: AV1 decoder unavailable ... -> exit 1
```

## Usage

```sh
make                      # needs a libpng build in ../../build-base
./avif2png in.avif out.png    # AVIF -> 16-bit PNG
./avif2png --selftest         # rounding-rule / error-bound measurements
sh runtest.sh                 # full one-shot verification
```

Environment: `PNGAVIF_DAV1D=/path/to/libdav1d.so` overrides decoder
discovery; `PNGAVIF_NO_DAV1D=1` simulates a missing decoder.

In your own code:

```c
png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
    NULL, my_error_fn, my_warning_fn);   /* your usual PNG callbacks */
if (setjmp(png_jmpbuf(png_ptr)))
   ...                                   /* AVIF *and* PNG errors land here */
png_avif_read_file(png_ptr, "in.avif", &info, my_row_fn, my_ctx);
```

## Verification summary (this tree)

* Baseline (`git stash`-equivalent: module absent): `pngtest` on an AVIF
  file → `libpng error: Not a PNG file`; no AVIF capability exists.
* After: ctest **38/38 passed, identical test set before/after**;
  `git diff` on tracked files is empty.
* End-to-end numeric check (`tests/verify_csc.py`): recompute the
  conversion in floating point from the decoded YUV planes and compare
  with the module's PNG16 output — max diff **≤ 0.501 ulp16** on
  4:4:4/4:2:0, 8/10-bit, BT.709/BT.2020 samples (pure output rounding).
* Alpha: bit-exact (0 ulp16) vs source luma on 8-bit and 10-bit samples.

## Limitations (documented, by design)

* Still images only: primary item + one alpha auxiliary item. Grid
  images, image sequences/animations and thumbnails are rejected
  (`AVIF: grid images are not supported`, ...).
* The alpha item is located via its `auxC` property; `iref` boxes are
  not walked (sufficient for single-image files).
* 12-bit sources are rejected (`AVIF: unsupported bit depth`).
* ICC colour profiles (`colr rICC/prof`) are not interpreted.
* Chroma upsampling for 4:2:0/4:2:2 is nearest-neighbour.
* Film-grain synthesis is disabled (`apply_grain=0`) for deterministic,
  bit-exact output.

## Files

* `pngavif.h`, `pngavif.c` — the module (drop-in, no libpng changes)
* `avif2png.c` — test driver / usage example + `--selftest`
* `runtest.sh` — one-shot verification (build, selftest, decode,
  numeric verification, failure paths, memory)
* `tests/` — self-made sample AVIFs (≤ 1.2 KB each), the alpha-sample
  remuxer (`make_alpha_avif.py`), and the numeric checkers
  (`verify_csc.py`, `compare_png16.py`)
