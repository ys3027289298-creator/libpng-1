/* pngbackend.c - test the pluggable compression backend interface
 *
 * Copyright (c) 2026 Cosmin Truta
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * This program verifies that libpng's compression layer is fully
 * decoupled from zlib:
 *
 *  1. It registers a custom compression backend ("stored+puff") that does
 *     NOT use zlib: deflate is a from-scratch encoder emitting RFC 1950
 *     zlib streams made of uncompressed (stored) RFC 1951 blocks, inflate
 *     is Mark Adler's reference inflate "puff" (a one-shot decoder, driven
 *     here from buffered input), and the CRC-32 and Adler-32 checksums are
 *     computed by local table-driven implementations.
 *
 *  2. A PNG written through the custom backend is read back through the
 *     default (zlib) backend, proving that a standard decoder accepts the
 *     custom backend's output.
 *
 *  3. A PNG written through the default (zlib) backend is read back
 *     through the custom backend, proving that the read path works
 *     entirely without zlib when a custom backend is installed.
 *
 *  4. In all cases the decompressed pixel data must be byte-for-byte
 *     identical to the original image.
 *
 * The program also writes "pngbackend-expected.rows", the exact byte
 * stream that the IDAT chunks of the non-interlaced test file must
 * decompress to (filter byte 0 followed by the raw row data, per row),
 * so that an external standard decoder (e.g. Python's zlib) can
 * independently verify the custom backend's output.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"
#include "puff.h"

#define TEST_WIDTH  151
#define TEST_HEIGHT 97

static const char *file_zlib   = "pngbackend-zlib.png";
static const char *file_stored = "pngbackend-stored.png";
static const char *file_zlib_i   = "pngbackend-zlib-interlaced.png";
static const char *file_stored_i = "pngbackend-stored-interlaced.png";
static const char *file_expected = "pngbackend-expected.rows";

static int nerrors = 0;

#define CHECK(cond, msg) do \
{ \
   if (!(cond)) \
   { \
      fprintf(stderr, "pngbackend: FAIL: %s\n", msg); \
      ++nerrors; \
   } \
} while (0)

/* ---------------------------------------------------------------------------
 * CRC-32 (ITU-T V.42, the PNG/zlib CRC) - local implementation
 */
static png_uint_32
local_crc32(png_uint_32 crc, png_const_bytep data, size_t length)
{
   static png_uint_32 crc_table[256];
   static int crc_table_made = 0;

   if (!crc_table_made)
   {
      png_uint_32 c;
      int n, k;

      for (n = 0; n < 256; n++)
      {
         c = (png_uint_32)n;
         for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320U ^ (c >> 1) : c >> 1;
         crc_table[n] = c;
      }
      crc_table_made = 1;
   }

   if (data == NULL)
      return 0; /* initial value, matching the zlib crc32(0, NULL, 0) */

   crc ^= 0xffffffffU;
   while (length-- > 0)
      crc = crc_table[(crc ^ *data++) & 0xff] ^ (crc >> 8);

   return crc ^ 0xffffffffU;
}

/* ---------------------------------------------------------------------------
 * Adler-32 (RFC 1950) - local implementation
 */
static png_uint_32
local_adler32(png_uint_32 adler, png_const_bytep data, size_t length)
{
   png_uint_32 s1 = adler & 0xffff;
   png_uint_32 s2 = (adler >> 16) & 0xffff;

   while (length > 0)
   {
      /* 5552 is the largest n such that 255n(n+1)/2 + (n+1)(65520) fits in
       * 32 bits.
       */
      size_t n = length > 5552 ? 5552 : length;

      length -= n;
      while (n-- > 0)
      {
         s1 += *data++;
         s2 += s1;
      }
      s1 %= 65521;
      s2 %= 65521;
   }

   return (s2 << 16) | s1;
}

/* ---------------------------------------------------------------------------
 * Custom deflate: an RFC 1950 zlib stream of RFC 1951 stored (uncompressed)
 * blocks.  This is a complete, correct, albeit compression-free, encoder.
 */
