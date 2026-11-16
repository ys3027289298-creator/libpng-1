/* pngavif.h - AVIF still-image reader for the libpng codec/processing layer
 *
 * Copyright (c) 2026 libpng contributors.
 * This code is released under the libpng license (see LICENSE.md).
 *
 * This module decodes AVIF still images (AV1-in-HEIF, as produced for HDR
 * source material) and delivers rows in the exact 16-bit row format used by
 * png_read_row(): big-endian 16-bit components, RGB or RGBA, one row per
 * callback.  It lives entirely in the codec/processing layer:
 *
 *   - it does NOT modify any existing libpng source file or code path;
 *   - it does NOT change any public libpng interface signature;
 *   - it adds NO build-time dependency: the AV1 decoder (libdav1d) is
 *     loaded with dlopen() at run time and only if actually needed.
 *
 * Error reporting is 100% the PNG error system: every failure is reported
 * with png_error() and every remark with png_warning() on the png_struct
 * supplied by the caller, so the application's existing PNG error callback
 * receives AVIF failures exactly like PNG failures.  As with the rest of
 * libpng, the error callback must not return (it longjmp()s).
 */

#ifndef PNGAVIF_H
#define PNGAVIF_H

#include "png.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PNGAVIF_VERSION_STRING "0.1.0"

/* Hard resource limits (compile-time overridable).  Decoding is refused
 * with png_error() beyond these bounds; they cap the memory footprint.
 */
#ifndef PNGAVIF_MAX_PIXELS
#  define PNGAVIF_MAX_PIXELS 268435456u   /* 16384 x 16384 */
#endif
#ifndef PNGAVIF_MAX_FILE_BYTES
#  define PNGAVIF_MAX_FILE_BYTES 268435456u /* 256 MiB input file */
#endif

typedef struct png_avif_info
{
   png_uint_32 width;          /* pixels */
   png_uint_32 height;         /* pixels */
   int bit_depth;              /* source bit depth: 8 or 10 */
   int num_channels;           /* output channels: 3 (RGB) or 4 (RGBA) */
   int has_alpha;              /* 1 if an alpha auxiliary image was found */
   /* CICP (ISO/IEC 23001-8) colour description actually used: */
   int colour_primaries;       /* from colr box, 9 (BT.2020) if absent */
   int transfer_characteristics;
   int matrix_coefficients;    /* 0=identity/GBR 1=BT.709 5/6=BT.601 9=BT.2020 */
   int full_range;             /* 1 = full ("PC") range, 0 = studio range */
} png_avif_info;

/* Row callback, modelled on the libpng row callbacks.
 *
 * 'row' points to width*num_channels 16-bit components in big-endian byte
 * order (identical layout to png_read_row() output for 16-bit RGB/RGBA with
 * no transforms), valid only for the duration of the call.  'row_number' is
 * 0-based, top to bottom.
 */
typedef PNG_CALLBACK(void, *png_avif_row_ptr, (png_structp png_ptr,
    png_bytep row, png_uint_32 row_number, png_voidp user_data));

/* Decode the AVIF file 'file_name'.  On success returns 0 after delivering
 * 'info' and height row callbacks.  On any failure png_error() is called
 * (which, with a standard libpng error handler, does not return).
 *
 * png_ptr must be a read struct created by the application with
 * png_create_read_struct(); its error/warning callbacks and its memory
 * allocator are used for everything this module does.
 */
int png_avif_read_file(png_structp png_ptr, png_const_charp file_name,
    png_avif_info *info, png_avif_row_ptr row_fn, png_voidp user_data);

/* Same, decoding from a memory buffer. */
int png_avif_read_memory(png_structp png_ptr, png_const_bytep data,
    size_t data_size, png_avif_info *info, png_avif_row_ptr row_fn,
    png_voidp user_data);

/* ---- conversion primitives (also used by the self-test) ---- */

/* Bit-depth expansion to 16 bits, rounding rules:
 *
 *  8-bit:  out = v * 257            (i.e. (v<<8)|v, bit replication).
 *          Exact: v*257 == round(v*65535/255), error is exactly 0.
 *
 *  10-bit: out = (v*65535 + 511) / 1023   (round to nearest).
 *          65535*v/1023 is never exactly x.5, so there are no ties;
 *          error <= 0.5 ulp(16-bit) = 1/131070 of full scale.
 */
png_uint_16 pngavif_scale8(unsigned int v);
png_uint_16 pngavif_scale10(unsigned int v);

/* YUV -> RGB colour-space conversion, fixed point.
 *
 * Coefficients are Q16 (1.0 == 65536), computed from the CICP matrix
 * coefficients and the studio/full range flag in the *source* bit-depth
 * domain, so 8-bit and 10-bit sources each get their exact studio-swing
 * constants (16/128, 64/512 offsets).  Each output component is
 *
 *   out = clip((sum(coeff_q16[k] * (in[k] - offset[k])) + 32768) >> 16)
 *
 * i.e. one single round-to-nearest (half up) at the final shift.
 * Worst-case error versus the infinite-precision result is bounded by
 * coefficient quantization (<= 0.5 ulp per product term) plus the final
 * rounding (0.5 ulp): <= 1.5 ulp for R/B (2 terms), <= 2 ulp for G
 * (3 terms).  Measured values are printed by avif2png --selftest.
 */
typedef struct pngavif_csc
{
   png_int_32 cy;    /* luma scale, Q16 */
   png_int_32 crv;   /* V -> R, Q16 */
   png_int_32 cgu;   /* U -> G, Q16 */
   png_int_32 cgv;   /* V -> G, Q16 */
   png_int_32 cbu;   /* U -> B, Q16 */
   png_int_32 yoff;  /* luma offset, source units */
   png_int_32 coff;  /* chroma centre, source units */
   int identity;     /* matrix_coefficients == 0 (GBR): bypass matrix */
   int bit_depth;    /* 8 or 10 */
} pngavif_csc;

void pngavif_csc_init(pngavif_csc *csc, int matrix_coefficients,
    int full_range, int bit_depth);

/* y/u/v in source units (0..255 or 0..1023); rgb[] receives 16-bit values. */
void pngavif_csc_apply(const pngavif_csc *csc, unsigned int y,
    unsigned int u, unsigned int v, png_uint_16 rgb[3]);

#ifdef __cplusplus
}
#endif

#endif /* PNGAVIF_H */
