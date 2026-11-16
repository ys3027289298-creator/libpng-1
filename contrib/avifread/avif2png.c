/* avif2png.c - test driver / example for the contrib/avifread module
 *
 * Decodes an AVIF still image to a 16-bit PNG (RGB or RGBA), using:
 *   - png_avif_read_file() for the AVIF side (errors reported through the
 *     *same* png_error callback as libpng proper),
 *   - the classic libpng write API for the PNG side.
 *
 * Also provides --selftest, which measures the bit-depth and colour
 * conversion error bounds documented in pngavif.h.
 *
 * Usage:
 *   avif2png input.avif [output.png]
 *   avif2png --selftest
 *
 * Environment:
 *   PNGAVIF_DAV1D=path     use this libdav1d instead of the system one
 *   PNGAVIF_NO_DAV1D=1     simulate a missing AV1 decoder (failure-path test)
 */

#include "pngavif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

/* ---- error handling: one shared callback for PNG and AVIF failures ---- */

static void PNGCBAPI
app_error(png_structp png_ptr, png_const_charp message)
{
   fprintf(stderr, "[png-error-callback] %s\n", message);
   png_longjmp(png_ptr, 1);
}

static void PNGCBAPI
app_warning(png_structp png_ptr, png_const_charp message)
{
   (void)png_ptr;
   fprintf(stderr, "[png-warning-callback] %s\n", message);
}

/* ---- row sink: hand each 16-bit big-endian row to png_write_row ----
 *
 * png_avif_read_file() fills in the png_avif_info before the first row
 * callback, so the PNG header is written lazily on row 0 and the whole
 * conversion runs in a single decode pass.
 */

typedef struct
{
   png_structp write_ptr;
   png_infop write_info;
   const png_avif_info *info;
   int started;
} app_write_ctx;

static void PNGCBAPI
app_row(png_structp png_ptr, png_bytep row, png_uint_32 row_number,
    png_voidp user_data)
{
   app_write_ctx *ctx = (app_write_ctx *)user_data;
   (void)png_ptr;

   if (ctx->write_ptr == NULL)
      return; /* info-only pass */

   if (!ctx->started)
   {
      png_set_IHDR(ctx->write_ptr, ctx->write_info, ctx->info->width,
          ctx->info->height, 16,
          ctx->info->has_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
          PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
          PNG_FILTER_TYPE_DEFAULT);
      png_write_info(ctx->write_ptr, ctx->write_info);
      ctx->started = 1;
   }

   (void)row_number;
   png_write_row(ctx->write_ptr, row);
}

static long
app_peak_rss_kib(void)
{
   struct rusage ru;
   if (getrusage(RUSAGE_SELF, &ru) != 0)
      return -1;
   return ru.ru_maxrss;
}

/* ---- conversion self-test ---- */

static int
selftest(void)
{
   unsigned int v;
   int rc = 0;

   /* 8-bit expansion must be exact. */
   {
      unsigned int bad = 0;
      for (v = 0; v < 256; v++)
         if (pngavif_scale8(v) != (png_uint_16)(v * 257u))
            bad++;
      printf("selftest scale8: 256 values, exact (v*257), mismatches=%u "
             "-> %s\n", bad, bad == 0 ? "PASS" : "FAIL");
      if (bad)
         rc = 1;
   }

   /* 10-bit expansion: round-to-nearest, error <= 0.5 ulp16. */
   {
      double maxerr = 0.0;
      unsigned int bad = 0;
      for (v = 0; v < 1024; v++)
      {
         png_uint_16 got = pngavif_scale10(v);
         double ideal = (double)v * 65535.0 / 1023.0;
         double err = got - ideal;
         if (err < 0)
            err = -err;
         if (err > maxerr)
            maxerr = err;
         /* must equal round-to-nearest */
         if (got != (png_uint_16)(ideal + 0.5))
            bad++;
      }
      printf("selftest scale10: 1024 values, round-to-nearest, "
             "max|err|=%.4f ulp16 (bound 0.5), mismatches=%u -> %s\n",
             maxerr, bad, bad == 0 && maxerr <= 0.5 ? "PASS" : "FAIL");
      if (bad || maxerr > 0.5)
         rc = 1;
   }

   /* Colour matrix: fixed point vs double reference. */
   {
      static const struct { int mc, fr; const char *name; } cases[] =
      {
         { 1, 0, "BT.709 studio" },
         { 1, 1, "BT.709 full" },
         { 6, 0, "BT.601 studio" },
         { 9, 0, "BT.2020 studio" },
         { 9, 1, "BT.2020 full" },
      };
      int depths[2] = { 8, 10 };
      size_t ci, di;

      for (di = 0; di < 2; di++)
      {
         int depth = depths[di];
         unsigned int maxval = depth == 10 ? 1023u : 255u;
         unsigned int step = depth == 10 ? 37u : 1u;

         for (ci = 0; ci < sizeof cases / sizeof *cases; ci++)
         {
            pngavif_csc csc;
            double maxerr = 0.0;
            unsigned int y, u, v2;

            pngavif_csc_init(&csc, cases[ci].mc, cases[ci].fr, depth);

            for (y = 0; y <= maxval; y += step)
               for (u = 0; u <= maxval; u += step)
                  for (v2 = 0; v2 <= maxval; v2 += step)
                  {
                     png_uint_16 rgb[3];
                     double kr, kb, kg, yrng, crng, yoff, coff;
                     double yy, uu, vv, ref[3];
                     int k;

                     pngavif_csc_apply(&csc, y, u, v2, rgb);

                     switch (cases[ci].mc)
                     {
                        case 6: kr = 0.299; kb = 0.114; break;
                        case 9: kr = 0.2627; kb = 0.0593; break;
                        default: kr = 0.2126; kb = 0.0722; break;
                     }
                     kg = 1.0 - kr - kb;
                     if (cases[ci].fr)
                     {
                        yrng = crng = (double)maxval;
                        yoff = 0.0;
                     }
                     else
                     {
                        yrng = depth == 10 ? 876.0 : 219.0;
                        crng = depth == 10 ? 896.0 : 224.0;
                        yoff = depth == 10 ? 64.0 : 16.0;
                     }
                     coff = ((double)maxval + 1.0) / 2.0;

                     yy = ((double)y - yoff) * 65535.0 / yrng;
                     uu = ((double)u - coff) * 65535.0 / crng;
                     vv = ((double)v2 - coff) * 65535.0 / crng;

                     ref[0] = yy + 2.0 * (1.0 - kr) * vv;
                     ref[1] = yy - kb * 2.0 * (1.0 - kb) / kg * uu
                               - kr * 2.0 * (1.0 - kr) / kg * vv;
                     ref[2] = yy + 2.0 * (1.0 - kb) * uu;

                     for (k = 0; k < 3; k++)
                     {
                        double r = ref[k];
                        double err;
                        if (r < 0.0)
                           r = 0.0;
                        if (r > 65535.0)
                           r = 65535.0;
                        err = (double)rgb[k] - r;
                        if (err < 0)
                           err = -err;
                        if (err > maxerr)
                           maxerr = err;
                     }
                  }

            printf("selftest csc %d-bit %-15s: max|err|=%.3f ulp16 "
                   "(bound 2.0) -> %s\n", depth, cases[ci].name, maxerr,
                   maxerr <= 2.0 ? "PASS" : "FAIL");
            if (maxerr > 2.0)
               rc = 1;
         }
      }
   }

   return rc;
}