typedef struct
{
   png_byte    zheader[2];   /* the RFC 1950 zlib header */
   unsigned int zheader_pos; /* emit position within zheader */
   png_byte    block_header[5]; /* current stored-block header */
   unsigned int block_header_pos; /* emit position within block_header */
   unsigned int block_remaining;  /* block payload bytes left to emit */
   int          block_active;     /* a block is being emitted */
   png_byte    trailer[4];   /* the Adler-32 checksum, big endian */
   unsigned int trailer_pos; /* emit position within trailer */
   png_uint_32 adler;        /* running Adler-32 of the input data */
   int         finished;     /* set once the final block has been started */
} stored_deflate_state;

static void
stored_make_block_header(stored_deflate_state *st, unsigned int len, int final)
{
   st->block_header[0] = (png_byte)(final != 0); /* BFINAL, BTYPE=00 */
   st->block_header[1] = (png_byte)(len & 0xff);
   st->block_header[2] = (png_byte)((len >> 8) & 0xff);
   st->block_header[3] = (png_byte)(~len & 0xff);
   st->block_header[4] = (png_byte)((~len >> 8) & 0xff);
   st->block_header_pos = 0;
   st->block_remaining = len;
   st->block_active = 1;
}

static int
stored_deflate_init(png_zstream *strm, int level, int method,
    int window_bits, int mem_level, int strategy)
{
   stored_deflate_state *st;
   unsigned int cmf, flg;

   (void)level; (void)mem_level; (void)strategy;

   if (method != 8) /* the only method PNG uses */
   {
      strm->msg = "stored backend: unsupported compression method";
      return PNG_Z_STREAM_ERROR;
   }

   st = strm->state;
   if (st == NULL)
   {
      st = malloc(sizeof *st);
      if (st == NULL)
      {
         strm->msg = "stored backend: out of memory";
         return PNG_Z_MEM_ERROR;
      }
      strm->state = st;
   }

   memset(st, 0, sizeof *st);
   st->adler = 1;

   /* RFC 1950 header: CMF = (CINFO << 4) | 8, FLG with FCHECK such that
    * (CMF * 256 + FLG) is a multiple of 31.  Honor the requested window
    * size (clamped to 8..15) even though stored blocks never reference the
    * window.
    */
   if (window_bits < 8 || window_bits > 15)
      window_bits = 15;
   cmf = ((unsigned int)(window_bits - 8) << 4) | 8;
   flg = (31 - ((cmf << 8) % 31)) % 31; /* FLEVEL=0, FDICT=0 */
   st->zheader[0] = (png_byte)cmf;
   st->zheader[1] = (png_byte)flg;

   return PNG_Z_OK;
}

static int
stored_deflate_reset(png_zstream *strm)
{
   stored_deflate_state *st = strm->state;

   if (st == NULL)
   {
      strm->msg = "stored backend: deflate stream not initialized";
      return PNG_Z_STREAM_ERROR;
   }

   st->zheader_pos = 0;
   st->block_header_pos = 0;
   st->block_remaining = 0;
   st->block_active = 0;
   st->trailer_pos = 0;
   st->adler = 1;
   st->finished = 0;

   return PNG_Z_OK;
}

