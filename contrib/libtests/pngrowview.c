/* pngrowview.c
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * Tests for the zero-copy row view API of the progressive reader:
 *
 *  - the rows delivered through png_row_view are pixel-identical to the
 *    rows seen by the classic copy-based row callback;
 *  - a row view is invalidated by the next row callback and reading it
 *    through png_row_view_at() afterwards raises an error;
 *  - out-of-range png_row_view_at() raises an error;
 *  - out-of-bounds writes to a viewed row are detected and raise an error;
 *  - the read-only flag is set on every view.
 *
 * With --bench the program instead generates a large PNG in memory and
 * compares the decode time of the copy-based row callback with the
 * zero-copy row view.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>

#if defined(HAVE_CONFIG_H) && !defined(PNG_NO_CONFIG_H)
#  include <config.h>
#endif

#ifdef PNG_FREESTANDING_TESTS
#  include <png.h>
#else
#  include "../../png.h"
#endif

#ifndef PNG_PROGRESSIVE_READ_SUPPORTED
int main(void)
{
   fprintf(stderr, "pngrowview: progressive read not supported\n");
   return 77; /* skipped */
}
#else

#define ARRAY_SIZE(a) (sizeof (a) / sizeof (a)[0])

/* 64-bit hash value (libpng has no public 64-bit type) */
typedef unsigned long long hash_t;

/* ------------------------- in-memory PNG file ------------------------- */

typedef struct
{
   png_bytep data;
   size_t    size;
   size_t    cap;
} mem_png;

static void
mem_png_free(mem_png *m)
{
   free(m->data);
   m->data = NULL;
   m->size = m->cap = 0;
}

static int
mem_png_append(mem_png *m, const png_byte *data, size_t len)
{
   if (m->size + len > m->cap)
   {
      size_t cap = (m->cap == 0) ? 65536 : m->cap;
      png_bytep p;
      while (cap < m->size + len)
         cap *= 2;
      p = (png_bytep)realloc(m->data, cap);
      if (p == NULL)
         return 0;
      m->data = p;
      m->cap = cap;
   }
   memcpy(m->data + m->size, data, len);
   m->size += len;
   return 1;
}

/* ------------------------------ test state ---------------------------- */

typedef struct
{
   /* error capture (used as the libpng error pointer) */
   jmp_buf    jmp;
   char       error[200];
   int        error_seen;

   /* image properties, filled in by the info callback */
   png_uint_32 width, height;
   int        bit_depth, color_type, interlaced;
   size_t     rowbytes;     /* final row size in bytes */

   /* copy-based path output: the "business buffer" */
   png_bytep  image;        /* full image: height * rowbytes */
   size_t     img_size;     /* allocated size of image and image2 */

   /* view-based path output (non-interlaced identity tests only) */
   png_bytep  image2;

   /* interlaced tests: test-only copy of the last delivered row */
   png_bytep  last_row;
   size_t     last_size;    /* meaningful bytes in last_row */
   unsigned int pixel_depth;/* bits per pixel */

   /* stream hash of the delivered rows */
   hash_t hash;
   png_uint_32 rows_seen;

   /* view-path checks */
   int        readonly_missing; /* set if any view lacks READ_ONLY */
   int        size_mismatch;    /* set if any view has a wrong size */

   /* invalidation test */
   png_row_view saved;
   int          have_saved;

   /* when zero, the view callback does not assemble image2 (benchmark) */
   int          assemble;

   /* test action mode */
   int          mode;
   png_uint_32  trigger_row;
} test_state;

/* Test action modes */
#define MODE_NONE          0
#define MODE_INVALIDATE    1 /* read a stale view after trigger_row */
#define MODE_BOUNDS        2 /* png_row_view_at out of range on trigger_row */
#define MODE_OOB_WRITE_HI  3 /* write past the row on trigger_row */
#define MODE_OOB_WRITE_LO  4 /* write before the row on trigger_row */

static void PNGCBAPI
test_error(png_structp png_ptr, png_const_charp msg)
{
   test_state *st = (test_state *)png_get_error_ptr(png_ptr);

   strncpy(st->error, (msg != NULL) ? msg : "(null)", sizeof st->error - 1);
   st->error[sizeof st->error - 1] = '\0';
   st->error_seen = 1;
   longjmp(st->jmp, 1);
}

static void PNGCBAPI
test_warn(png_structp png_ptr, png_const_charp msg)
{
   (void)png_ptr;
   (void)msg;
}