/* ---- main ---- */

/* Decode 'input' and, if 'output' is not NULL, write it as PNG16.
 * Returns 0 on success, 1 on failure (already reported via the callbacks).
 */
static int
run_one(const char *input, const char *output)
{
   png_structp read_ptr, write_ptr = NULL;
   png_infop write_info = NULL;
   png_avif_info info;
   app_write_ctx ctx;
   FILE * volatile out = NULL; /* set before setjmp, read in cleanup */
   volatile int failed = 0;

   read_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
       app_error, app_warning);
   if (read_ptr == NULL)
   {
      fprintf(stderr, "avif2png: cannot create png read struct\n");
      return 1;
   }

   if (output != NULL)
   {
      out = fopen(output, "wb");
      write_ptr = out ? png_create_write_struct(PNG_LIBPNG_VER_STRING,
          NULL, app_error, app_warning) : NULL;
      write_info = write_ptr ? png_create_info_struct(write_ptr) : NULL;
      if (write_info == NULL)
      {
         fprintf(stderr, "avif2png: cannot create %s\n", output);
         if (out != NULL)
            fclose(out);
         png_destroy_read_struct(&read_ptr, NULL, NULL);
         return 1;
      }
      png_init_io(write_ptr, out);
   }

   memset(&info, 0, sizeof info);
   memset(&ctx, 0, sizeof ctx);
   ctx.write_ptr = write_ptr;
   ctx.write_info = write_info;
   ctx.info = &info;

   /* Both jmpbufs are armed before the decode starts: a PNG write error
    * inside the row callback longjmps through write_ptr, an AVIF error
    * through read_ptr, and both land here.
    */
   if (write_ptr != NULL && setjmp(png_jmpbuf(write_ptr)))
      failed = 1;
   else if (setjmp(png_jmpbuf(read_ptr)))
      failed = 1;
   else
   {
      png_avif_read_file(read_ptr, input, &info, app_row, &ctx);

      printf("avif: %ux%u %d-bit -> 16-bit %s, CICP %d/%d/%d %s range\n",
             (unsigned)info.width, (unsigned)info.height, info.bit_depth,
             info.has_alpha ? "RGBA" : "RGB",
             info.colour_primaries, info.transfer_characteristics,
             info.matrix_coefficients,
             info.full_range ? "full" : "studio");

      if (write_ptr != NULL)
      {
         png_write_end(write_ptr, write_info);
         printf("png: wrote %s (16-bit %s)\n", output,
                info.has_alpha ? "RGBA" : "RGB");
      }
   }

   if (write_ptr != NULL)
      png_destroy_write_struct(&write_ptr, &write_info);
   if (out != NULL)
      fclose(out);
   png_destroy_read_struct(&read_ptr, NULL, NULL);

   if (failed)
   {
      fprintf(stderr, "avif2png: FAILED (%s)\n", input);
      return 1;
   }
   return 0;
}

int
main(int argc, char **argv)
{
   int rc;

   if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
      return selftest();

   if (argc < 2 || argc > 3)
   {
      fprintf(stderr, "usage: avif2png input.avif [output.png]\n"
              "       avif2png --selftest\n");
      return 2;
   }

   rc = run_one(argv[1], argc == 3 ? argv[2] : NULL);
   if (rc == 0)
      printf("mem: peak RSS %ld KiB\n", app_peak_rss_kib());
   return rc;
}