static int
stored_deflate(png_zstream *strm, int flush)
{
   stored_deflate_state *st = strm->state;

   if (st == NULL)
   {
      strm->msg = "stored backend: deflate stream not initialized";
      return PNG_Z_STREAM_ERROR;
   }

   if (st->finished && st->trailer_pos >= 4)
   {
      strm->msg = "stored backend: deflate called after stream end";
      return PNG_Z_STREAM_ERROR;
   }

   for (;;)
   {
      /* 1. Emit the zlib header. */
      while (st->zheader_pos < 2)
      {
         if (strm->avail_out == 0)
            return PNG_Z_OK;
         *strm->next_out++ = st->zheader[st->zheader_pos++];
         --strm->avail_out;
      }

      /* 2. Start a new block if none is pending. */
      if (!st->block_active && !st->finished)
      {
         if (strm->avail_in > 0)
         {
            unsigned int len = strm->avail_in > 65535 ? 65535 : strm->avail_in;
            int final = (flush == PNG_Z_FINISH && len == strm->avail_in);

            stored_make_block_header(st, len, final);
            if (final)
               st->finished = 1;
         }
         else if (flush == PNG_Z_FINISH)
         {
            /* An empty final block terminates the stream. */
            stored_make_block_header(st, 0, 1);
            st->finished = 1;
         }
         else if (flush == PNG_Z_SYNC_FLUSH)
         {
            /* An empty non-final stored block aligns the stream to a byte
             * boundary, which is exactly what a sync flush requires.
             */
            stored_make_block_header(st, 0, 0);
         }
         else
            return PNG_Z_OK; /* need more input */
      }

      /* 3. Emit the pending block header. */
      while (st->block_active && st->block_header_pos < 5)
      {
         if (strm->avail_out == 0)
            return PNG_Z_OK;
         *strm->next_out++ = st->block_header[st->block_header_pos++];
         --strm->avail_out;
      }

      /* 4. Emit the block payload; the input bytes are the output bytes. */
      while (st->block_active && st->block_header_pos == 5 &&
          st->block_remaining > 0)
      {
         unsigned int n = st->block_remaining;

         if (n > strm->avail_in)
            n = strm->avail_in;
         if (n > strm->avail_out)
            n = strm->avail_out;
         if (n == 0)
            return PNG_Z_OK;

         memcpy(strm->next_out, strm->next_in, n);
         st->adler = local_adler32(st->adler, strm->next_in, n);

         strm->next_in += n;
         strm->avail_in -= n;
         strm->next_out += n;
         strm->avail_out -= n;
         st->block_remaining -= n;
      }

      if (st->block_active && st->block_header_pos == 5 &&
          st->block_remaining == 0)
         st->block_active = 0; /* block complete */

      /* 5. Emit the Adler-32 trailer once the final block is done. */
      if (st->finished && !st->block_active)
      {
         if (st->trailer_pos == 0)
         {
            st->trailer[0] = (png_byte)((st->adler >> 24) & 0xff);
            st->trailer[1] = (png_byte)((st->adler >> 16) & 0xff);
            st->trailer[2] = (png_byte)((st->adler >> 8) & 0xff);
            st->trailer[3] = (png_byte)(st->adler & 0xff);
         }

         while (st->trailer_pos < 4)
         {
            if (strm->avail_out == 0)
               return PNG_Z_OK;
            *strm->next_out++ = st->trailer[st->trailer_pos++];
            --strm->avail_out;
         }

         return PNG_Z_STREAM_END;
      }
   }
}

static int
stored_deflate_end(png_zstream *strm)
{
   free(strm->state);
   strm->state = NULL;
   return PNG_Z_OK;
}

/* ---------------------------------------------------------------------------
 * Custom inflate: Mark Adler's puff (reference inflate, see puff.c), driven
 * from buffered input.  puff decodes a complete raw deflate stream in one
 * shot, so this adapter accumulates the compressed data until puff can
 * decode it, then hands the decompressed bytes out incrementally.  The
 * RFC 1950 header and Adler-32 trailer are handled by the adapter itself.
 */
typedef struct
{
   png_byte *inbuf;        /* all compressed bytes received so far */
   size_t    in_len;
   size_t    in_cap;
   png_byte *outbuf;       /* the whole decompressed stream, once decoded */
   size_t    out_len;      /* valid bytes in outbuf */
   size_t    out_cap;      /* allocated size of outbuf */
   size_t    out_pos;      /* bytes already handed to libpng */
   int       decoded;      /* the whole stream has been decoded */
   int       holding;      /* a virtual input byte is being held back */
   png_byte  held;         /* the held-back byte (bookkeeping only) */
} puff_inflate_state;

static int
puff_buffer_append(puff_inflate_state *st, png_const_bytep data, size_t len)
{
   if (st->in_len + len > st->in_cap)
   {
      size_t new_cap = st->in_cap == 0 ? 16384 : st->in_cap * 2;
      png_byte *new_buf;

      while (new_cap < st->in_len + len)
         new_cap *= 2;

      new_buf = realloc(st->inbuf, new_cap);
      if (new_buf == NULL)
         return 0;

      st->inbuf = new_buf;
      st->in_cap = new_cap;
   }

   memcpy(st->inbuf + st->in_len, data, len);
   st->in_len += len;
   return 1;
}

static void
puff_inflate_reset_state(puff_inflate_state *st)
{
   st->in_len = 0;
   st->out_len = 0;
   st->out_pos = 0;
   st->decoded = 0;
   st->holding = 0;
   st->held = 0;
}

