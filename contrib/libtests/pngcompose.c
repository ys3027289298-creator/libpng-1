/* pngcompose.c
 *
 * Copyright (c) 2026 Cosmin Truta
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * Test write-side background compositing (png_set_write_background):
 * fixed-point alpha compositing of RGBA/GA rows over a solid background
 * color, at 8-bit and 16-bit depth, with and without preservation of
 * the alpha channel.
 *
 * The composited output of four groups of test images (semi-transparent
 * edges, fully transparent, fully opaque, and 16-bit) is compared, pixel
 * by pixel, against a floating-point (double precision) reference.  The
 * performance requirement is that compositing a 4096x4096 image takes
 * less than 40 ms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#if defined(HAVE_CONFIG_H) && !defined(PNG_NO_CONFIG_H)
#  include <config.h>
#endif

#ifdef PNG_FREESTANDING_TESTS
#  include <png.h>
#else
#  include "../../png.h"
#endif

#ifdef PNG_WRITE_BACKGROUND_SUPPORTED

/* ---------------- memory IO ---------------- */

typedef struct
{
   png_bytep data;
   size_t    size;
   size_t    capacity;
} mem_writer;

static void PNGAPI
mem_write_fn(png_structp png_ptr, png_bytep data, size_t length)
{
   mem_writer *mw = (mem_writer *)png_get_io_ptr(png_ptr);

   if (mw->size + length > mw->capacity)
   {
      size_t new_capacity = (mw->capacity != 0) ? mw->capacity : 65536;
      png_bytep new_data;

      while (new_capacity < mw->size + length)
         new_capacity *= 2;

      new_data = (png_bytep)realloc(mw->data, new_capacity);
      if (new_data == NULL)
         png_error(png_ptr, "pngcompose: out of memory");

      mw->data = new_data;
      mw->capacity = new_capacity;
   }

   memcpy(mw->data + mw->size, data, length);
   mw->size += length;
}

static void PNGAPI
mem_flush_fn(png_structp png_ptr)
{
   (void)png_ptr;
}

typedef struct
{
   png_const_bytep data;
   size_t          size;
   size_t          offset;
} mem_reader;

static void PNGAPI
mem_read_fn(png_structp png_ptr, png_bytep data, size_t length)
{
   mem_reader *mr = (mem_reader *)png_get_io_ptr(png_ptr);

   if (mr->offset + length > mr->size)
      png_error(png_ptr, "pngcompose: read past end of memory buffer");

   memcpy(data, mr->data + mr->offset, length);
   mr->offset += length;
}

/* ---------------- test image generation ---------------- */

/* Deterministic pseudo-random channel value. */
static unsigned int
channel_pattern(png_uint_32 x, png_uint_32 y, unsigned int c,
    unsigned int max)
{
   png_uint_32 v = (x * 37 + y * 101 + c * 57) * 2654435761U;

   if (max == 255)
      return (v >> 16) & 0xff;

   return (v ^ (v >> 16)) & 0xffff;
}

/* The four test image groups. */
enum
{
   GROUP_SEMI,        /* semi-transparent edges: alpha sweeps all values */
   GROUP_TRANSPARENT, /* fully transparent, non-black color channels */
   GROUP_OPAQUE,      /* fully opaque */
   GROUP_SEMI16       /* 16-bit, alpha sweeps all values */
};