/* Fast order-dependent row stream hash. */
static hash_t
hash_row(hash_t h, const png_byte *p, size_t n)
{
   hash_t s = 0;

   while (n >= 8)
   {
      hash_t v;
      memcpy(&v, p, 8);
      s += v;
      p += 8;
      n -= 8;
   }
   while (n-- > 0)
      s = s * 131 + *p++;

   return h * 1099511628211ULL ^ s;
}

/* --------------------------- PNG generation --------------------------- */

typedef struct
{
   mem_png *m;
   int      failed;
} mem_write_state;

static void PNGCBAPI
mem_write_fn(png_structp png_ptr, png_bytep data, size_t length)
{
   mem_write_state *ws = (mem_write_state *)png_get_io_ptr(png_ptr);
   if (mem_png_append(ws->m, data, length) == 0)
   {
      ws->failed = 1;
      png_error(png_ptr, "out of memory");
   }
}

static void PNGCBAPI
mem_flush_fn(png_structp png_ptr)
{
   (void)png_ptr;
}

/* Deterministic pixel pattern: varied but compressible. */
static void
fill_row(png_bytep row, png_uint_32 width, int channels, int depth,
    png_uint_32 y)
{
   png_uint_32 x;
   int c;

   if (depth == 8)
   {
      for (x = 0; x < width; x++)
         for (c = 0; c < channels; c++)
            *row++ = (png_byte)(x * 37 + y * 11 + c * 53 + ((x * y) & 15));
   }
   else /* depth == 16 */
   {
      for (x = 0; x < width; x++)
         for (c = 0; c < channels; c++)
         {
            unsigned int v = (x * 37 + y * 11 + c * 53 + ((x * y) & 15)) * 257;
            *row++ = (png_byte)(v >> 8);
            *row++ = (png_byte)(v & 0xff);
         }
   }
}

/* Generate a PNG in memory.  'filters' is a PNG_FILTER_* mask; use
 * PNG_ALL_FILTERS for the default adaptive filtering.
 * Returns 0 on success.
 */