static int
puff_inflate_init(png_zstream *strm, int window_bits)
{
   puff_inflate_state *st = strm->state;

   (void)window_bits; /* puff handles any window size in the stream */

   if (st == NULL)
   {
      st = calloc(1, sizeof *st);
      if (st == NULL)
      {
         strm->msg = "puff backend: out of memory";
         return PNG_Z_MEM_ERROR;
      }
      strm->state = st;
   }

   puff_inflate_reset_state(st);
   return PNG_Z_OK;
}

static int
puff_inflate_reset(png_zstream *strm)
{
   puff_inflate_state *st = strm->state;

   if (st == NULL)
   {
      strm->msg = "puff backend: inflate stream not initialized";
      return PNG_Z_STREAM_ERROR;
   }

   puff_inflate_reset_state(st);
   return PNG_Z_OK;
}

/* Attempt to decode everything buffered so far.  Returns PNG_Z_OK if the
 * stream is complete (or more input is needed), PNG_Z_MEM_ERROR or
 * PNG_Z_DATA_ERROR on failure.
 */
static int
puff_inflate_decode(png_zstream *strm, puff_inflate_state *st)
{
   unsigned long source_len, dest_len;
   int ret;

   if (st->decoded)
      return PNG_Z_OK;

   if (st->in_len < 2)
      return PNG_Z_OK; /* wait for the zlib header */

   /* Validate the RFC 1950 zlib header. */
   {
      unsigned int cmf = st->inbuf[0];
      unsigned int flg = st->inbuf[1];

      if ((cmf & 0x0f) != 8)
      {
         strm->msg = "puff backend: unknown compression method";
         return PNG_Z_DATA_ERROR;
      }

      if ((cmf >> 4) > 7)
      {
         strm->msg = "puff backend: invalid window size";
         return PNG_Z_DATA_ERROR;
      }

      if (((cmf << 8) + flg) % 31 != 0)
      {
         strm->msg = "puff backend: incorrect header check";
         return PNG_Z_DATA_ERROR;
      }

      if ((flg & 0x20) != 0)
      {
         strm->msg = "puff backend: preset dictionary not supported";
         return PNG_Z_NEED_DICT;
      }
   }

   /* Decode the raw deflate stream that follows the 2-byte header.  puff
    * decodes from the start on every call, so this is quadratic in the
    * stream size; that is acceptable for a test backend.
    */
   for (;;)
   {
      if (st->out_cap == 0)
      {
         st->out_cap = 65536;
         st->outbuf = malloc(st->out_cap);
         if (st->outbuf == NULL)
         {
            strm->msg = "puff backend: out of memory";
            return PNG_Z_MEM_ERROR;
         }
      }

      dest_len = st->out_cap;
      source_len = st->in_len - 2;
      ret = puff(st->outbuf, &dest_len, st->inbuf + 2, &source_len);

      if (ret == 1)
      {
         /* Output space exhausted: grow and retry from scratch. */
         png_byte *new_buf;
         size_t new_cap = st->out_cap * 2;

         new_buf = realloc(st->outbuf, new_cap);
         if (new_buf == NULL)
         {
            strm->msg = "puff backend: out of memory";
            return PNG_Z_MEM_ERROR;
         }
         st->outbuf = new_buf;
         st->out_cap = new_cap;
         continue;
      }

      if (ret == 2)
         return PNG_Z_OK; /* incomplete input: wait for more */

      if (ret != 0)
      {
         strm->msg = "puff backend: invalid compressed data";
         return PNG_Z_DATA_ERROR;
      }

      /* The deflate stream is complete; source_len bytes of the buffer
       * (after the 2-byte header) were consumed.  The 4-byte Adler-32
       * trailer must follow.
       */
      st->out_len = dest_len;

      {
         size_t consumed = 2 + source_len;

         if (st->in_len - consumed < 4)
            return PNG_Z_OK; /* wait for the trailer */

         if (local_adler32(1, st->outbuf, st->out_len) !=
             ((png_uint_32)st->inbuf[consumed] << 24 |
              (png_uint_32)st->inbuf[consumed + 1] << 16 |
              (png_uint_32)st->inbuf[consumed + 2] << 8 |
              (png_uint_32)st->inbuf[consumed + 3]))
         {
            strm->msg = "puff backend: incorrect data check";
            return PNG_Z_DATA_ERROR;
         }
      }

      st->decoded = 1;
      return PNG_Z_OK;
   }
}

