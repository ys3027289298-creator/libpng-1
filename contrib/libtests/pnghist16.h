/* pnghist16.h
 *
 * Copyright (c) 2026 Cosmin Truta
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * 16-bit grayscale histogram and auto-levels (percentile-based contrast
 * stretch).  The histogram keeps one 64-bit counter per possible sample
 * value, so the full 0..65535 range is represented without binning.
 *
 * The typical sequence is:
 *
 *    png_hist16 ctx;
 *    png_hist16_init(&ctx);
 *    for (each row of a 16-bit grayscale image)
 *       png_hist16_count_row(&ctx, row, width, bytes_per_row);
 *    png_hist16_auto_levels(&ctx, clip_per_mille, clip_per_mille);
 *    for (each pixel)
 *       out = png_hist16_map(&ctx, in);
 *
 * The histogram counts and the stretch mapping table can both be read back
 * with png_hist16_get_histogram() and png_hist16_get_lut().
 */

#ifndef PNGHIST16_H
#define PNGHIST16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of histogram buckets: one for each possible 16-bit sample value. */
#define PNG_HIST16_SIZE 65536u

/* Return codes. */
#define PNG_HIST16_OK 0
#define PNG_HIST16_ERR_ARG 1
#define PNG_HIST16_ERR_EMPTY 2
#define PNG_HIST16_ERR_CLIP 3
#define PNG_HIST16_ERR_NO_LUT 4

typedef struct png_hist16
{
   /* 64-bit counters, so any image up to 2^64-1 pixels can be described. */
   uint64_t histogram[PNG_HIST16_SIZE];

   /* Stretch mapping table produced by png_hist16_auto_levels(). */
   uint16_t lut[PNG_HIST16_SIZE];

   /* Clip points found by png_hist16_auto_levels(), in input value units. */
   uint16_t low;
   uint16_t high;

   /* Number of pixels accumulated and clip ratios actually applied. */
   uint64_t total;
   unsigned int low_clip_per_mille;
   unsigned int high_clip_per_mille;

   /* Non-zero after png_hist16_auto_levels() has succeeded. */
   int have_lut;
} png_hist16;

/* Reset all state.  Always succeeds. */
void png_hist16_init(png_hist16 *ctx);

/* Accumulate one row of packed big-endian 16-bit samples (the PNG in-memory
 * representation).  stride is the byte offset between successive samples;
 * pass 2 for tightly packed rows.  Returns PNG_HIST16_OK or an error code.
 */
int png_hist16_count_row_be(png_hist16 *ctx, const unsigned char *row,
   size_t width, size_t stride);

/* Accumulate one row of native uint16_t samples.  stride is the element
 * offset between successive samples; pass 1 for tightly packed rows.
 */
int png_hist16_count_row(png_hist16 *ctx, const uint16_t *row,
   size_t width, size_t stride);

/* Build the auto-levels mapping table.  low_clip and high_clip are the
 * fractions of pixels to clip at each end, in parts per thousand (0..1000);
 * e.g. 5 means 0.5%.  The two ends are configurable independently.  The
 * clipping is percentile-based: exactly floor(total * clip/1000) samples at
 * each end fall outside the retained range.  The retained range spans the
 * sample at rank k+1 through the sample at rank total-k; those two cut
 * values map to 0 and 65535 respectively.
 */
int png_hist16_auto_levels(png_hist16 *ctx, unsigned int low_clip_per_mille,
   unsigned int high_clip_per_mille);

/* Map one sample through the stretch table.  Requires a built table. */
uint16_t png_hist16_map(const png_hist16 *ctx, uint16_t value);

/* Read-back accessors.  The returned pointers are valid until the next
 * mutating call on the context.  Returns NULL on a bad argument.
 */
const uint64_t *png_hist16_get_histogram(const png_hist16 *ctx);
const uint16_t *png_hist16_get_lut(const png_hist16 *ctx);
uint64_t png_hist16_get_total(const png_hist16 *ctx);

/* Human-readable description of an error code; never returns NULL. */
const char *png_hist16_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* PNGHIST16_H */
