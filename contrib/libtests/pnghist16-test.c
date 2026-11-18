/* pnghist16-test.c
 *
 * Copyright (c) 2026 Cosmin Truta
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * Correctness tests and a timing benchmark for the 16-bit grayscale
 * histogram / auto-levels routines in pnghist16.c.
 *
 * Pass --bench to run the 4096x4096 performance benchmark; without it the
 * program only runs the correctness checks.  With --bench-limit=MS the
 * benchmark additionally fails (exit code 1) if the histogram pass takes
 * longer than MS milliseconds; the feature requirement is 20 ms.
 */

#define _POSIX_C_SOURCE 199309L /* clock_gettime */

#include "pnghist16.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;

static void
check(int condition, const char *what)
{
   if (!condition)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      ++failures;
   }
}

static double
elapsed_ms(const struct timespec *start, const struct timespec *end)
{
   double s = (double)(end->tv_sec - start->tv_sec);
   double ns = (double)(end->tv_nsec - start->tv_nsec);

   return s * 1000.0 + ns / 1.0e6;
}

/* Build a deterministic pseudo-random image without external dependencies. */
static uint16_t *
make_image(size_t count, uint16_t min, uint16_t max, uint64_t seed)
{
   uint16_t *img = malloc(count * sizeof(uint16_t));
   size_t i;

   if (img == NULL)
   {
      fprintf(stderr, "out of memory\n");
      exit(1);
   }

   for (i = 0; i < count; ++i)
   {
      /* xorshift64* */
      seed ^= seed >> 12;
      seed ^= seed << 25;
      seed ^= seed >> 27;
      img[i] = (uint16_t)(min +
         (uint16_t)((seed * 0x2545F4914F6CDD1DULL) % (max - min + 1u)));
   }

   return img;
}

static void
test_basic_counting(void)
{
   png_hist16 ctx;
   const uint64_t *h;
   uint16_t row[] = {0, 65535, 100, 100, 0, 100};

   png_hist16_init(&ctx);
   check(png_hist16_count_row(&ctx, row, 6, 1) == PNG_HIST16_OK,
      "count_row ok");
   check(png_hist16_get_total(&ctx) == 6, "total counts all samples");

   h = png_hist16_get_histogram(&ctx);
   check(h != NULL, "histogram readable");
   check(h[0] == 2 && h[100] == 3 && h[65535] == 1,
      "per-bucket counts");
   check(h[1] == 0, "untouched bucket is zero");
}

static void
test_big_endian_row(void)
{
   png_hist16 ctx;
   const uint64_t *h;
   /* big-endian packed samples: 0x0000, 0x1234, 0xFFFF */
   unsigned char row[] = {
      0x00, 0x00, 0x12, 0x34, 0xFF, 0xFF
   };

   png_hist16_init(&ctx);
   check(png_hist16_count_row_be(&ctx, row, 3, 2) == PNG_HIST16_OK,
      "count_row_be ok");
   h = png_hist16_get_histogram(&ctx);
   check(h[0x0000] == 1 && h[0x1234] == 1 && h[0xFFFF] == 1,
      "big-endian sample decode");
}

static void
test_strided_row(void)
{
   png_hist16 ctx;
   const uint64_t *h;
   /* gray+alpha pairs; only the gray channel (stride 2) is counted. */
   uint16_t row[] = {50, 0, 60, 0, 70, 0};

   png_hist16_init(&ctx);
   check(png_hist16_count_row(&ctx, row, 3, 2) == PNG_HIST16_OK,
      "strided count ok");
   h = png_hist16_get_histogram(&ctx);
   check(h[50] == 1 && h[60] == 1 && h[70] == 1,
      "stride skips interleaved samples");
}

static void
test_percentile_clip(void)
{
   png_hist16 ctx;
   const uint16_t *lut;
   uint16_t img[1000];
   size_t i;

   /* Uniform ramp 0..999 repeated, 1000 pixels total. */
   for (i = 0; i < 1000; ++i)
      img[i] = (uint16_t)i;

   png_hist16_init(&ctx);
   check(png_hist16_count_row(&ctx, img, 1000, 1) == PNG_HIST16_OK,
      "ramp counted");

   /* Clip 5 per mille (0.5%) at each end: 5 pixels below the low cut and
    * 5 pixels above the high cut.  The cut-point samples themselves are
    * retained and map to the output extremes.
    */
   check(png_hist16_auto_levels(&ctx, 5, 5) == PNG_HIST16_OK,
      "auto_levels symmetric ok");
   check(ctx.low == 5 && ctx.high == 994, "percentile cut points");

   lut = png_hist16_get_lut(&ctx);
   check(lut != NULL, "lut readable");
   check(lut[0] == 0 && lut[4] == 0, "clipped low tail maps to 0");
   check(lut[5] == 0, "low cut point maps to 0");
   check(lut[994] == 65535, "high cut point maps to 65535");
   check(lut[995] == 65535 && lut[65535] == 65535,
      "clipped high tail maps to 65535");
   check(png_hist16_map(&ctx, 5) == 0 &&
      png_hist16_map(&ctx, 994) == 65535, "map matches lut");

   /* Monotonic non-decreasing across the whole table. */
   {
      int monotonic = 1;
      for (i = 1; i < PNG_HIST16_SIZE; ++i)
         if (lut[i] < lut[i - 1])
            monotonic = 0;
      check(monotonic, "lut is monotonic");
   }
}