static int
puff_inflate(png_zstream *strm, int flush)
{
   puff_inflate_state *st = strm->state;
   size_t n;
   int ret;
   int took_input = 0;

   (void)flush; /* the adapter produces output as soon as it can */

   if (st == NULL)
   {
      strm->msg = "puff backend: inflate stream not initialized";
      return PNG_Z_STREAM_ERROR;
   }

   /* Buffer all the input offered.  Once decoding has completed, a single
    * virtual input byte is "held" instead: libpng reads more input whenever
    * avail_in reaches zero, even if the backend still has decoded output to
    * deliver, so the backend must not let avail_in hit zero while it owes
    * output.  (zlib never faces this because it consumes input only as it
    * produces output; this adapter consumes everything up front.)
    */
   if (st->holding)
   {
      /* The offered byte is our own held byte; nothing new to buffer. */
   }
   else if (strm->avail_in > 0)
   {
      if (!puff_buffer_append(st, strm->next_in, strm->avail_in))
      {
         strm->msg = "puff backend: out of memory";
         return PNG_Z_MEM_ERROR;
      }
      strm->next_in += strm->avail_in;
      strm->avail_in = 0;
      took_input = 1;
   }

   ret = puff_inflate_decode(strm, st);
   if (ret != PNG_Z_OK)
      return ret;

   /* Hand out as much of the decoded data as fits. */
   n = st->out_len - st->out_pos;
   if (n > strm->avail_out)
      n = strm->avail_out;

   if (n > 0)
   {
      memcpy(strm->next_out, st->outbuf + st->out_pos, n);
      strm->next_out += n;
      strm->avail_out -= n;
      st->out_pos += n;
   }

   if (st->decoded)
   {
      if (st->out_pos == st->out_len)
      {
         /* All output delivered; release the held byte, if any. */
         st->holding = 0;
         strm->avail_in = 0;
         return PNG_Z_STREAM_END;
      }

      /* More output to deliver: hold one virtual input byte so that libpng
       * does not try to read more compressed data.
       */
      st->holding = 1;
      strm->next_in = &st->held;
      strm->avail_in = 1;
      return PNG_Z_OK;
   }

   if (!took_input && n == 0)
   {
      /* No new input and no output to give: the stream is truncated. */
      strm->msg = "puff backend: truncated compressed data";
      return PNG_Z_BUF_ERROR;
   }

   return PNG_Z_OK;
}

static int
puff_inflate_end(png_zstream *strm)
{
   puff_inflate_state *st = strm->state;

   if (st != NULL)
   {
      free(st->inbuf);
      free(st->outbuf);
      free(st);
      strm->state = NULL;
   }

   return PNG_Z_OK;
}

/* The custom backend: no zlib anywhere. */
static const png_compression_backend stored_puff_backend =
{
   "stored+puff",
   puff_inflate_init,
   puff_inflate_reset,
   puff_inflate,
   puff_inflate_end,
   NULL, /* inflate_validate: Adler-32 is always validated */
   stored_deflate_init,
   stored_deflate_reset,
   stored_deflate,
   stored_deflate_end,
   local_crc32
};

/* ---------------------------------------------------------------------------
 * Test image generation and comparison
 */
static png_bytep
make_image(png_uint_32 *rowbytes_out)
{
   png_uint_32 rowbytes = TEST_WIDTH * 3;
   png_bytep image = malloc((size_t)rowbytes * TEST_HEIGHT);
   png_uint_32 x, y;

   if (image == NULL)
      return NULL;

   /* A deterministic pattern with both smooth and noisy regions, so that
    * the deflate streams exercise matches, literals and stored blocks.
    */
   for (y = 0; y < TEST_HEIGHT; y++)
      for (x = 0; x < TEST_WIDTH; x++)
      {
         png_bytep px = image + (size_t)y * rowbytes + x * 3;
         unsigned int v = x * 7 + y * 13;

         px[0] = (png_byte)((x + y) & 0xff);
         px[1] = (png_byte)((v ^ (x >> 3)) & 0xff);
         px[2] = (png_byte)((v * 2654435761U >> 24) & 0xff);
      }

   *rowbytes_out = rowbytes;
   return image;
}