static int
make_png(mem_png *out, png_uint_32 width, png_uint_32 height, int bit_depth,
    int color_type, int interlace, int filters)
{
   png_structp wp;
   png_infop   wi;
   mem_write_state ws;
   png_bytep   row = NULL;
   png_uint_32 y;
   int         channels;

   memset(out, 0, sizeof *out);

   wp = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   if (wp == NULL)
      return 1;
   wi = png_create_info_struct(wp);
   if (wi == NULL)
   {
      png_destroy_write_struct(&wp, NULL);
      return 1;
   }

   if (setjmp(png_jmpbuf(wp)))
   {
      png_destroy_write_struct(&wp, &wi);
      free(row);
      mem_png_free(out);
      return 1;
   }

   ws.m = out;
   ws.failed = 0;
   png_set_write_fn(wp, &ws, mem_write_fn, mem_flush_fn);

   png_set_IHDR(wp, wi, width, height, bit_depth, color_type, interlace,
       PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
   png_set_filter(wp, 0, filters);

   png_write_info(wp, wi);

   /* Interlaced images must be written once per pass. */
   {
      int passes = png_set_interlace_handling(wp);
      int pass;

      channels = png_get_channels(wp, wi);
      row = (png_bytep)malloc(png_get_rowbytes(wp, wi));
      if (row == NULL)
         png_error(wp, "out of memory");

      for (pass = 0; pass < passes; pass++)
         for (y = 0; y < height; y++)
         {
            fill_row(row, width, channels, bit_depth, y);
            png_write_row(wp, row);
         }
   }

   png_write_end(wp, wi);
   png_destroy_write_struct(&wp, &wi);
   free(row);

   if (ws.failed != 0)
   {
      mem_png_free(out);
      return 1;
   }
   return 0;
}

/* ------------------------- progressive decoding ----------------------- */

static void
feed_png(png_structp rp, png_infop ri, const mem_png *png)
{
   /* Feed in varying chunk sizes to exercise the save/restore buffering. */
   size_t pos = 0;
   size_t step = 7;

   while (pos < png->size)
   {
      size_t n = png->size - pos;
      if (n > step)
         n = step;
      png_process_data(rp, ri, png->data + pos, n);
      pos += n;
      step = step * 3 + 1;
      if (step > 65536)
         step = 65536;
   }
}

static void PNGCBAPI
info_cb(png_structp rp, png_infop ri)
{
   test_state *st = (test_state *)png_get_progressive_ptr(rp);

   st->width = png_get_image_width(rp, ri);
   st->height = png_get_image_height(rp, ri);
   st->bit_depth = png_get_bit_depth(rp, ri);
   st->color_type = png_get_color_type(rp, ri);
   st->interlaced = png_get_interlace_type(rp, ri);

   /* Required for the interlaced tests: makes the row callbacks deliver
    * full-width rows.
    */
   png_set_interlace_handling(rp);
   png_read_update_info(rp, ri);

   st->rowbytes = png_get_rowbytes(rp, ri);
   st->pixel_depth = (unsigned int)png_get_channels(rp, ri) *
       (unsigned int)st->bit_depth;

   /* The info callback may run several times (benchmark repetitions);
    * reuse the buffers when the size has not changed so that allocation
    * and first-touch costs stay out of the measured decode time.
    */
   {
      size_t need = (size_t)st->height * st->rowbytes;

      if (st->img_size != need)
      {
         free(st->image);
         free(st->image2);
         st->image = (png_bytep)malloc(need);
         st->image2 = (png_bytep)malloc(need);
         st->img_size = (st->image != NULL && st->image2 != NULL) ? need : 0;
      }
      free(st->last_row);
      st->last_row = (png_bytep)malloc(st->rowbytes);
      if (st->image == NULL || st->image2 == NULL || st->last_row == NULL)
         png_error(rp, "out of memory");
      if (st->assemble != 0)
      {
         memset(st->image, 0, need);
         memset(st->image2, 0, need);
      }
   }
}

static void PNGCBAPI
end_cb(png_structp rp, png_infop ri)
{
   (void)rp;
   (void)ri;
}

/* Classic row callback: copy every row into the application buffer, the
 * way applications are required to do without the row view API.
 */
static void PNGCBAPI
row_cb_copy(png_structp rp, png_bytep new_row, png_uint_32 row_number,
    int pass)
{
   test_state *st = (test_state *)png_get_progressive_ptr(rp);
   png_bytep row = st->image + (size_t)row_number * st->rowbytes;

   (void)pass;
   png_progressive_combine_row(rp, row, new_row);
   st->hash = hash_row(st->hash, row, st->rowbytes);
   st->rows_seen++;
}

/* The number of meaningful row bytes delivered for a row of the given
 * interlace pass: for interlaced images read with
 * png_set_interlace_handling the expanded row covers iwidth*inc pixels,
 * which may be narrower or wider than the image.
 */
static size_t
delivered_row_bytes(const test_state *st, int pass)
{
   if (st->interlaced != 0)
   {
      png_uint_32 cols = PNG_PASS_COLS(st->width, pass) <<
          PNG_PASS_COL_SHIFT(pass);
      png_uint_32 meaningful = (cols < st->width) ? cols : st->width;
      return ((size_t)meaningful * st->pixel_depth + 7) / 8;
   }
   return st->rowbytes;
}

/* Reference row callback: hash the delivered row pointer directly, without
 * the combine copy.  For interlaced images this is the stream the row view
 * delivery must reproduce.
 */
static void PNGCBAPI
row_cb_raw(png_structp rp, png_bytep new_row, png_uint_32 row_number,
    int pass)
{
   test_state *st = (test_state *)png_get_progressive_ptr(rp);

   (void)rp;
   (void)row_number;

   if (new_row != NULL)
   {
      size_t n = delivered_row_bytes(st, pass);
      memcpy(st->last_row, new_row, n);
      st->last_size = n;
      st->hash = hash_row(st->hash, new_row, n);
   }
   else
      st->hash = hash_row(st->hash, st->last_row, st->last_size);
   st->rows_seen++;
}

/* Zero-copy row view callback: consume the row in place. */
static void PNGCBAPI
row_cb_view(png_structp rp, const png_row_view *view)
{
   test_state *st = (test_state *)png_get_progressive_ptr(rp);

   if ((view->flags & PNG_ROW_VIEW_FLAG_READ_ONLY) == 0)
      st->readonly_missing = 1;

   if (view->data != NULL)
   {
      if (view->size != delivered_row_bytes(st, view->pass))
         st->size_mismatch = 1;

      st->hash = hash_row(st->hash, view->data, view->size);

      if (st->assemble != 0)
      {
         /* Test-only assembly of the delivered rows for later comparison;
          * a real zero-copy consumer does not make this copy.
          */
         if (st->interlaced == 0)
            memcpy(st->image2 + (size_t)view->row_number * st->rowbytes,
                view->data, view->size);
         else
            memcpy(st->last_row, view->data, view->size);
         st->last_size = view->size;
      }
   }
   else
   {
      /* Interlaced repeat row: the previous row is displayed again. */
      st->hash = hash_row(st->hash, st->last_row, st->last_size);
   }
   st->rows_seen++;

   /* Test actions. */
   if (view->data != NULL && st->mode == MODE_INVALIDATE)
   {
      if (st->have_saved == 0)
      {
         st->saved = *view;
         st->have_saved = 1;
      }
      else
      {
         /* The saved view must be invalid by now; reading it must raise a
          * png_error.
          */
         if (png_row_view_valid(&st->saved) != 0)
            png_error(rp, "test: stale view still marked valid");
         (void)png_row_view_at(&st->saved, 0);
         png_error(rp, "test: stale view read did not fail");
      }
   }
   else if (view->data != NULL && view->row_number == st->trigger_row)
   {
      switch (st->mode)
      {
         case MODE_BOUNDS:
            (void)png_row_view_at(view, 0); /* in range: must succeed */
            (void)png_row_view_at(view, view->size); /* must raise png_error */
            png_error(rp, "test: out-of-range read did not fail");
            break;

         case MODE_OOB_WRITE_HI:
            ((png_bytep)view->data)[view->size + 4] = 0;
            break;

         case MODE_OOB_WRITE_LO:
            ((png_bytep)view->data)[-10] = 0;
            break;

         default:
            break;
      }
   }
}

static png_structp
make_reader(test_state *st, png_infop *ri_out)
{
   png_structp rp;
   png_infop   ri;

   rp = png_create_read_struct(PNG_LIBPNG_VER_STRING, st, test_error,
       test_warn);
   if (rp == NULL)
   {
      fprintf(stderr, "pngrowview: png_create_read_struct failed\n");
      exit(1);
   }
   ri = png_create_info_struct(rp);
   if (ri == NULL)
   {
      png_destroy_read_struct(&rp, NULL, NULL);
      fprintf(stderr, "pngrowview: png_create_info_struct failed\n");
      exit(1);
   }
   *ri_out = ri;
   return rp;
}

/* Decode a PNG with the given row delivery mode:
 *   0: copy-based callback (png_progressive_combine_row)
 *   1: zero-copy row view
 *   2: classic callback, no copy (reference stream)
 */
static void
decode(test_state *st, const mem_png *png, int delivery)
{
   png_structp rp;
   png_infop   ri;

   rp = make_reader(st, &ri);

   if (setjmp(st->jmp) == 0)
   {
      if (delivery == 1)
      {
         png_set_progressive_read_fn(rp, st, info_cb, NULL, end_cb);
         png_set_progressive_row_view_fn(rp, row_cb_view);
      }
      else if (delivery == 2)
         png_set_progressive_read_fn(rp, st, info_cb, row_cb_raw, end_cb);
      else
         png_set_progressive_read_fn(rp, st, info_cb, row_cb_copy, end_cb);

      feed_png(rp, ri, png);
   }

   png_destroy_read_struct(&rp, &ri, NULL);
}

static void
state_init(test_state *st)
{
   memset(st, 0, sizeof *st);
   st->hash = 1469598103934665603ULL;
   st->assemble = 1;
}

static void
state_reset_stream(test_state *st)
{
   st->hash = 1469598103934665603ULL;
   st->rows_seen = 0;
}

static void
state_free(test_state *st)
{
   free(st->image);
   free(st->image2);
   free(st->last_row);
   st->image = st->image2 = st->last_row = NULL;
   st->img_size = 0;
}

/* ------------------------------- tests -------------------------------- */

/* Decode the same PNG in every delivery mode and verify that the zero-copy
 * row view produces pixel-identical results.
 */
static int
test_identical(const char *name, png_uint_32 width, png_uint_32 height,
    int bit_depth, int color_type, int interlace)
{
   mem_png png;
   test_state copy, raw, view;
   int ret = 1;

   if (make_png(&png, width, height, bit_depth, color_type, interlace,
       PNG_ALL_FILTERS) != 0)
   {
      fprintf(stderr, "pngrowview: %s: make_png failed\n", name);
      return 1;
   }

   state_init(&copy);
   decode(&copy, &png, 0);
   state_init(&raw);
   decode(&raw, &png, 2);
   state_init(&view);
   decode(&view, &png, 1);

   if (copy.error_seen || raw.error_seen || view.error_seen)
   {
      fprintf(stderr, "pngrowview: %s: unexpected error: %s %s %s\n", name,
          copy.error, raw.error, view.error);
      goto done;
   }
   if (view.rows_seen == 0 || view.rows_seen != raw.rows_seen ||
       view.rows_seen != copy.rows_seen)
   {
      fprintf(stderr, "pngrowview: %s: row count mismatch: %lu/%lu/%lu\n",
          name, (unsigned long)copy.rows_seen, (unsigned long)raw.rows_seen,
          (unsigned long)view.rows_seen);
      goto done;
   }
   /* The view stream must be identical to the classic delivered stream. */
   if (view.hash != raw.hash)
   {
      fprintf(stderr, "pngrowview: %s: row stream mismatch (view vs raw)\n",
          name);
      goto done;
   }
   if (view.readonly_missing)
   {
      fprintf(stderr, "pngrowview: %s: READ_ONLY flag missing\n", name);
      goto done;
   }
   if (view.size_mismatch)
   {
      fprintf(stderr, "pngrowview: %s: view size mismatch\n", name);
      goto done;
   }
   if (interlace == 0)
   {
      /* Non-interlaced: the assembled images and the row streams of the
       * copy-based and zero-copy paths must all be identical.
       */
      if (view.hash != copy.hash)
      {
         fprintf(stderr, "pngrowview: %s: row stream mismatch (view vs "
             "copy)\n", name);
         goto done;
      }
      if (memcmp(copy.image, view.image2,
          (size_t)copy.height * copy.rowbytes) != 0)
      {
         fprintf(stderr, "pngrowview: %s: pixel data mismatch\n", name);
         goto done;
      }
   }

   printf("pngrowview: %s: identical (%lu rows, %lu bytes/row)\n", name,
       (unsigned long)view.rows_seen, (unsigned long)view.rowbytes);
   ret = 0;

done:
   state_free(&copy);
   state_free(&raw);
   state_free(&view);
   mem_png_free(&png);
   return ret;
}

/* Run one error-detection test: decode in view mode with a test action and
 * verify that the expected libpng error is raised.
 */
static int
test_error_mode(const char *name, int mode, png_uint_32 trigger_row,
    const char *expect)
{
   mem_png png;
   test_state st;
   int ret = 1;

   if (make_png(&png, 37, 23, 8, PNG_COLOR_TYPE_RGB,
       PNG_INTERLACE_NONE, PNG_ALL_FILTERS) != 0)
   {
      fprintf(stderr, "pngrowview: %s: make_png failed\n", name);
      return 1;
   }

   state_init(&st);
   st.mode = mode;
   st.trigger_row = trigger_row;
   decode(&st, &png, 1);

   if (st.error_seen == 0)
   {
      fprintf(stderr, "pngrowview: %s: expected error was not raised\n",
          name);
      goto done;
   }
   if (strstr(st.error, expect) == NULL)
   {
      fprintf(stderr, "pngrowview: %s: wrong error: %s (expected '%s')\n",
          name, st.error, expect);
      goto done;
   }

   printf("pngrowview: %s: detected: %s\n", name, st.error);
   ret = 0;

done:
   state_free(&st);
   mem_png_free(&png);
   return ret;
}

/* ----------------------------- benchmark ------------------------------ */

static double
now_seconds(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int
bench_one(const mem_png *png, double *tcopy, double *tview, int *mismatch)
{
   /* Time both delivery modes interleaved and keep the best time of each,
    * so that clock frequency drift between the measurements does not bias
    * the comparison.
    */
   test_state sc, sv;
   double best_c = 0, best_v = 0;
   int rep;

   state_init(&sc);
   state_init(&sv);
   sc.assemble = 0;
   sv.assemble = 0; /* true zero-copy consumption in view mode */

   for (rep = 0; rep < 5; rep++)
   {
      double t0, t1;

      state_reset_stream(&sc);
      t0 = now_seconds();
      decode(&sc, png, 0);
      t1 = now_seconds();
      if (rep == 0 || t1 - t0 < best_c)
         best_c = t1 - t0;

      state_reset_stream(&sv);
      t0 = now_seconds();
      decode(&sv, png, 1);
      t1 = now_seconds();
      if (rep == 0 || t1 - t0 < best_v)
         best_v = t1 - t0;
   }

   *mismatch = 0;
   if (sc.error_seen || sv.error_seen)
   {
      fprintf(stderr, "pngrowview: bench: decode error: %s %s\n",
          sc.error, sv.error);
      state_free(&sc);
      state_free(&sv);
      return 1;
   }
   if (sc.hash != sv.hash || sc.rows_seen != sv.rows_seen)
      *mismatch = 1;

   *tcopy = best_c;
   *tview = best_v;
   state_free(&sc);
   state_free(&sv);
   return 0;
}

static int
bench(png_uint_32 width, png_uint_32 height)
{
   static const struct
   {
      const char *name;
      int         filters;
   } filter_modes[] =
   {
      { "filter-none", PNG_FILTER_NONE },
      { "filter-all",  PNG_ALL_FILTERS },
   };

   size_t f;

   printf("pngrowview: benchmark %lux%lu RGB8\n", (unsigned long)width,
       (unsigned long)height);

   for (f = 0; f < ARRAY_SIZE(filter_modes); f++)
   {
      mem_png png;
      double tc = 0, tv = 0;
      int mismatch = 0;

      if (make_png(&png, width, height, 8, PNG_COLOR_TYPE_RGB,
          PNG_INTERLACE_NONE, filter_modes[f].filters) != 0)
      {
         fprintf(stderr, "pngrowview: bench: make_png failed\n");
         return 1;
      }

      if (bench_one(&png, &tc, &tv, &mismatch) != 0)
      {
         mem_png_free(&png);
         return 1;
      }

      if (mismatch != 0)
      {
         fprintf(stderr, "pngrowview: bench: %s: output mismatch!\n",
             filter_modes[f].name);
         mem_png_free(&png);
         return 1;
      }

      printf("pngrowview: bench: %-11s copy %8.1f ms  view %8.1f ms  "
          "delta %+6.1f%%  (%lu bytes encoded)\n",
          filter_modes[f].name, tc * 1e3, tv * 1e3,
          100.0 * (tv - tc) / tc, (unsigned long)png.size);

      mem_png_free(&png);
   }

   return 0;
}

/* -------------------------------- main -------------------------------- */

int
main(int argc, char **argv)
{
   int failures = 0;

   if (argc > 1 && strcmp(argv[1], "--bench") == 0)
   {
      png_uint_32 width = 4096, height = 4096;
      if (argc > 2)
      {
         unsigned long w = 0, h = 0;
         if (sscanf(argv[2], "%lux%lu", &w, &h) != 2 ||
             w == 0 || h == 0 || w > 65535 || h > 65535)
         {
            fprintf(stderr, "usage: pngrowview --bench [WxH]\n");
            return 2;
         }
         width = (png_uint_32)w;
         height = (png_uint_32)h;
      }
      return bench(width, height);
   }

   /* Pixel-identity tests: copy-based vs zero-copy delivery. */
   failures += test_identical("rgb8-257x131", 257, 131, 8,
       PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE);
   failures += test_identical("rgba8-64x64", 64, 64, 8,
       PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE);
   failures += test_identical("rgb16-129x67", 129, 67, 16,
       PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE);
   failures += test_identical("g8-1x1", 1, 1, 8,
       PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE);
   failures += test_identical("rgb8-64x64-interlaced", 64, 64, 8,
       PNG_COLOR_TYPE_RGB, PNG_INTERLACE_ADAM7);
   failures += test_identical("ga8-31x17-interlaced", 31, 17, 8,
       PNG_COLOR_TYPE_GRAY_ALPHA, PNG_INTERLACE_ADAM7);

   /* Lifetime: reading a view after the next row callback must fail. */
   failures += test_error_mode("invalidate", MODE_INVALIDATE, 0,
       "no longer valid");

   /* Bounds: png_row_view_at at one-past-the-end must fail. */
   failures += test_error_mode("bounds", MODE_BOUNDS, 3, "out of range");

   /* Out-of-bounds writes to the viewed row must be detected. */
   failures += test_error_mode("oob-write-high", MODE_OOB_WRITE_HI, 5,
       "out-of-bounds write");
   failures += test_error_mode("oob-write-low", MODE_OOB_WRITE_LO, 5,
       "out-of-bounds write");

   if (failures != 0)
   {
      fprintf(stderr, "pngrowview: %d test(s) FAILED\n", failures);
      return 1;
   }

   printf("pngrowview: all tests passed\n");
   return 0;
}
#endif /* PNG_PROGRESSIVE_READ_SUPPORTED */
