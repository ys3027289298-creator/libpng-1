/* webpreadtest.c
 *
 * Copyright (c) 2026 libpng contributors
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * Test driver for the webpread adapter.  It plays the role of the "old
 * pipeline": a libpng progressive reader with info/row/end callbacks
 * (the same pattern as example.c).  Input bytes are pushed through the
 * webpread adapter (or, with --direct, straight into png_process_data,
 * which reproduces the pre-adapter behavior).
 *
 * For every input file one RESULT line is printed:
 *
 *    RESULT file=<f> mode=<adapter|direct> format=<PNG|WEBP|PENDING>
 *           status=<status> rows=<n> size=<w>x<h> crc32=0x<hex> msg=<m>
 *
 * rows is the number of row callbacks that delivered a row; crc32 is the
 * CRC32 of the combined 8-bit RGBA image (same normalization for PNG and
 * WebP inputs, so checksums are directly comparable).  A final PEAK_RSS
 * line reports the process peak resident set size.
 *
 * Usage: webpreadtest [--direct] [--chunk N] file...
 */
#include "webpread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>
#include <sys/resource.h>

#define DRIVER_MAX_IMAGE (256u*1024u*1024u)   /* driver-side image cap */

typedef struct
{
   png_bytep    image;
   png_uint_32  w, h;
   size_t       rowbytes;
   unsigned long rows;
   int          ended, got_info;
   char         err[160];
} app_state;