static void
test_asymmetric_clip(void)
{
   png_hist16 ctx;
   uint16_t img[1000];
   size_t i;

   for (i = 0; i < 1000; ++i)
      img[i] = (uint16_t)i;

   png_hist16_init(&ctx);
   png_hist16_count_row(&ctx, img, 1000, 1);

   /* 1% low, 10% high: retain ranks 11..900. */
   check(png_hist16_auto_levels(&ctx, 10, 100) == PNG_HIST16_OK,
      "asymmetric auto_levels ok");
   check(ctx.low == 10, "low cut at 1st percentile");
   check(ctx.high == 899, "high cut retains through the 90th percentile");
}

static void
test_no_clip_full_stretch(void)
{
   png_hist16 ctx;
   const uint16_t *lut;
   uint16_t img[256];
   size_t i;

   for (i = 0; i < 256; ++i)
      img[i] = (uint16_t)(i * 100 + 1000); /* 1000..26500 */

   png_hist16_init(&ctx);
   png_hist16_count_row(&ctx, img, 256, 1);
   check(png_hist16_auto_levels(&ctx, 0, 0) == PNG_HIST16_OK,
      "zero clip ok");
   check(ctx.low == 1000 && ctx.high == 26500,
      "zero clip uses observed extremes");

   lut = png_hist16_get_lut(&ctx);
   check(lut[1000] == 0 && lut[26500] == 65535,
      "full range stretch endpoints");
}

static void
test_errors(void)
{
   png_hist16 ctx;
   uint16_t one = 42;

   png_hist16_init(&ctx);

   check(png_hist16_auto_levels(&ctx, 0, 0) == PNG_HIST16_ERR_EMPTY,
      "empty histogram rejected");
   check(png_hist16_get_lut(&ctx) == NULL, "no lut before auto_levels");

   png_hist16_count_row(&ctx, &one, 1, 1);
   check(png_hist16_auto_levels(&ctx, 1001, 0) == PNG_HIST16_ERR_CLIP,
      "clip > 1000 rejected");

   /* A single distinct value makes low == high: overlap. */
   check(png_hist16_auto_levels(&ctx, 0, 0) == PNG_HIST16_ERR_CLIP,
      "degenerate range rejected");

   check(png_hist16_count_row(NULL, &one, 1, 1) == PNG_HIST16_ERR_ARG,
      "null ctx rejected");
   check(png_hist16_count_row(&ctx, NULL, 1, 1) == PNG_HIST16_ERR_ARG,
      "null row rejected");
   check(png_hist16_count_row(&ctx, &one, 1, 0) == PNG_HIST16_ERR_ARG,
      "zero stride rejected");

   /* Overflow-safe index arithmetic. */
   check(png_hist16_count_row(&ctx, &one, SIZE_MAX, 2) ==
      PNG_HIST16_ERR_ARG, "native width*stride overflow rejected");
   check(png_hist16_count_row_be(&ctx, (const unsigned char *)&one, SIZE_MAX,
         2) == PNG_HIST16_ERR_ARG,
      "big-endian width*stride overflow rejected");
}

static void
test_roundtrip_properties(void)
{
   png_hist16 ctx;
   uint16_t *img;
   size_t i, n = 100000;
   const uint16_t *lut;
   int reaches_extreme = 0;

   img = make_image(n, 2000, 60000, 0x123456789abcdef0ULL);
   png_hist16_init(&ctx);
   png_hist16_count_row(&ctx, img, n, 1);
   check(png_hist16_auto_levels(&ctx, 5, 5) == PNG_HIST16_OK,
      "random image auto_levels ok");

   lut = png_hist16_get_lut(&ctx);

   /* A stretch must be monotonic and span the full output range; applying
    * it again need not be a no-op because the clipped image has a different
    * histogram than the original.
    */
   for (i = 1; i < PNG_HIST16_SIZE; ++i)
      check(lut[i] >= lut[i - 1], "lut stays monotonic on random data");

   check(lut[0] == 0 && lut[PNG_HIST16_SIZE - 1] == 65535,
      "lut covers both output extremes");

   for (i = 0; i < n; ++i)
   {
      uint16_t mapped = lut[img[i]];
      if (mapped == 0 || mapped == 65535)
         reaches_extreme = 1;
   }
   check(reaches_extreme, "some pixels reach an output extreme");

   free(img);
}