/* Fill one RGBA pixel (8 or 16-bit, big-endian) of a test image. */
static void
make_pixel(int group, png_uint_32 x, png_uint_32 y, int bit_depth,
    png_byte *px)
{
   if (bit_depth == 8) /* RGBA, 8-bit */
   {
      png_uint_32 a;

      px[0] = (png_byte)channel_pattern(x, y, 0, 255);
      px[1] = (png_byte)channel_pattern(x, y, 1, 255);
      px[2] = (png_byte)channel_pattern(x, y, 2, 255);

      switch (group)
      {
         case GROUP_TRANSPARENT:
            a = 0;
            break;
         case GROUP_OPAQUE:
            a = 255;
            break;
         default: /* GROUP_SEMI */
            a = (x + y * 3) & 0xff;
            /* Make sure the critical edge alpha values are present. */
            if (y == 0 && x < 6)
            {
               static const png_byte edge_a[6] = {0, 1, 127, 128, 254, 255};
               a = edge_a[x];
            }
            break;
      }

      px[3] = (png_byte)a;
   }

   else /* RGBA, 16-bit, big-endian */
   {
      png_uint_32 a;
      unsigned int r = channel_pattern(x, y, 0, 65535);
      unsigned int g = channel_pattern(x, y, 1, 65535);
      unsigned int b = channel_pattern(x, y, 2, 65535);

      px[0] = (png_byte)(r >> 8); px[1] = (png_byte)(r & 0xff);
      px[2] = (png_byte)(g >> 8); px[3] = (png_byte)(g & 0xff);
      px[4] = (png_byte)(b >> 8); px[5] = (png_byte)(b & 0xff);

      a = (x * 8191 + y * 3) & 0xffff;
      /* Make sure the critical edge alpha values are present. */
      if (y == 0 && x < 6)
      {
         static const png_uint_16 edge_a[6] =
            {0, 1, 32767, 32768, 65534, 65535};
         a = edge_a[x];
      }

      px[6] = (png_byte)(a >> 8); px[7] = (png_byte)(a & 0xff);
   }
}

/* ---------------- reference compositing ---------------- */

/* High-precision reference: evaluate the compositing formula in double
 * precision and round to nearest.  All intermediate values are exactly
 * representable in a double, so this is the exact mathematical result of
 * (foreground*alpha + background*(max-alpha) + max/2) / max.
 */
static unsigned int
ref_compose(unsigned int fg, unsigned int bg, unsigned int alpha,
    unsigned int max)
{
   double n = (double)fg * alpha + (double)bg * (max - alpha) + max / 2.0;

   return (unsigned int)floor(n / max);
}

/* ---------------- round-trip test ---------------- */

/* Write the image with png_set_write_background, read it back, and
 * compare every pixel against the reference.  Returns the number of
 * mismatched samples.
 */
