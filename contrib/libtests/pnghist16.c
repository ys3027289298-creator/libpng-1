/* pnghist16.c
 *
 * Copyright (c) 2026 Cosmin Truta
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * 16-bit grayscale histogram and percentile-based auto-levels.
 * See pnghist16.h for the interface description.
 */

#include "pnghist16.h"

#include <string.h>

void
png_hist16_init(png_hist16 *ctx)
{
   if (ctx != NULL)
      memset(ctx, 0, sizeof(*ctx));
}

int
png_hist16_count_row(png_hist16 *ctx, const uint16_t *row, size_t width,
   size_t stride)
{
   size_t i;

   if (ctx == NULL || (row == NULL && width != 0) || stride == 0)
      return PNG_HIST16_ERR_ARG;

   /* Reject sizes whose index arithmetic would overflow size_t. */
   if (width > (SIZE_MAX - 1) / stride)
      return PNG_HIST16_ERR_ARG;

   for (i = 0; i < width; ++i)
   {
      ++ctx->histogram[row[i * stride]];
      ++ctx->total;
   }

   return PNG_HIST16_OK;
}

int
png_hist16_count_row_be(png_hist16 *ctx, const unsigned char *row,
   size_t width, size_t stride)
{
   size_t i;

   if (ctx == NULL || (row == NULL && width != 0) || stride < 2)
      return PNG_HIST16_ERR_ARG;

   /* Reject sizes whose byte-offset arithmetic would overflow size_t. */
   if (width > SIZE_MAX / stride)
      return PNG_HIST16_ERR_ARG;

   for (i = 0; i < width; ++i)
   {
      const unsigned char *p = row + i * stride;
      uint16_t value = (uint16_t)(((unsigned int)p[0] << 8) | p[1]);

      ++ctx->histogram[value];
      ++ctx->total;
   }

   return PNG_HIST16_OK;
}

/* Find the percentile cut point.
 *
 * When strict is zero, return the smallest bucket whose cumulative count
 * reaches target (used for the high end: target is the 1-based rank of the
 * last retained sample).
 *
 * When strict is non-zero, return the smallest bucket whose cumulative count
 * exceeds target (used for the low end: the 'target' lowest samples are
 * discarded, so the cut point is the first bucket after them).  A target of
 * zero then yields the first non-empty bucket.
 */
static uint16_t
find_percentile(const png_hist16 *ctx, uint64_t target, int strict)
{
   unsigned int value;
   uint64_t cumulative = 0;

   for (value = 0; value < PNG_HIST16_SIZE; ++value)
   {
      cumulative += ctx->histogram[value];
      if (strict ? cumulative > target : cumulative >= target)
         return (uint16_t)value;
   }

   /* target <= total, so this is only reachable on an inconsistent ctx. */
   return (uint16_t)(PNG_HIST16_SIZE - 1);
}

int
png_hist16_auto_levels(png_hist16 *ctx, unsigned int low_clip_per_mille,
   unsigned int high_clip_per_mille)
{
   uint64_t low_target, high_target, span;
   unsigned int value;
   uint16_t low, high;

   if (ctx == NULL)
      return PNG_HIST16_ERR_ARG;

   if (low_clip_per_mille > 1000 || high_clip_per_mille > 1000)
      return PNG_HIST16_ERR_CLIP;

   if (ctx->total == 0)
      return PNG_HIST16_ERR_EMPTY;

   /* Percentile cut points.  The two clip ratios are independent so the
    * stretch can be asymmetric.  Let k be the number of samples clipped at
    * an end.  The low cut point is the first bucket after the k lowest
    * samples (strict cumulative > k); the high cut point is the bucket
    * containing the (total-k)-th sample (non-strict cumulative >=
    * total-k), so exactly k samples at each end fall outside the range.
    */
   low_target = (ctx->total * low_clip_per_mille) / 1000u;
   high_target = ctx->total -
      (ctx->total * high_clip_per_mille) / 1000u;

   low = find_percentile(ctx, low_target, 1);
   high = find_percentile(ctx, high_target, 0);

   /* This happens if the two clip fractions overlap (or nearly so), e.g.
    * 600 per mille at each end of a sparse image.
    */
   if (low >= high)
      return PNG_HIST16_ERR_CLIP;

   ctx->low = low;
   ctx->high = high;
   ctx->low_clip_per_mille = low_clip_per_mille;
   ctx->high_clip_per_mille = high_clip_per_mille;

   span = (uint64_t)high - (uint64_t)low;

   for (value = 0; value < PNG_HIST16_SIZE; ++value)
   {
      uint64_t mapped;

      if (value < low)
         mapped = 0;
      else if (value > high)
         mapped = 65535u;
      else
         /* Rounded nearest; span is at least 1 and the product fits in
          * 64 bits because value-low < span <= 65535.  At the cut points
          * themselves this yields exactly 0 and 65535, so exactly the
          * requested percentile counts are clipped.
          */
         mapped = ((uint64_t)(value - low) * 65535u + span / 2) / span;

      ctx->lut[value] = (uint16_t)mapped;
   }

   ctx->have_lut = 1;
   return PNG_HIST16_OK;
}

uint16_t
png_hist16_map(const png_hist16 *ctx, uint16_t value)
{
   if (ctx == NULL || !ctx->have_lut)
      return value; /* nothing to do without a built mapping table */

   return ctx->lut[value];
}

const uint64_t *
png_hist16_get_histogram(const png_hist16 *ctx)
{
   if (ctx == NULL)
      return NULL;

   return ctx->histogram;
}

const uint16_t *
png_hist16_get_lut(const png_hist16 *ctx)
{
   if (ctx == NULL || !ctx->have_lut)
      return NULL;

   return ctx->lut;
}

uint64_t
png_hist16_get_total(const png_hist16 *ctx)
{
   if (ctx == NULL)
      return 0;

   return ctx->total;
}

const char *
png_hist16_strerror(int err)
{
   switch (err)
   {
      case PNG_HIST16_OK:
         return "ok";
      case PNG_HIST16_ERR_ARG:
         return "invalid argument";
      case PNG_HIST16_ERR_EMPTY:
         return "no samples accumulated";
      case PNG_HIST16_ERR_CLIP:
         return "clip ratios overlap or exceed range";
      case PNG_HIST16_ERR_NO_LUT:
         return "mapping table not built";
      default:
         return "unknown error";
   }
}
