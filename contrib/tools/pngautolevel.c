/* pngautolevel.c
 *
 * Copyright (c) 2026 Cosmin Truta
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * Apply percentile-based auto-levels to a 16-bit grayscale PNG.
 *
 * Usage:
 *   pngautolevel [--clip-low=PER-MILLE] [--clip-high=PER-MILLE]
 *                [--histogram=FILE] [--lut=FILE] input.png output.png
 *
 * The clip ratios default to 5 (0.5%) at each end and are configurable
 * independently.  --histogram writes the 65536 64-bit bucket counts, one
 * per line; --lut writes the 65536 output values of the stretch mapping
 * table, one per line.
 *
 * Only 16-bit grayscale PNGs (with or without an alpha channel) are accepted;
 * the alpha channel is passed through unchanged.
 */

#include "pnghist16.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PNG_FREESTANDING_TESTS
#  include <png.h>
#else
#  include "../../png.h"
#endif

#ifndef PNG_SETJMP_SUPPORTED
#  include <setjmp.h> /* because png.h did *not* include this */
#endif

#define DEFAULT_CLIP_PER_MILLE 5u

struct options
{
   const char *input;
   const char *output;
   const char *histogram_path;
   const char *lut_path;
   unsigned int clip_low;
   unsigned int clip_high;
};

static void
usage(const char *prog)
{
   fprintf(stderr,
      "usage: %s [--clip-low=PER-MILLE] [--clip-high=PER-MILLE]\n"
      "          [--histogram=FILE] [--lut=FILE] input.png output.png\n",
      prog);
}

static unsigned int
parse_per_mille(const char *text)
{
   char *end;
   long value = strtol(text, &end, 10);

   if (*end != '\0' || value < 0 || value > 1000)
   {
      fprintf(stderr, "pngautolevel: clip ratio must be 0..1000 per mille, "
         "got '%s'\n", text);
      exit(2);
   }

   return (unsigned int)value;
}

/* Match --name=value or --name followed by a separate argument.  Returns
 * the option value (either the suffix of argv[i] or argv[i+1]) and advances
 * *index past any consumed argument, or NULL when the name does not match.
 */
static const char *
option_value(int argc, char **argv, int *index, const char *name)
{
   const char *arg = argv[*index];
   size_t len = strlen(name);

   if (strncmp(arg, name, len) != 0)
      return NULL;

   if (arg[len] == '=')
      return arg + len + 1;

   if (arg[len] == '\0')
   {
      if (*index + 1 >= argc)
      {
         fprintf(stderr, "pngautolevel: option %s requires a value\n",
            name);
         exit(2);
      }
      ++*index;
      return argv[*index];
   }

   return NULL;
}

static struct options
parse_args(int argc, char **argv)
{
   struct options opt;
   int i;
   int positionals = 0;
   const char *value;

   opt.input = NULL;
   opt.output = NULL;
   opt.histogram_path = NULL;
   opt.lut_path = NULL;
   opt.clip_low = DEFAULT_CLIP_PER_MILLE;
   opt.clip_high = DEFAULT_CLIP_PER_MILLE;

   for (i = 1; i < argc; ++i)
   {
      const char *arg = argv[i];

      if ((value = option_value(argc, argv, &i, "--clip-low")) != NULL)
         opt.clip_low = parse_per_mille(value);
      else if ((value = option_value(argc, argv, &i, "--clip-high")) != NULL)
         opt.clip_high = parse_per_mille(value);
      else if ((value = option_value(argc, argv, &i, "--histogram")) != NULL)
         opt.histogram_path = value;
      else if ((value = option_value(argc, argv, &i, "--lut")) != NULL)
         opt.lut_path = value;
      else if (arg[0] == '-')
      {
         fprintf(stderr, "pngautolevel: unknown option '%s'\n", arg);
         usage(argv[0]);
         exit(2);
      }
      else if (positionals == 0)
      {
         opt.input = arg;
         ++positionals;
      }
      else if (positionals == 1)
      {
         opt.output = arg;
         ++positionals;
      }
      else
      {
         fprintf(stderr, "pngautolevel: too many file arguments\n");
         exit(2);
      }
   }

   if (opt.input == NULL || opt.output == NULL)
   {
      usage(argv[0]);
      exit(2);
   }

   return opt;
}

static void
write_uint64_list(const char *path, const uint64_t *values, size_t count)
{
   FILE *fp = fopen(path, "w");
   size_t i;

   if (fp == NULL)
   {
      fprintf(stderr, "pngautolevel: cannot open %s\n", path);
      exit(1);
   }

   for (i = 0; i < count; ++i)
      fprintf(fp, "%llu\n", (unsigned long long)values[i]);

   fclose(fp);
}

static void
write_uint16_list(const char *path, const uint16_t *values, size_t count)
{
   FILE *fp = fopen(path, "w");
   size_t i;

   if (fp == NULL)
   {
      fprintf(stderr, "pngautolevel: cannot open %s\n", path);
      exit(1);
   }

   for (i = 0; i < count; ++i)
      fprintf(fp, "%u\n", (unsigned int)values[i]);

   fclose(fp);
}