static int
run_roundtrip(const char *name, int bit_depth, int keep_alpha,
    const png_color_16 *background, png_bytep pixels,
    png_uint_32 width, png_uint_32 height)
{
   mem_writer mw;
   mem_reader mr;
   png_structp png_ptr = NULL;
   png_infop info_ptr = NULL;
   png_bytep *rows = NULL;
   png_bytep result = NULL;
   png_uint_32 out_width, out_height;
   size_t row_bytes, out_row_bytes;
   int out_color_type, out_channels, errors = 0;
   int reading = 0;
   unsigned int max = (bit_depth == 16) ? 65535U : 255U;
   png_uint_32 x, y;
   int c;

   /* The test images are RGBA; the output loses the alpha channel when
    * keep_alpha is zero.
    */
   out_color_type = (keep_alpha != 0) ?
       PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB;
   out_channels = (keep_alpha != 0) ? 4 : 3;
   row_bytes = (size_t)width * 4 * (bit_depth / 8);
   out_row_bytes = (size_t)width * out_channels * (bit_depth / 8);

   memset(&mw, 0, sizeof mw);

   /* ---- write ---- */

   png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
       NULL, NULL, NULL);
   if (png_ptr == NULL)
   {
      fprintf(stderr, "pngcompose: %s: png_create_write_struct failed\n",
          name);
      errors = 1;
      goto cleanup;
   }

   info_ptr = png_create_info_struct(png_ptr);
   if (info_ptr == NULL)
   {
      fprintf(stderr, "pngcompose: %s: png_create_info_struct failed\n",
          name);
      errors = 1;
      goto cleanup;
   }

   if (setjmp(png_jmpbuf(png_ptr)))
   {
      fprintf(stderr, "pngcompose: %s: libpng error during write\n", name);
      errors = 1;
      goto cleanup;
   }

   png_set_write_fn(png_ptr, &mw, mem_write_fn, mem_flush_fn);

   png_set_IHDR(png_ptr, info_ptr, width, height, bit_depth, out_color_type,
       PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

   png_write_info(png_ptr, info_ptr);

   png_set_write_background(png_ptr, background, keep_alpha);

   rows = (png_bytep *)malloc(height * sizeof(png_bytep));
   if (rows == NULL)
   {
      fprintf(stderr, "pngcompose: %s: out of memory\n", name);
      errors = 1;
      goto cleanup;
   }
   for (y = 0; y < height; y++)
      rows[y] = pixels + (size_t)y * row_bytes;

   png_write_image(png_ptr, rows);
   png_write_end(png_ptr, info_ptr);

   png_destroy_write_struct(&png_ptr, &info_ptr);
   free(rows);
   rows = NULL;

   /* ---- read back ---- */

   mr.data = mw.data;
   mr.size = mw.size;
   mr.offset = 0;

   png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
       NULL, NULL, NULL);
   if (png_ptr == NULL)
   {
      fprintf(stderr, "pngcompose: %s: png_create_read_struct failed\n",
          name);
      errors = 1;
      goto cleanup;
   }
   reading = 1;

   info_ptr = png_create_info_struct(png_ptr);
   if (info_ptr == NULL)
   {
      fprintf(stderr, "pngcompose: %s: png_create_info_struct failed\n",
          name);
      errors = 1;
      goto cleanup;
   }

   if (setjmp(png_jmpbuf(png_ptr)))
   {
      fprintf(stderr, "pngcompose: %s: libpng error during read\n", name);
      errors = 1;
      goto cleanup;
   }

   png_set_read_fn(png_ptr, &mr, mem_read_fn);

   png_read_info(png_ptr, info_ptr);

   out_width = png_get_image_width(png_ptr, info_ptr);
   out_height = png_get_image_height(png_ptr, info_ptr);

   if (out_width != width || out_height != height ||
       png_get_bit_depth(png_ptr, info_ptr) != bit_depth ||
       png_get_color_type(png_ptr, info_ptr) != out_color_type ||
       png_get_channels(png_ptr, info_ptr) != out_channels)
   {
      fprintf(stderr, "pngcompose: %s: unexpected output format"
          " (%ux%u, depth %d, color type %d)\n",
          name, out_width, out_height,
          png_get_bit_depth(png_ptr, info_ptr),
          png_get_color_type(png_ptr, info_ptr));
      errors = 1;
      goto cleanup;
   }

   result = (png_bytep)malloc(out_row_bytes * height);
   rows = (png_bytep *)malloc(height * sizeof(png_bytep));
   if (result == NULL || rows == NULL)
   {
      fprintf(stderr, "pngcompose: %s: out of memory\n", name);
      errors = 1;
      goto cleanup;
   }
   for (y = 0; y < height; y++)
      rows[y] = result + (size_t)y * out_row_bytes;

   png_read_image(png_ptr, rows);
   png_read_end(png_ptr, info_ptr);

   /* ---- compare against the reference ---- */

   for (y = 0; y < height; y++)
   {
      const png_byte *src = pixels + (size_t)y * row_bytes;
      const png_byte *out = result + (size_t)y * out_row_bytes;

      for (x = 0; x < width; x++)
      {
         unsigned int fg[4], a, expected, actual;

         if (bit_depth == 8)
         {
            fg[0] = src[0]; fg[1] = src[1]; fg[2] = src[2];
            a = src[3];
            src += 4;
         }
         else
         {
            fg[0] = ((unsigned int)src[0] << 8) | src[1];
            fg[1] = ((unsigned int)src[2] << 8) | src[3];
            fg[2] = ((unsigned int)src[4] << 8) | src[5];
            a = ((unsigned int)src[6] << 8) | src[7];
            src += 8;
         }

         for (c = 0; c < 3; c++)
         {
            unsigned int bg = (c == 0) ? background->red :
                (c == 1) ? background->green : background->blue;

            expected = ref_compose(fg[c], bg, a, max);

            if (bit_depth == 8)
               actual = *out++;
            else
            {
               actual = ((unsigned int)out[0] << 8) | out[1];
               out += 2;
            }

            if (actual != expected)
            {
               if (errors < 5)
                  fprintf(stderr, "pngcompose: %s: pixel (%u,%u) channel %d:"
                      " got %u, expected %u"
                      " (fg=%u bg=%u alpha=%u)\n",
                      name, x, y, c, actual, expected, fg[c], bg, a);
               errors++;
            }
         }

         if (keep_alpha != 0)
         {
            /* The alpha channel must be passed through unchanged. */
            if (bit_depth == 8)
               actual = *out++;
            else
            {
               actual = ((unsigned int)out[0] << 8) | out[1];
               out += 2;
            }

            if (actual != a)
            {
               if (errors < 5)
                  fprintf(stderr, "pngcompose: %s: pixel (%u,%u) alpha:"
                      " got %u, expected %u\n", name, x, y, actual, a);
               errors++;
            }
         }
      }
   }