/* Write the image to a PNG file using the given backend (NULL = default).
 * Returns 0 on success.
 */
static int
write_png(const char *path, const png_compression_backend *backend,
    png_bytep image, png_uint_32 rowbytes, int interlaced)
{
   FILE *fp = fopen(path, "wb");
   png_struct *png_ptr;
   png_info *info_ptr;
   png_bytep *rows;
   png_uint_32 y;
   int ret = 1;

   if (fp == NULL)
      return 1;

   png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   info_ptr = png_ptr ? png_create_info_struct(png_ptr) : NULL;
   rows = png_ptr ? malloc(sizeof *rows * TEST_HEIGHT) : NULL;

   if (info_ptr == NULL || rows == NULL)
      goto out;

   if (backend != NULL)
      png_set_compression_backend(png_ptr, backend);

   /* Keep the filtered data deterministic: filter type 0 (None) only. */
   png_set_filter(png_ptr, 0, PNG_FILTER_NONE);

   if (setjmp(png_jmpbuf(png_ptr)))
      goto out;

   png_init_io(png_ptr, fp);
   png_set_IHDR(png_ptr, info_ptr, TEST_WIDTH, TEST_HEIGHT, 8,
       PNG_COLOR_TYPE_RGB,
       interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
       PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
   png_write_info(png_ptr, info_ptr);

   for (y = 0; y < TEST_HEIGHT; y++)
      rows[y] = image + (size_t)y * rowbytes;

   png_write_image(png_ptr, rows);
   png_write_end(png_ptr, info_ptr);
   ret = 0;

out:
   free(rows);
   if (png_ptr != NULL)
      png_destroy_write_struct(&png_ptr, info_ptr ? &info_ptr : NULL);
   fclose(fp);
   return ret;
}

/* Read a PNG file using the given backend (NULL = default) and compare the
 * pixels byte-for-byte with the original image.  Returns 0 on success.
 */
static int
check_png(const char *path, const png_compression_backend *backend,
    png_bytep image, png_uint_32 rowbytes, const char *what)
{
   FILE *fp = fopen(path, "rb");
   png_struct *png_ptr;
   png_info *info_ptr;
   png_bytep buf = NULL;
   png_bytep *rows = NULL;
   png_uint_32 y;
   int ret = 1;

   if (fp == NULL)
   {
      fprintf(stderr, "pngbackend: cannot open %s\n", path);
      return 1;
   }

   png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   info_ptr = png_ptr ? png_create_info_struct(png_ptr) : NULL;

   if (info_ptr == NULL)
      goto out;

   if (backend != NULL)
      png_set_compression_backend(png_ptr, backend);

   if (setjmp(png_jmpbuf(png_ptr)))
      goto out;

   png_init_io(png_ptr, fp);
   png_read_info(png_ptr, info_ptr);

   CHECK(png_get_image_width(png_ptr, info_ptr) == TEST_WIDTH,
       "width mismatch");
   CHECK(png_get_image_height(png_ptr, info_ptr) == TEST_HEIGHT,
       "height mismatch");
   CHECK(png_get_rowbytes(png_ptr, info_ptr) == rowbytes, "rowbytes mismatch");

   buf = malloc((size_t)rowbytes * TEST_HEIGHT);
   rows = malloc(sizeof *rows * TEST_HEIGHT);
   if (buf == NULL || rows == NULL)
      goto out;

   for (y = 0; y < TEST_HEIGHT; y++)
      rows[y] = buf + (size_t)y * rowbytes;

   png_set_interlace_handling(png_ptr);
   png_read_update_info(png_ptr, info_ptr);
   png_read_image(png_ptr, rows);
   png_read_end(png_ptr, info_ptr);

   if (memcmp(buf, image, (size_t)rowbytes * TEST_HEIGHT) != 0)
   {
      fprintf(stderr, "pngbackend: FAIL: %s: pixel data mismatch\n", what);
      ++nerrors;
   }

   ret = 0;

out:
   free(rows);
   free(buf);
   if (png_ptr != NULL)
      png_destroy_read_struct(&png_ptr, info_ptr ? &info_ptr : NULL, NULL);
   fclose(fp);
   return ret;
}

/* Write the exact expected decompressed IDAT stream of the non-interlaced
 * test file: one filter byte (0) followed by the raw row data, per row.
 */
static int
write_expected_rows(const char *path, png_bytep image, png_uint_32 rowbytes)
{
   FILE *fp = fopen(path, "wb");
   png_uint_32 y;

   if (fp == NULL)
      return 1;

   for (y = 0; y < TEST_HEIGHT; y++)
   {
      fputc(0, fp); /* filter type None */
      fwrite(image + (size_t)y * rowbytes, 1, rowbytes, fp);
   }

   fclose(fp);
   return 0;
}

int
main(void)
{
   png_bytep image;
   png_uint_32 rowbytes;
   const png_compression_backend *backend;

   image = make_image(&rowbytes);
   if (image == NULL)
   {
      fprintf(stderr, "pngbackend: out of memory\n");
      return 1;
   }

   /* The default backend must be the zlib one. */
   {
      png_struct *png_ptr =
         png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

      backend = png_get_compression_backend(png_ptr);
      CHECK(backend != NULL && strcmp(backend->name, "zlib") == 0,
          "default backend is not zlib");

      /* Setting a NULL backend must restore the default. */
      png_set_compression_backend(png_ptr, &stored_puff_backend);
      backend = png_get_compression_backend(png_ptr);
      CHECK(backend != NULL && strcmp(backend->name, "stored+puff") == 0,
          "custom backend was not installed");
      png_set_compression_backend(png_ptr, NULL);
      backend = png_get_compression_backend(png_ptr);
      CHECK(backend != NULL && strcmp(backend->name, "zlib") == 0,
          "NULL backend did not restore zlib");

      png_destroy_read_struct(&png_ptr, NULL, NULL);
   }

   /* Write the test files: with the default (zlib) backend and with the
   * custom backend, both non-interlaced and interlaced.
   */
   CHECK(write_png(file_zlib, NULL, image, rowbytes, 0) == 0,
       "write with zlib backend failed");
   CHECK(write_png(file_stored, &stored_puff_backend, image, rowbytes, 0) == 0,
       "write with custom backend failed");
   CHECK(write_png(file_zlib_i, NULL, image, rowbytes, 1) == 0,
       "interlaced write with zlib backend failed");
   CHECK(write_png(file_stored_i, &stored_puff_backend, image, rowbytes, 1)
       == 0, "interlaced write with custom backend failed");
   CHECK(write_expected_rows(file_expected, image, rowbytes) == 0,
       "could not write expected rows");

   /* Read back every combination; the pixel data must always be identical
    * to the original image.
    */
   CHECK(check_png(file_zlib, NULL, image, rowbytes,
       "zlib file / zlib backend") == 0, "read failed");
   CHECK(check_png(file_stored, NULL, image, rowbytes,
       "custom file / zlib backend (standard decoder acceptance)") == 0,
       "read failed");
   CHECK(check_png(file_zlib, &stored_puff_backend, image, rowbytes,
       "zlib file / custom backend") == 0, "read failed");
   CHECK(check_png(file_stored, &stored_puff_backend, image, rowbytes,
       "custom file / custom backend") == 0, "read failed");
   CHECK(check_png(file_zlib_i, NULL, image, rowbytes,
       "interlaced zlib file / zlib backend") == 0, "read failed");
   CHECK(check_png(file_stored_i, NULL, image, rowbytes,
       "interlaced custom file / zlib backend") == 0, "read failed");
   CHECK(check_png(file_zlib_i, &stored_puff_backend, image, rowbytes,
       "interlaced zlib file / custom backend") == 0, "read failed");
   CHECK(check_png(file_stored_i, &stored_puff_backend, image, rowbytes,
       "interlaced custom file / custom backend") == 0, "read failed");

   free(image);

   if (nerrors != 0)
   {
      fprintf(stderr, "pngbackend: FAIL (%d error(s))\n", nerrors);
      return 1;
   }

   printf("pngbackend: PASS\n");
   return 0;
}