int
main(int argc, char **argv)
{
   struct options opt = parse_args(argc, argv);
   png_structp read_ptr, write_ptr;
   png_infop read_info, write_info;
   FILE *in_fp = NULL, *out_fp = NULL;
   int bit_depth, color_type, interlace_type;
   png_uint_32 width, height;
   size_t rowbytes, channels, sample_stride;
   unsigned char *pixels = NULL;
   png_hist16 hist;
   int result = 1;
   png_uint_32 x, y;

   in_fp = fopen(opt.input, "rb");
   if (in_fp == NULL)
   {
      fprintf(stderr, "pngautolevel: cannot open %s\n", opt.input);
      return 1;
   }

   read_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL,
      NULL);
   read_info = (read_ptr != NULL) ? png_create_info_struct(read_ptr) : NULL;
   if (read_ptr == NULL || read_info == NULL ||
      setjmp(png_jmpbuf(read_ptr)))
   {
      png_destroy_read_struct(&read_ptr, &read_info, NULL);
      fclose(in_fp);
      return 1;
   }

   png_init_io(read_ptr, in_fp);
   png_read_info(read_ptr, read_info);

   width = png_get_image_width(read_ptr, read_info);
   height = png_get_image_height(read_ptr, read_info);
   bit_depth = png_get_bit_depth(read_ptr, read_info);
   color_type = png_get_color_type(read_ptr, read_info);
   interlace_type = png_get_interlace_type(read_ptr, read_info);

   if (bit_depth != 16 ||
      (color_type != PNG_COLOR_TYPE_GRAY &&
       color_type != PNG_COLOR_TYPE_GRAY_ALPHA))
   {
      fprintf(stderr, "pngautolevel: %s is not a 16-bit grayscale PNG "
         "(bit_depth=%d, color_type=%d)\n", opt.input, bit_depth,
         color_type);
      goto done_read;
   }

   channels = (color_type == PNG_COLOR_TYPE_GRAY_ALPHA) ? 2u : 1u;
   sample_stride = channels * 2u; /* bytes between gray samples in a row */
   rowbytes = (size_t)width * sample_stride;
   pixels = malloc((size_t)height * rowbytes);
   if (pixels == NULL)
   {
      fprintf(stderr, "pngautolevel: out of memory\n");
      goto done_read;
   }

   /* Pass 1: read every row and accumulate the histogram. */
   png_hist16_init(&hist);
   for (y = 0; y < height; ++y)
   {
      unsigned char *row = pixels + (size_t)y * rowbytes;

      png_read_row(read_ptr, row, NULL);
      if (png_hist16_count_row_be(&hist, row, (size_t)width,
            sample_stride) != PNG_HIST16_OK)
      {
         fprintf(stderr, "pngautolevel: histogram failure\n");
         goto done_read;
      }
   }
   png_read_end(read_ptr, NULL);

   {
      int rc = png_hist16_auto_levels(&hist, opt.clip_low, opt.clip_high);
      if (rc != PNG_HIST16_OK)
      {
         fprintf(stderr, "pngautolevel: %s\n", png_hist16_strerror(rc));
         goto done_read;
      }
   }

   printf("pngautolevel: %ux%u, total=%llu, clip=%u/%u per mille, "
      "cut=%u..%u\n", (unsigned)width, (unsigned)height,
      (unsigned long long)png_hist16_get_total(&hist), opt.clip_low,
      opt.clip_high, (unsigned)hist.low, (unsigned)hist.high);

   if (opt.histogram_path != NULL)
      write_uint64_list(opt.histogram_path,
         png_hist16_get_histogram(&hist), PNG_HIST16_SIZE);
   if (opt.lut_path != NULL)
      write_uint16_list(opt.lut_path, png_hist16_get_lut(&hist),
         PNG_HIST16_SIZE);

   /* Pass 2: apply the mapping table in place, then write the result. */
   for (y = 0; y < height; ++y)
   {
      unsigned char *row = pixels + (size_t)y * rowbytes;

      for (x = 0; x < width; ++x)
      {
         unsigned char *p = row + (size_t)x * sample_stride;
         uint16_t value = (uint16_t)(((unsigned int)p[0] << 8) | p[1]);
         uint16_t mapped = png_hist16_map(&hist, value);

         p[0] = (unsigned char)(mapped >> 8);
         p[1] = (unsigned char)(mapped & 0xff);
         /* alpha bytes, when present, are left untouched */
      }
   }

   out_fp = fopen(opt.output, "wb");
   if (out_fp == NULL)
   {
      fprintf(stderr, "pngautolevel: cannot open %s\n", opt.output);
      goto done_read;
   }

   write_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL,
      NULL);
   write_info = (write_ptr != NULL) ? png_create_info_struct(write_ptr) :
      NULL;
   if (write_ptr == NULL || write_info == NULL ||
      setjmp(png_jmpbuf(write_ptr)))
   {
      png_destroy_write_struct(&write_ptr, &write_info);
      fclose(out_fp);
      goto done_read;
   }

   png_init_io(write_ptr, out_fp);
   png_set_IHDR(write_ptr, write_info, width, height, 16, color_type,
      interlace_type, PNG_COMPRESSION_TYPE_DEFAULT,
      PNG_FILTER_TYPE_DEFAULT);
   png_write_info(write_ptr, write_info);

   for (y = 0; y < height; ++y)
      png_write_row(write_ptr, pixels + (size_t)y * rowbytes);
   png_write_end(write_ptr, write_info);

   result = 0;
   png_destroy_write_struct(&write_ptr, &write_info);
   fclose(out_fp);

done_read:
   free(pixels);
   png_destroy_read_struct(&read_ptr, &read_info, NULL);
   fclose(in_fp);
   return result;
}