cleanup:
   if (png_ptr != NULL)
   {
      if (reading != 0)
         png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
      else
         png_destroy_write_struct(&png_ptr, &info_ptr);
   }

   free(rows);
   free(result);
   free(mw.data);

   if (errors != 0)
      fprintf(stderr, "pngcompose: %s: FAILED (%d mismatched samples)\n",
          name, errors);
   else
      printf("pngcompose: %s: OK (%ux%u, %d-bit, keep_alpha=%d)\n",
          name, width, height, bit_depth, keep_alpha);

   return errors;
}

/* Like run_roundtrip, but for grayscale+alpha images. */
static int
run_roundtrip_gray(const char *name, int bit_depth, int keep_alpha,
    png_uint_16 background_gray, png_bytep pixels,
    png_uint_32 width, png_uint_32 height)
{
   mem_writer mw;
   mem_reader mr;
   png_structp png_ptr = NULL;
   png_infop info_ptr = NULL;
   png_bytep *rows = NULL;
   png_bytep result = NULL;
   size_t row_bytes, out_row_bytes;
   int out_color_type, out_channels, errors = 0;
   int reading = 0;
   unsigned int max = (bit_depth == 16) ? 65535U : 255U;
   png_color_16 background;
   png_uint_32 x, y;

   out_color_type = (keep_alpha != 0) ?
       PNG_COLOR_TYPE_GRAY_ALPHA : PNG_COLOR_TYPE_GRAY;
   out_channels = (keep_alpha != 0) ? 2 : 1;
   row_bytes = (size_t)width * 2 * (bit_depth / 8);
   out_row_bytes = (size_t)width * out_channels * (bit_depth / 8);

   memset(&background, 0, sizeof background);
   background.gray = background_gray;
   memset(&mw, 0, sizeof mw);

   png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
       NULL, NULL, NULL);
   if (png_ptr == NULL)
   {
      fprintf(stderr, "pngcompose: %s: png_create_write_struct failed\n",
          name);
      errors = 1;
      goto cleanup;
   }

   info_ptr = png_create_info_struct(png_ptr);
   if (info_ptr == NULL)
   {
      fprintf(stderr, "pngcompose: %s: png_create_info_struct failed\n",
          name);
      errors = 1;
      goto cleanup;
   }

   if (setjmp(png_jmpbuf(png_ptr)))
   {
      fprintf(stderr, "pngcompose: %s: libpng error during write\n", name);
      errors = 1;
      goto cleanup;
   }

   png_set_write_fn(png_ptr, &mw, mem_write_fn, mem_flush_fn);

   png_set_IHDR(png_ptr, info_ptr, width, height, bit_depth, out_color_type,
       PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

   png_write_info(png_ptr, info_ptr);

   png_set_write_background(png_ptr, &background, keep_alpha);

   rows = (png_bytep *)malloc(height * sizeof(png_bytep));
   if (rows == NULL)
   {
      fprintf(stderr, "pngcompose: %s: out of memory\n", name);
      errors = 1;
      goto cleanup;
   }
   for (y = 0; y < height; y++)
      rows[y] = pixels + (size_t)y * row_bytes;

   png_write_image(png_ptr, rows);
   png_write_end(png_ptr, info_ptr);

   png_destroy_write_struct(&png_ptr, &info_ptr);
   free(rows);
   rows = NULL;

   mr.data = mw.data;
   mr.size = mw.size;
   mr.offset = 0;

   png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
       NULL, NULL, NULL);
   if (png_ptr == NULL)
   {
      fprintf(stderr, "pngcompose: %s: png_create_read_struct failed\n",
          name);
      errors = 1;
      goto cleanup;
   }
   reading = 1;

   info_ptr = png_create_info_struct(png_ptr);
   if (info_ptr == NULL)
   {
      fprintf(stderr, "pngcompose: %s: png_create_info_struct failed\n",
          name);
      errors = 1;
      goto cleanup;
   }

   if (setjmp(png_jmpbuf(png_ptr)))
   {
      fprintf(stderr, "pngcompose: %s: libpng error during read\n", name);
      errors = 1;
      goto cleanup;
   }

   png_set_read_fn(png_ptr, &mr, mem_read_fn);

   png_read_info(png_ptr, info_ptr);

   if (png_get_image_width(png_ptr, info_ptr) != width ||
       png_get_image_height(png_ptr, info_ptr) != height ||
       png_get_bit_depth(png_ptr, info_ptr) != bit_depth ||
       png_get_color_type(png_ptr, info_ptr) != out_color_type ||
       png_get_channels(png_ptr, info_ptr) != out_channels)
   {
      fprintf(stderr, "pngcompose: %s: unexpected output format\n", name);
      errors = 1;
      goto cleanup;
   }

   result = (png_bytep)malloc(out_row_bytes * height);
   rows = (png_bytep *)malloc(height * sizeof(png_bytep));
   if (result == NULL || rows == NULL)
   {
      fprintf(stderr, "pngcompose: %s: out of memory\n", name);
      errors = 1;
      goto cleanup;
   }
   for (y = 0; y < height; y++)
      rows[y] = result + (size_t)y * out_row_bytes;

   png_read_image(png_ptr, rows);
   png_read_end(png_ptr, info_ptr);

   for (y = 0; y < height; y++)
   {
      const png_byte *src = pixels + (size_t)y * row_bytes;
      const png_byte *out = result + (size_t)y * out_row_bytes;

      for (x = 0; x < width; x++)
      {
         unsigned int fg, a, expected, actual;

         if (bit_depth == 8)
         {
            fg = src[0];
            a = src[1];
            src += 2;
         }
         else
         {
            fg = ((unsigned int)src[0] << 8) | src[1];
            a = ((unsigned int)src[2] << 8) | src[3];
            src += 4;
         }

         expected = ref_compose(fg, background_gray, a, max);

         if (bit_depth == 8)
            actual = *out++;
         else
         {
            actual = ((unsigned int)out[0] << 8) | out[1];
            out += 2;
         }

         if (actual != expected)
         {
            if (errors < 5)
               fprintf(stderr, "pngcompose: %s: pixel (%u,%u) gray:"
                   " got %u, expected %u (fg=%u bg=%u alpha=%u)\n",
                   name, x, y, actual, expected, fg, background_gray, a);
            errors++;
         }

         if (keep_alpha != 0)
         {
            if (bit_depth == 8)
               actual = *out++;
            else
            {
               actual = ((unsigned int)out[0] << 8) | out[1];
               out += 2;
            }

            if (actual != a)
            {
               if (errors < 5)
                  fprintf(stderr, "pngcompose: %s: pixel (%u,%u) alpha:"
                      " got %u, expected %u\n", name, x, y, actual, a);
               errors++;
            }
         }
      }
   }