static void PNGCBAPI
info_cb(png_structp png_ptr, png_infop info_ptr)
{
   app_state   *a = (app_state *)png_get_progressive_ptr(png_ptr);
   png_uint_32 w, h;
   int         bit_depth, color_type, interlace, compression, filter;

   png_get_IHDR(png_ptr, info_ptr, &w, &h, &bit_depth, &color_type,
       &interlace, &compression, &filter);

   /* Normalize everything to 8-bit RGBA so that PNG and WebP inputs
    * produce identically laid-out rows.
    */
   if (color_type == PNG_COLOR_TYPE_PALETTE)
      png_set_palette_to_rgb(png_ptr);
   if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
      png_set_expand_gray_1_2_4_to_8(png_ptr);
   if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
      png_set_tRNS_to_alpha(png_ptr);
   if (color_type == PNG_COLOR_TYPE_GRAY ||
       color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
      png_set_gray_to_rgb(png_ptr);
   if (bit_depth == 16)
      png_set_scale_16(png_ptr);
   if ((color_type & PNG_COLOR_MASK_ALPHA) == 0)
      png_set_add_alpha(png_ptr, 0xff, PNG_FILLER_AFTER);

   png_read_update_info(png_ptr, info_ptr);

   if (png_get_channels(png_ptr, info_ptr) != 4 ||
       png_get_bit_depth(png_ptr, info_ptr) != 8)
      png_error(png_ptr, "driver: RGBA8 normalization failed");

   a->rowbytes = png_get_rowbytes(png_ptr, info_ptr);
   if ((unsigned long long)a->rowbytes * h > DRIVER_MAX_IMAGE)
      png_error(png_ptr, "driver: image too large");
   a->image = (png_bytep)malloc(a->rowbytes * h);
   if (a->image == NULL)
      png_error(png_ptr, "driver: out of memory");

   a->w = w;
   a->h = h;
   a->got_info = 1;
}

static void PNGCBAPI
row_cb(png_structp png_ptr, png_bytep new_row, png_uint_32 row_num, int pass)
{
   app_state *a = (app_state *)png_get_progressive_ptr(png_ptr);

   (void)pass;
   if (new_row != NULL)
   {
      png_progressive_combine_row(png_ptr, a->image + row_num * a->rowbytes,
          new_row);
      a->rows++;
   }
}

static void PNGCBAPI
end_cb(png_structp png_ptr, png_infop info_ptr)
{
   app_state *a = (app_state *)png_get_progressive_ptr(png_ptr);

   (void)info_ptr;
   a->ended = 1;
}

static void PNGCBAPI
err_cb(png_structp png_ptr, png_const_charp msg)
{
   app_state *a = (app_state *)png_get_error_ptr(png_ptr);

   snprintf(a->err, sizeof a->err, "%s", msg != NULL ? msg : "?");
   longjmp(png_jmpbuf(png_ptr), 1);
}

static void PNGCBAPI
warn_cb(png_structp png_ptr, png_const_charp msg)
{
   (void)png_ptr;
   fprintf(stderr, "libpng warning: %s\n", msg != NULL ? msg : "?");
}

static int  opt_direct = 0;
static long opt_chunk = 4096;

static void
run_file(const char *filename)
{
   app_state    a;
   FILE        *fp;
   png_structp  png_ptr = NULL;
   png_infop    info_ptr = NULL;
   webp_adapt  *ad = NULL;
   const char  *status = "OK";
   char         msg[200] = "";
   unsigned long crc = 0;
   png_byte     buf[65536];

   memset(&a, 0, sizeof a);

   fp = fopen(filename, "rb");
   if (fp == NULL)
   {
      printf("RESULT file=%s mode=%s format=- status=IO_ERROR rows=0 "
          "size=0x0 crc32=0x00000000 msg=cannot-open\n",
          filename, opt_direct ? "direct" : "adapter");
      return;
   }

   png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, &a, err_cb,
       warn_cb);
   if (png_ptr != NULL)
      info_ptr = png_create_info_struct(png_ptr);
   if (png_ptr == NULL || info_ptr == NULL)
      status = "OOM";
   else if (setjmp(png_jmpbuf(png_ptr)))
   {
      status = "PNG_ERROR";
      snprintf(msg, sizeof msg, "%s", a.err);
   }
   else
   {
      png_set_progressive_read_fn(png_ptr, &a, info_cb, row_cb, end_cb);

      if (!opt_direct)
      {
         ad = webp_adapt_create(png_ptr, info_ptr);
         if (ad == NULL)
            png_error(png_ptr, "driver: out of memory");
      }

      for (;;)
      {
         size_t n = fread(buf, 1,
             (size_t)opt_chunk < sizeof buf ? (size_t)opt_chunk : sizeof buf,
             fp);
         int    eos = n == 0;

         if (opt_direct)
            png_process_data(png_ptr, info_ptr, buf, n);
         else
         {
            int st = webp_adapt_push(ad, buf, n, eos);

            if (st != WEBP_ADAPT_OK)
            {
               status = webp_adapt_status_name(st);
               snprintf(msg, sizeof msg, "%s", webp_adapt_message(ad));
               break;
            }
         }
         if (eos)
            break;
      }

      if (strcmp(status, "OK") == 0)
      {
         if (!a.ended)
            status = "INCOMPLETE";
         else if (a.got_info)
         {
            uLong c = crc32(0L, Z_NULL, 0);

            c = crc32(c, a.image, (uInt)(a.rowbytes * a.h));
            crc = (unsigned long)c;
         }
      }
   }

   {
      const char *format = "-";

      if (ad != NULL)
      {
         switch (webp_adapt_format(ad))
         {
            case WEBP_ADAPT_FMT_PNG:  format = "PNG";  break;
            case WEBP_ADAPT_FMT_WEBP: format = "WEBP"; break;
            default:                  format = "PENDING"; break;
         }
      }

      printf("RESULT file=%s mode=%s format=%s status=%s rows=%lu "
          "size=%lux%lu crc32=0x%08lx msg=%s\n",
          filename, opt_direct ? "direct" : "adapter", format, status,
          a.rows, (unsigned long)a.w, (unsigned long)a.h, crc,
          msg[0] != '\0' ? msg : "-");
   }

   webp_adapt_destroy(&ad);
   if (png_ptr != NULL)
      png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
   free(a.image);
   fclose(fp);
}

int
main(int argc, char **argv)
{
   int i;

   for (i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--direct") == 0)
         opt_direct = 1;
      else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc)
         opt_chunk = atol(argv[++i]);
      else if (argv[i][0] == '-' && argv[i][1] != '\0')
      {
         fprintf(stderr, "usage: webpreadtest [--direct] [--chunk N] "
             "file...\n");
         return 2;
      }
      else
         run_file(argv[i]);
   }

   {
      struct rusage ru;

      if (getrusage(RUSAGE_SELF, &ru) == 0)
         printf("PEAK_RSS kb=%ld\n", ru.ru_maxrss);
   }
   return 0;
}