static double
run_benchmark(void)
{
   const size_t width = 4096;
   const size_t height = 4096;
   const size_t count = width * height;
   uint16_t *img = make_image(count, 0, 65535, 0xdeadbeefcafe1234ULL);
   png_hist16 *ctx = calloc(1, sizeof(png_hist16));
   struct timespec t0, t1;
   size_t row;
   double hist_ms, level_ms, apply_ms;
   const uint64_t *h;
   uint64_t sum = 0;

   if (ctx == NULL)
   {
      fprintf(stderr, "out of memory\n");
      exit(1);
   }

   /* Histogram statistics: 4096 rows of 4096 packed samples. */
   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (row = 0; row < height; ++row)
      png_hist16_count_row(ctx, img + row * width, width, 1);
   clock_gettime(CLOCK_MONOTONIC, &t1);
   hist_ms = elapsed_ms(&t0, &t1);

   h = png_hist16_get_histogram(ctx);
   for (row = 0; row < PNG_HIST16_SIZE; ++row)
      sum += h[row];

   printf("bench 4096x4096 (%zu pixels):\n", count);
   printf("  histogram: %.2f ms (%.1f Mpix/s), total=%llu\n", hist_ms,
      (double)count / hist_ms / 1000.0,
      (unsigned long long)png_hist16_get_total(ctx));
   check(sum == count, "benchmark counts every pixel once");

   /* Percentile clip + LUT build. */
   clock_gettime(CLOCK_MONOTONIC, &t0);
   check(png_hist16_auto_levels(ctx, 5, 5) == PNG_HIST16_OK,
      "benchmark auto_levels ok");
   clock_gettime(CLOCK_MONOTONIC, &t1);
   level_ms = elapsed_ms(&t0, &t1);
   printf("  auto-levels (0.5%%/0.5%% clip, cut %u..%u): %.2f ms\n",
      ctx->low, ctx->high, level_ms);

   /* Applying the table to the whole image, as a real pipeline would. */
   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (row = 0; row < count; ++row)
      img[row] = png_hist16_map(ctx, img[row]);
   clock_gettime(CLOCK_MONOTONIC, &t1);
   apply_ms = elapsed_ms(&t0, &t1);
   printf("  apply lut: %.2f ms (%.1f Mpix/s)\n", apply_ms,
      (double)count / apply_ms / 1000.0);

   free(img);
   free(ctx);
   return hist_ms;
}

int
main(int argc, char **argv)
{
   int bench = 0;
   double bench_limit_ms = 0.0;
   double hist_ms;
   int i;

   for (i = 1; i < argc; ++i)
   {
      if (strcmp(argv[i], "--bench") == 0)
         bench = 1;
      else if (strncmp(argv[i], "--bench-limit=", 14) == 0)
      {
         bench = 1;
         bench_limit_ms = atof(argv[i] + 14);
         if (bench_limit_ms <= 0.0)
         {
            fprintf(stderr, "invalid --bench-limit: %s\n", argv[i] + 14);
            return 2;
         }
      }
      else if (strcmp(argv[i], "--bench-limit") == 0 && i + 1 < argc)
      {
         bench = 1;
         bench_limit_ms = atof(argv[++i]);
         if (bench_limit_ms <= 0.0)
         {
            fprintf(stderr, "invalid --bench-limit: %s\n", argv[i]);
            return 2;
         }
      }
      else
      {
         fprintf(stderr, "unknown option: %s\n", argv[i]);
         return 2;
      }
   }

   test_basic_counting();
   test_big_endian_row();
   test_strided_row();
   test_percentile_clip();
   test_asymmetric_clip();
   test_no_clip_full_stretch();
   test_errors();
   test_roundtrip_properties();

   if (bench)
   {
      hist_ms = run_benchmark();
      if (bench_limit_ms > 0.0 && hist_ms > bench_limit_ms)
      {
         fprintf(stderr, "FAIL: histogram %.2f ms exceeds limit %.2f ms\n",
            hist_ms, bench_limit_ms);
         ++failures;
      }
   }

   if (failures != 0)
   {
      fprintf(stderr, "%d test(s) failed\n", failures);
      return 1;
   }

   printf("all pnghist16 tests passed%s\n", bench ? " (with benchmark)" : "");
   return 0;
}