cleanup:
   if (png_ptr != NULL)
   {
      if (reading != 0)
         png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
      else
         png_destroy_write_struct(&png_ptr, &info_ptr);
   }

   free(rows);
   free(result);
   free(mw.data);

   if (errors != 0)
      fprintf(stderr, "pngcompose: %s: FAILED (%d mismatched samples)\n",
          name, errors);
   else
      printf("pngcompose: %s: OK (%ux%u, %d-bit, keep_alpha=%d)\n",
          name, width, height, bit_depth, keep_alpha);

   return errors;
}

/* ---------------- performance test ---------------- */

/* Write one image to memory, timing only the png_write_image call.
 * Returns the elapsed CPU time in milliseconds.
 */
static double
time_write(int bit_depth, int use_background, const png_color_16 *background,
    png_bytep *rows, png_uint_32 width, png_uint_32 height, mem_writer *mw)
{
   png_structp png_ptr;
   png_infop info_ptr;
   clock_t start, end;
   double elapsed = -1;

   png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
       NULL, NULL, NULL);
   if (png_ptr == NULL)
      return -1;

   info_ptr = png_create_info_struct(png_ptr);
   if (info_ptr == NULL)
   {
      png_destroy_write_struct(&png_ptr, NULL);
      return -1;
   }

   if (setjmp(png_jmpbuf(png_ptr)))
   {
      fprintf(stderr, "pngcompose: perf: libpng error during write\n");
      png_destroy_write_struct(&png_ptr, &info_ptr);
      return -1;
   }

   /* Compression is disabled so that the measurement reflects the cost of
    * compositing, not of deflating; the remaining overhead (row copies,
    * CRC computation) is identical in the baseline measurement.
    */
   png_set_compression_level(png_ptr, 0);
   png_set_filter(png_ptr, 0, PNG_FILTER_NONE);

   png_set_write_fn(png_ptr, mw, mem_write_fn, mem_flush_fn);

   png_set_IHDR(png_ptr, info_ptr, width, height, bit_depth,
       PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
       PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

   png_write_info(png_ptr, info_ptr);

   if (use_background != 0)
      png_set_write_background(png_ptr, background, 1 /* keep alpha */);

   start = clock();
   png_write_image(png_ptr, rows);
   end = clock();

   png_write_end(png_ptr, info_ptr);
   png_destroy_write_struct(&png_ptr, &info_ptr);

   elapsed = 1000.0 * (double)(end - start) / CLOCKS_PER_SEC;
   return elapsed;
}

static int
run_performance_test(int bit_depth, png_uint_32 size)
{
   const double budget_ms = 40.0;
   png_uint_32 width = size, height = size;
   size_t pixel_count = (size_t)width * height;
   size_t row_bytes = (size_t)width * 4 * (bit_depth / 8);
   png_bytep pixels = NULL;
   png_bytep *rows = NULL;
   png_color_16 background;
   mem_writer mw;
   double t_plain = 1.0e30, t_composed = 1.0e30, delta, t;
   unsigned int max = (bit_depth == 16) ? 65535U : 255U;
   png_uint_32 x, y;
   int rep, errors = 0;

   pixels = (png_bytep)malloc(pixel_count * 4 * (bit_depth / 8));
   rows = (png_bytep *)malloc(height * sizeof(png_bytep));
   if (pixels == NULL || rows == NULL)
   {
      fprintf(stderr, "pngcompose: perf: out of memory\n");
      free(pixels);
      free(rows);
      return 1;
   }

   /* A realistic mix of opaque, transparent and semi-transparent pixels. */
   for (y = 0; y < height; y++)
   {
      png_bytep px = pixels + (size_t)y * row_bytes;

      rows[y] = px;
      for (x = 0; x < width; x++, px += 4 * (bit_depth / 8))
         make_pixel(GROUP_SEMI, x, y, bit_depth, px);
   }

   memset(&background, 0, sizeof background);
   background.red = (png_uint_16)(max * 4 / 5);
   background.green = (png_uint_16)(max / 2);
   background.blue = (png_uint_16)(max / 5);

   /* Pre-size the output buffer so that no reallocation happens inside
    * the timed region.
    */
   memset(&mw, 0, sizeof mw);
   mw.capacity = pixel_count * 4 * (bit_depth / 8) + height + 65536;
   mw.data = (png_bytep)malloc(mw.capacity);
   if (mw.data == NULL)
   {
      fprintf(stderr, "pngcompose: perf: out of memory\n");
      free(pixels);
      free(rows);
      return 1;
   }

   /* Measure the plain and the compositing write back to back, so that
    * both experience the same system load, and keep the best pair of
    * measurements: the minima are the values least disturbed by other
    * activity on the machine.
    */
   for (rep = 0; rep < 7; rep++)
   {
      mw.size = 0;
      t = time_write(bit_depth, 0, NULL, rows, width, height, &mw);
      if (t >= 0 && t < t_plain)
         t_plain = t;

      mw.size = 0;
      t = time_write(bit_depth, 1, &background, rows, width, height, &mw);
      if (t >= 0 && t < t_composed)
         t_composed = t;
   }

   free(mw.data);
   free(pixels);
   free(rows);

   if (t_plain > 1.0e29 || t_composed > 1.0e29)
   {
      fprintf(stderr, "pngcompose: perf: write failed\n");
      return 1;
   }

   delta = t_composed - t_plain;

   printf("pngcompose: perf %d-bit %ux%u: write %.1f ms,"
       " write+compose %.1f ms, compose %.1f ms (budget %.1f ms)\n",
       bit_depth, width, height, t_plain, t_composed, delta, budget_ms);

   if (delta > budget_ms)
   {
      fprintf(stderr, "pngcompose: perf %d-bit: compositing took %.1f ms,"
          " over the %.1f ms budget\n", bit_depth, delta, budget_ms);
      errors = 1;
   }

   return errors;
}

/* ---------------- main ---------------- */

int
main(void)
{
   const png_uint_32 width = 263, height = 127;
   png_bytep pixels8 = NULL, pixels16 = NULL, pixelsg8 = NULL,
       pixelsg16 = NULL;
   png_color_16 background8, background16;
   png_uint_32 x, y;
   int result = 0;

   /* Background colors with distinct channel values, to catch any
    * channel mix-up, and far from black, to catch "black edge" bugs:
    * a semi-transparent edge composited over these backgrounds must not
    * come out darker than both the foreground and the background.
    */
   memset(&background8, 0, sizeof background8);
   background8.red = 250;
   background8.green = 128;
   background8.blue = 10;
   background8.gray = 200;

   memset(&background16, 0, sizeof background16);
   background16.red = 64000;
   background16.green = 32768;
   background16.blue = 1000;
   background16.gray = 50000;

   pixels8 = (png_bytep)malloc((size_t)width * height * 4);
   pixels16 = (png_bytep)malloc((size_t)width * height * 8);
   pixelsg8 = (png_bytep)malloc((size_t)width * height * 2);
   pixelsg16 = (png_bytep)malloc((size_t)width * height * 4);
   if (pixels8 == NULL || pixels16 == NULL ||
       pixelsg8 == NULL || pixelsg16 == NULL)
   {
      fprintf(stderr, "pngcompose: out of memory\n");
      result = 1;
      goto cleanup;
   }

   /* Group 1: semi-transparent edges (8-bit). */
   for (y = 0; y < height; y++)
      for (x = 0; x < width; x++)
         make_pixel(GROUP_SEMI, x, y, 8,
             pixels8 + ((size_t)y * width + x) * 4);

   result += run_roundtrip("semi-transparent/strip-alpha", 8, 0,
       &background8, pixels8, width, height);
   result += run_roundtrip("semi-transparent/keep-alpha", 8, 1,
       &background8, pixels8, width, height);

   /* Group 2: fully transparent (8-bit).  The color channels are
    * deliberately non-zero: the output must be the background color
    * everywhere, not a darkened ("black edge") version of it.
    */
   for (y = 0; y < height; y++)
      for (x = 0; x < width; x++)
         make_pixel(GROUP_TRANSPARENT, x, y, 8,
             pixels8 + ((size_t)y * width + x) * 4);

   result += run_roundtrip("transparent/strip-alpha", 8, 0,
       &background8, pixels8, width, height);
   result += run_roundtrip("transparent/keep-alpha", 8, 1,
       &background8, pixels8, width, height);

   /* Group 3: fully opaque (8-bit).  The output must equal the input
    * color channels exactly.
    */
   for (y = 0; y < height; y++)
      for (x = 0; x < width; x++)
         make_pixel(GROUP_OPAQUE, x, y, 8,
             pixels8 + ((size_t)y * width + x) * 4);

   result += run_roundtrip("opaque/strip-alpha", 8, 0,
       &background8, pixels8, width, height);
   result += run_roundtrip("opaque/keep-alpha", 8, 1,
       &background8, pixels8, width, height);

   /* Group 4: semi-transparent edges (16-bit). */
   for (y = 0; y < height; y++)
      for (x = 0; x < width; x++)
         make_pixel(GROUP_SEMI16, x, y, 16,
             pixels16 + ((size_t)y * width + x) * 8);

   result += run_roundtrip("semi-transparent-16/strip-alpha", 16, 0,
       &background16, pixels16, width, height);
   result += run_roundtrip("semi-transparent-16/keep-alpha", 16, 1,
       &background16, pixels16, width, height);

   /* Grayscale+alpha coverage, 8 and 16-bit. */
   for (y = 0; y < height; y++)
   {
      for (x = 0; x < width; x++)
      {
         png_bytep px = pixelsg8 + ((size_t)y * width + x) * 2;

         px[0] = (png_byte)channel_pattern(x, y, 0, 255);
         px[1] = (png_byte)((x + y * 3) & 0xff);
      }
   }

   result += run_roundtrip_gray("gray-alpha-8/strip-alpha", 8, 0,
       background8.gray, pixelsg8, width, height);
   result += run_roundtrip_gray("gray-alpha-8/keep-alpha", 8, 1,
       background8.gray, pixelsg8, width, height);

   for (y = 0; y < height; y++)
   {
      for (x = 0; x < width; x++)
      {
         png_bytep px = pixelsg16 + ((size_t)y * width + x) * 4;
         unsigned int g = channel_pattern(x, y, 0, 65535);
         png_uint_32 a = (x * 8191 + y * 3) & 0xffff;

         px[0] = (png_byte)(g >> 8); px[1] = (png_byte)(g & 0xff);
         px[2] = (png_byte)(a >> 8); px[3] = (png_byte)(a & 0xff);
      }
   }

   result += run_roundtrip_gray("gray-alpha-16/strip-alpha", 16, 0,
       background16.gray, pixelsg16, width, height);
   result += run_roundtrip_gray("gray-alpha-16/keep-alpha", 16, 1,
       background16.gray, pixelsg16, width, height);

   /* Performance: compositing a 4096x4096 image must take < 40 ms. */
   result += run_performance_test(8, 4096);
   result += run_performance_test(16, 4096);

cleanup:
   free(pixels8);
   free(pixels16);
   free(pixelsg8);
   free(pixelsg16);

   if (result != 0)
      fprintf(stderr, "pngcompose: FAILED\n");

   return result != 0 ? 1 : 0;
}

#else /* !PNG_WRITE_BACKGROUND_SUPPORTED */

int
main(void)
{
   fprintf(stderr, "pngcompose: not supported:"
       " PNG_WRITE_BACKGROUND_SUPPORTED is not compiled in\n");
   return 0;
}

#endif /* WRITE_BACKGROUND */
