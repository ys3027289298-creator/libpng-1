/* webpread.c
 *
 * Copyright (c) 2026 libpng contributors
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * WebP reading adapter for the libpng progressive row-callback interface.
 * See webpread.h for the contract and README.md for the design notes.
 *
 * Implementation overview:
 *
 *   1. Sniffing: the first 12 bytes decide PNG passthrough vs WebP decode.
 *   2. WebP container: a byte-at-a-time RIFF chunk walk.  Only the VP8L
 *      payload is buffered (bounded by WEBP_ADAPT_MAX_INPUT); every other
 *      chunk is skipped or rejected with an explicit status code.
 *   3. VP8L decoder: a self-contained lossless decoder per RFC 9649,
 *      supporting the full entropy layer (simple and normal canonical
 *      prefix codes, LZ77 with the 2-D distance mapping, color cache) and
 *      the subtract-green transform.  Predictor, color and color-indexing
 *      transforms, meta prefix codes, animation and lossy VP8 are reported
 *      as explicit UNSUPPORTED_* statuses.
 *   4. PNG synthesis: after a fully successful decode the pixels are
 *      emitted as a PNG datastream (8-bit RGBA, filter 0) through
 *      png_process_data(), so the application's row callbacks fire with
 *      unmodified libpng semantics.  Nothing is fed to the PNG state
 *      machine unless the whole WebP stream decoded cleanly.
 */
#include "webpread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include <zlib.h>

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

static png_uint_32
le32(const png_byte *p)
{
   return (png_uint_32)p[0] | ((png_uint_32)p[1] << 8) |
      ((png_uint_32)p[2] << 16) | ((png_uint_32)p[3] << 24);
}

static void
be32(png_byte *p, png_uint_32 v)
{
   p[0] = (png_byte)(v >> 24);
   p[1] = (png_byte)(v >> 16);
   p[2] = (png_byte)(v >> 8);
   p[3] = (png_byte)v;
}

/* ------------------------------------------------------------------ */
/* Bit reader: bytes in stream order, bits LSB-first (RFC 9649 3.2)    */
/* ------------------------------------------------------------------ */

typedef struct
{
   const png_byte *data;
   size_t          size;
   size_t          pos;        /* next byte to load              */
   png_uint_32     bitbuf;     /* bits not yet consumed          */
   int             bitcnt;     /* number of valid bits in bitbuf */
   int             error;      /* set on read past end           */
} bit_reader;

static png_uint_32
br_read(bit_reader *br, int nbits)   /* nbits in [0..24] */
{
   png_uint_32 v;

   if (nbits == 0)
      return 0;

   while (br->bitcnt < nbits)
   {
      if (br->pos >= br->size)
      {
         br->error = 1;
         return 0;
      }
      br->bitbuf |= (png_uint_32)br->data[br->pos++] << br->bitcnt;
      br->bitcnt += 8;
   }

   v = br->bitbuf & (((png_uint_32)1 << nbits) - 1);
   br->bitbuf >>= nbits;
   br->bitcnt -= nbits;
   return v;
}

/* ------------------------------------------------------------------ */
/* Canonical prefix codes (RFC 9649 3.7)                               */
/*                                                                     */
/* Codes are stored as per-length counts plus the symbol list sorted   */
/* by (code length, symbol value); decoding walks one bit at a time.   */
/* A code with exactly one used symbol consumes zero bits per symbol   */
/* (RFC 9649 3.7.2.1: a single leaf node is a complete tree).          */
/* ------------------------------------------------------------------ */

#define MAX_CODE_BITS 15

typedef struct
{
   png_uint_16  count[MAX_CODE_BITS + 1];
   png_uint_16 *symbol;     /* NULL iff is_single            */
   png_uint_16  single;
   int          is_single;
} prefix_code;

static void
prefix_free(prefix_code *pc)
{
   free(pc->symbol);
   pc->symbol = NULL;
}

/* Returns 0 on success, <0 on invalid code. */
static int
prefix_build(prefix_code *pc, const png_byte *lengths, int n)
{
   int i, len, nused = 0, last = 0;

   memset(pc->count, 0, sizeof pc->count);
   pc->symbol = NULL;
   pc->is_single = 0;

   for (i = 0; i < n; i++)
   {
      if (lengths[i] > MAX_CODE_BITS)
         return -1;
      pc->count[lengths[i]]++;
      if (lengths[i] != 0)
      {
         nused++;
         last = i;
      }
   }
   pc->count[0] = 0;

   if (nused == 0)
      return -1;                 /* empty code: invalid */

   if (nused == 1)                /* single leaf: zero bits per symbol */
   {
      pc->is_single = 1;
      pc->single = (png_uint_16)last;
      memset(pc->count, 0, sizeof pc->count);
      return 0;
   }

   /* The tree must be complete (RFC 9649 3.7.2.1). */
   {
      int left = 1;

      for (len = 1; len <= MAX_CODE_BITS; len++)
      {
         left <<= 1;
         left -= pc->count[len];
         if (left < 0)
            return -1;           /* over-subscribed */
      }
      if (left != 0)
         return -1;              /* incomplete */
   }

   pc->symbol = (png_uint_16 *)malloc((size_t)nused * sizeof(png_uint_16));
   if (pc->symbol == NULL)
      return -2;                 /* OOM */

   {
      int offs[MAX_CODE_BITS + 1];

      offs[1] = 0;
      for (len = 1; len < MAX_CODE_BITS; len++)
         offs[len + 1] = offs[len] + pc->count[len];

      for (i = 0; i < n; i++)
         if (lengths[i] != 0)
            pc->symbol[offs[lengths[i]]++] = (png_uint_16)i;
   }
   return 0;
}

/* Returns the symbol, or -1 on bitstream error. */
static int
prefix_decode(bit_reader *br, const prefix_code *pc)
{
   int len, code = 0, first = 0, index = 0;

   if (pc->is_single)
      return pc->single;

   for (len = 1; len <= MAX_CODE_BITS; len++)
   {
      int count;

      code |= (int)br_read(br, 1);
      if (br->error)
         return -1;
      count = pc->count[len];
      if (code - first < count)
         return pc->symbol[index + (code - first)];
      index += count;
      first = (first + count) << 1;
      code <<= 1;
   }
   return -1;
}

/* ------------------------------------------------------------------ */
/* LZ77 prefix coding (RFC 9649 3.6.2.2)                               */
/* ------------------------------------------------------------------ */

static int
lz77_value(bit_reader *br, int prefix)
{
   int extra_bits, offset;

   if (prefix < 4)
      return prefix + 1;
   extra_bits = (prefix - 2) >> 1;
   offset = (2 + (prefix & 1)) << extra_bits;
   return offset + (int)br_read(br, extra_bits) + 1;
}

/* Distance codes 1..120: 2-D neighborhood offsets (RFC 9649 Figure 20). */
static const png_int_16 dist_map[120][2] =
{
   { 0, 1}, { 1, 0}, { 1, 1}, {-1, 1}, { 0, 2}, { 2, 0}, { 1, 2},
   {-1, 2}, { 2, 1}, {-2, 1}, { 2, 2}, {-2, 2}, { 0, 3}, { 3, 0},
   { 1, 3}, {-1, 3}, { 3, 1}, {-3, 1}, { 2, 3}, {-2, 3}, { 3, 2},
   {-3, 2}, { 0, 4}, { 4, 0}, { 1, 4}, {-1, 4}, { 4, 1}, {-4, 1},
   { 3, 3}, {-3, 3}, { 2, 4}, {-2, 4}, { 4, 2}, {-4, 2}, { 0, 5},
   { 3, 4}, {-3, 4}, { 4, 3}, {-4, 3}, { 5, 0}, { 1, 5}, {-1, 5},
   { 5, 1}, {-5, 1}, { 2, 5}, {-2, 5}, { 5, 2}, {-5, 2}, { 4, 4},
   {-4, 4}, { 3, 5}, {-3, 5}, { 5, 3}, {-5, 3}, { 0, 6}, { 6, 0},
   { 1, 6}, {-1, 6}, { 6, 1}, {-6, 1}, { 2, 6}, {-2, 6}, { 6, 2},
   {-6, 2}, { 4, 5}, {-4, 5}, { 5, 4}, {-5, 4}, { 3, 6}, {-3, 6},
   { 6, 3}, {-6, 3}, { 0, 7}, { 7, 0}, { 1, 7}, {-1, 7}, { 5, 5},
   {-5, 5}, { 7, 1}, {-7, 1}, { 4, 6}, {-4, 6}, { 6, 4}, {-6, 4},
   { 2, 7}, {-2, 7}, { 7, 2}, {-7, 2}, { 3, 7}, {-3, 7}, { 7, 3},
   {-7, 3}, { 5, 6}, {-5, 6}, { 6, 5}, {-6, 5}, { 8, 0}, { 4, 7},
   {-4, 7}, { 7, 4}, {-7, 4}, { 8, 1}, { 8, 2}, { 6, 6}, {-6, 6},
   { 8, 3}, { 5, 7}, {-5, 7}, { 7, 5}, {-7, 5}, { 8, 4}, { 6, 7},
   {-6, 7}, { 7, 6}, {-7, 6}, { 8, 5}, { 7, 7}, {-7, 7}, { 8, 6},
   { 8, 7}
};

/* ------------------------------------------------------------------ */
/* Adapter object                                                      */
/* ------------------------------------------------------------------ */

/* WebP container parser states. */
enum
{
   WS_CHUNK_HEADER,   /* expecting an 8-byte chunk header       */
   WS_VP8X,           /* buffering the 10-byte VP8X payload     */
   WS_VP8L,           /* buffering the VP8L payload             */
   WS_SKIP,           /* skipping an uninteresting chunk        */
   WS_PAD,            /* skipping the odd-size pad byte         */
   WS_DONE            /* RIFF body fully consumed               */
};

struct webp_adapt
{
   png_structp png_ptr;
   png_infop   info_ptr;

   int         status;          /* enum webp_adapt_status, sticky */
   char        msg[200];        /* detail for status              */
   int         format;          /* enum webp_adapt_format         */

   /* Sniff buffer (first 12 bytes). */
   png_byte    sniff[12];
   size_t      sniff_len;
   int         sniff_done;

   /* RIFF/WEBP container state. */
   int         wstate;
   png_uint_32 riff_left;       /* bytes left in the RIFF body    */
   png_byte    chdr[8];         /* chunk header accumulator       */
   size_t      chdr_len;
   png_byte    fourcc[4];
   png_uint_32 chunk_left;      /* payload bytes left in chunk    */
   int         chunk_pad;       /* pad byte follows the payload   */
   png_byte    vp8x[10];        /* VP8X payload accumulator       */
   size_t      vp8x_len;
   png_byte   *vp8l;            /* VP8L payload accumulator       */
   size_t      vp8l_have;
   size_t      vp8l_size;
   int         got_image;       /* a VP8L image decoded cleanly   */
};

const char *
webp_adapt_status_name(int status)
{
   switch (status)
   {
      case WEBP_ADAPT_OK:                      return "OK";
      case WEBP_ADAPT_TRUNCATED:               return "TRUNCATED";
      case WEBP_ADAPT_BAD_CONTAINER:           return "BAD_CONTAINER";
      case WEBP_ADAPT_BAD_BITSTREAM:           return "BAD_BITSTREAM";
      case WEBP_ADAPT_UNSUPPORTED_COMPRESSION: return "UNSUPPORTED_COMPRESSION";
      case WEBP_ADAPT_UNSUPPORTED_TRANSFORM:   return "UNSUPPORTED_TRANSFORM";
      case WEBP_ADAPT_UNSUPPORTED_FEATURE:     return "UNSUPPORTED_FEATURE";
      case WEBP_ADAPT_SIZE_LIMIT:              return "SIZE_LIMIT";
      case WEBP_ADAPT_OOM:                     return "OOM";
      case WEBP_ADAPT_INTERNAL:                return "INTERNAL";
      default:                                 return "?";
   }
}

static int
set_status(webp_adapt *a, int status, const char *fmt, ...)
{
   va_list ap;

   va_start(ap, fmt);
   vsnprintf(a->msg, sizeof a->msg, fmt, ap);
   va_end(ap);
   a->status = status;
   return status;
}

webp_adapt *
webp_adapt_create(png_structp png_ptr, png_infop info_ptr)
{
   webp_adapt *a = (webp_adapt *)calloc(1, sizeof *a);

   if (a == NULL)
      return NULL;
   a->png_ptr = png_ptr;
   a->info_ptr = info_ptr;
   a->format = WEBP_ADAPT_FMT_PENDING;
   a->wstate = WS_CHUNK_HEADER;
   return a;
}

void
webp_adapt_destroy(webp_adapt **adapt_ptr)
{
   if (adapt_ptr != NULL && *adapt_ptr != NULL)
   {
      free((*adapt_ptr)->vp8l);
      free(*adapt_ptr);
      *adapt_ptr = NULL;
   }
}

int
webp_adapt_status(const webp_adapt *a)
{
   return a->status;
}

int
webp_adapt_format(const webp_adapt *a)
{
   return a->format;
}

const char *
webp_adapt_message(const webp_adapt *a)
{
   return a->msg;
}

/* ------------------------------------------------------------------ */
/* PNG stream synthesis (WebP -> png_process_data)                     */
/* ------------------------------------------------------------------ */

#define IDAT_CHUNK_SIZE 32768

typedef struct
{
   webp_adapt *a;
   z_stream    zs;
   int         zs_ok;
   png_byte    out[16384];        /* deflate output staging       */
   png_byte    idat[IDAT_CHUNK_SIZE + 8]; /* one IDAT chunk       */
   size_t      idat_len;          /* payload bytes in idat[]      */
   png_byte   *row;               /* filter byte + RGBA row       */
} png_emitter;

static void
emit_bytes(png_emitter *e, const png_byte *p, size_t n)
{
   /* Feeds the caller's own progressive read struct; any libpng error
    * longjmps to the caller's handler, exactly as in the old pipeline.
    */
   png_process_data(e->a->png_ptr, e->a->info_ptr, (png_bytep)p, n);
}

static void
emit_chunk(png_emitter *e, const char type[4], const png_byte *data,
    size_t len)
{
   png_byte   hdr[8];
   png_byte   tail[4];
   uLong      crc;

   be32(hdr, (png_uint_32)len);
   memcpy(hdr + 4, type, 4);
   crc = crc32(0L, (const Bytef *)type, 4);
   if (len > 0)
      crc = crc32(crc, (const Bytef *)data, (uInt)len);
   be32(tail, (png_uint_32)crc);

   emit_bytes(e, hdr, 8);
   if (len > 0)
      emit_bytes(e, data, len);
   emit_bytes(e, tail, 4);
}

static void
emit_idat_flush(png_emitter *e)
{
   if (e->idat_len > 0)
   {
      emit_chunk(e, "IDAT", e->idat, e->idat_len);
      e->idat_len = 0;
   }
}

static void
emit_idat_byte(png_emitter *e, png_byte b)
{
   e->idat[e->idat_len++] = b;
   if (e->idat_len == IDAT_CHUNK_SIZE)
      emit_idat_flush(e);
}

/* Run deflate over the bytes made available in zs.next_in/avail_in and
 * pack the output into IDAT chunks.  Returns 0 on zlib failure.
 */
static int
emit_deflate(png_emitter *e, int flush)
{
   int ret;

   do
   {
      size_t produced, i;

      e->zs.next_out = e->out;
      e->zs.avail_out = (uInt)sizeof e->out;
      ret = deflate(&e->zs, flush);
      if (ret == Z_STREAM_ERROR)
         return 0;
      produced = sizeof e->out - e->zs.avail_out;
      for (i = 0; i < produced; i++)
         emit_idat_byte(e, e->out[i]);
   }
   while (e->zs.avail_out == 0);

   if (flush == Z_FINISH && ret != Z_STREAM_END)
      return 0;
   return 1;
}

/* ------------------------------------------------------------------ */
/* VP8L decoding                                                       */
/* ------------------------------------------------------------------ */

/* Read one prefix code (simple or normal) for the given alphabet size.
 * Returns WEBP_ADAPT_OK or an error status (message set).
 */
static int
read_prefix_code(bit_reader *br, prefix_code *pc, int alphabet, webp_adapt *a)
{
   png_byte *lengths;
   int       rc, build_rc;

   lengths = (png_byte *)calloc((size_t)alphabet, 1);
   if (lengths == NULL)
      return set_status(a, WEBP_ADAPT_OOM, "out of memory");

   if (br_read(br, 1))                 /* simple code length code */
   {
      int num_symbols = (int)br_read(br, 1) + 1;
      int is_first_8bits = (int)br_read(br, 1);
      int symbol0 = (int)br_read(br, is_first_8bits ? 8 : 1);

      if (symbol0 >= alphabet)
      {
         free(lengths);
         return set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
             "simple prefix code: symbol %d outside alphabet of %d",
             symbol0, alphabet);
      }
      lengths[symbol0] = 1;
      if (num_symbols == 2)
      {
         int symbol1 = (int)br_read(br, 8);

         if (symbol1 >= alphabet)
         {
            free(lengths);
            return set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
                "simple prefix code: symbol %d outside alphabet of %d",
                symbol1, alphabet);
         }
         lengths[symbol1] = 1;
      }
   }
   else                                /* normal code length code */
   {
      static const png_byte order[19] =
         { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
      png_byte   cl_lengths[19];
      prefix_code cl_code;
      int        num_code_lengths = 4 + (int)br_read(br, 4);
      int        i, symbol, prev_len, max_symbol;

      memset(cl_lengths, 0, sizeof cl_lengths);
      for (i = 0; i < num_code_lengths; i++)
         cl_lengths[order[i]] = (png_byte)br_read(br, 3);

      rc = prefix_build(&cl_code, cl_lengths, 19);
      if (rc != 0)
      {
         free(lengths);
         return set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
             "code-length prefix code is not a complete tree");
      }

      if (br_read(br, 1))             /* explicit max_symbol */
      {
         int length_nbits = 2 + 2 * (int)br_read(br, 3);

         max_symbol = 2 + (int)br_read(br, length_nbits);
         if (max_symbol > alphabet)
         {
            prefix_free(&cl_code);
            free(lengths);
            return set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
                "max_symbol %d exceeds alphabet of %d", max_symbol, alphabet);
         }
      }
      else
         max_symbol = alphabet;

      symbol = 0;
      prev_len = 8;                   /* default repeat value */
      while (symbol < alphabet)
      {
         int code_len;

         if (max_symbol-- == 0)
            break;
         code_len = prefix_decode(br, &cl_code);
         if (code_len < 0)
         {
            prefix_free(&cl_code);
            free(lengths);
            return set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
                "truncated code-length sequence");
         }
         if (code_len < 16)
         {
            lengths[symbol++] = (png_byte)code_len;
            if (code_len != 0)
               prev_len = code_len;
         }
         else
         {
            int repeat, fill;

            if (code_len == 16)
            {
               repeat = 3 + (int)br_read(br, 2);
               fill = prev_len;
            }
            else if (code_len == 17)
            {
               repeat = 3 + (int)br_read(br, 3);
               fill = 0;
            }
            else                      /* code_len == 18 */
            {
               repeat = 11 + (int)br_read(br, 7);
               fill = 0;
            }
            if (symbol + repeat > alphabet)
            {
               prefix_free(&cl_code);
               free(lengths);
               return set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
                   "code-length repeat overruns the alphabet");
            }
            while (repeat-- > 0)
               lengths[symbol++] = (png_byte)fill;
         }
      }
      prefix_free(&cl_code);
   }

   if (br->error)
   {
      free(lengths);
      return set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
          "truncated prefix code description");
   }

   build_rc = prefix_build(pc, lengths, alphabet);
   free(lengths);
   if (build_rc == -2)
      return set_status(a, WEBP_ADAPT_OOM, "out of memory");
   if (build_rc != 0)
      return set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
          "prefix code is not a complete binary tree");
   return WEBP_ADAPT_OK;
}

/* Decode the buffered VP8L payload and, only if the whole image decodes
 * cleanly, emit it as a PNG stream.  Returns a webp_adapt_status value.
 */
static int
vp8l_decode_emit(webp_adapt *a, const png_byte *payload, size_t payload_size)
{
   static const png_byte png_sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

   bit_reader  br;
   prefix_code codes[5];
   png_uint_32 width, height, npixels, pos;
   int         alpha_is_used, version, subtract_green = 0;
   int         cache_bits = 0, cache_size = 0;
   int         green_alphabet, i, status = WEBP_ADAPT_OK;
   png_uint_32 *pixels = NULL, *cache = NULL;
   png_byte   *row = NULL;
   png_emitter e;

   memset(codes, 0, sizeof codes);
   memset(&e, 0, sizeof e);
   e.a = a;

   if (payload_size < 5)
      return set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
          "VP8L chunk too small (%lu bytes)", (unsigned long)payload_size);

   if (payload[0] != 0x2f)
      return set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
          "bad VP8L signature byte 0x%02x (expected 0x2f)", payload[0]);

   br.data = payload + 1;
   br.size = payload_size - 1;
   br.pos = 0;
   br.bitbuf = 0;
   br.bitcnt = 0;
   br.error = 0;

   /* Image header: 14+14+1+3 bits (RFC 9649 3.8.1). */
   width  = br_read(&br, 14) + 1;
   height = br_read(&br, 14) + 1;
   alpha_is_used = (int)br_read(&br, 1);
   version = (int)br_read(&br, 3);
   (void)alpha_is_used;

   if (version != 0)
      return set_status(a, WEBP_ADAPT_UNSUPPORTED_FEATURE,
          "VP8L version %d (only 0 is defined)", version);

   if ((unsigned long long)width * height > WEBP_ADAPT_MAX_PIXELS)
      return set_status(a, WEBP_ADAPT_SIZE_LIMIT,
          "image %lux%lu exceeds the pixel cap of %lu",
          (unsigned long)width, (unsigned long)height,
          (unsigned long)WEBP_ADAPT_MAX_PIXELS);
   npixels = width * height;

   /* Transforms (RFC 9649 3.8.2). */
   while (br_read(&br, 1))
   {
      int type = (int)br_read(&br, 2);

      if (type == 2)
         subtract_green = 1;
      else
      {
         static const char *const names[4] =
            { "predictor", "color", "subtract-green", "color-indexing" };

         status = set_status(a, WEBP_ADAPT_UNSUPPORTED_TRANSFORM,
             "%s transform is not supported", names[type]);
         goto done;
      }
   }

   /* Color cache (RFC 9649 3.6.2.3). */
   if (br_read(&br, 1))
   {
      cache_bits = (int)br_read(&br, 4);
      if (cache_bits < 1 || cache_bits > 11)
      {
         status = set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
             "invalid color cache size bits %d (valid: 1..11)", cache_bits);
         goto done;
      }
      cache_size = 1 << cache_bits;
   }

   /* Meta prefix codes (RFC 9649 3.7.2.2). */
   if (br_read(&br, 1))
   {
      status = set_status(a, WEBP_ADAPT_UNSUPPORTED_FEATURE,
          "meta prefix codes (entropy image) are not supported");
      goto done;
   }

   /* One prefix code group: green+length+cache, R, B, A, distance. */
   green_alphabet = 256 + 24 + cache_size;
   {
      static const int alphabets[5] = { 0, 256, 256, 256, 40 };

      for (i = 0; i < 5; i++)
      {
         int alphabet = (i == 0) ? green_alphabet : alphabets[i];

         status = read_prefix_code(&br, &codes[i], alphabet, a);
         if (status != WEBP_ADAPT_OK)
            goto done;
      }
   }

   if (br.error)
   {
      status = set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
          "truncated VP8L header");
      goto done;
   }

   /* Decode the whole image before emitting anything: the PNG state
    * machine only ever sees a complete, valid stream.
    */
   pixels = (png_uint_32 *)malloc((size_t)npixels * sizeof(png_uint_32));
   cache = (png_uint_32 *)calloc(cache_size != 0 ? (size_t)cache_size : 1,
        sizeof(png_uint_32));
   if (pixels == NULL || cache == NULL)
   {
      status = set_status(a, WEBP_ADAPT_OOM, "out of memory");
      goto done;
   }

   pos = 0;
   while (pos < npixels)
   {
      int s = prefix_decode(&br, &codes[0]);

      if (s < 0)
      {
         status = set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
             "truncated image data at pixel %lu of %lu",
             (unsigned long)pos, (unsigned long)npixels);
         goto done;
      }

      if (s < 256)                    /* literal */
      {
         int r = prefix_decode(&br, &codes[1]);
         int b = prefix_decode(&br, &codes[2]);
         int al = prefix_decode(&br, &codes[3]);
         png_uint_32 px;

         if (r < 0 || b < 0 || al < 0)
         {
            status = set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
                "truncated image data at pixel %lu of %lu",
                (unsigned long)pos, (unsigned long)npixels);
            goto done;
         }
         px = ((png_uint_32)al << 24) | ((png_uint_32)r << 16) |
            ((png_uint_32)s << 8) | (png_uint_32)b;
         pixels[pos++] = px;
         if (cache_size != 0)
            cache[(0x1e35a7bdU * px) >> (32 - cache_bits)] = px;
      }
      else if (s < 256 + 24)          /* LZ77 backward reference */
      {
         int length = lz77_value(&br, s - 256);
         int dist_sym = prefix_decode(&br, &codes[4]);
         int dist_code, dist, k;

         if (dist_sym < 0 || br.error)
         {
            status = set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
                "truncated backward reference at pixel %lu",
                (unsigned long)pos);
            goto done;
         }
         dist_code = lz77_value(&br, dist_sym);
         if (br.error)
         {
            status = set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
                "truncated backward reference at pixel %lu",
                (unsigned long)pos);
            goto done;
         }

         if (dist_code > 120)
            dist = dist_code - 120;
         else
         {
            dist = dist_map[dist_code - 1][0] +
               dist_map[dist_code - 1][1] * (int)width;
            if (dist < 1)
               dist = 1;
         }

         if ((png_uint_32)dist > pos)
         {
            status = set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
                "backward reference distance %d at pixel %lu reaches "
                "before the image start", dist, (unsigned long)pos);
            goto done;
         }
         if ((png_uint_32)length > npixels - pos)
         {
            status = set_status(a, WEBP_ADAPT_BAD_BITSTREAM,
                "backward reference length %d at pixel %lu overruns "
                "the image (%lu pixels)", length, (unsigned long)pos,
                (unsigned long)npixels);
            goto done;
         }

         for (k = 0; k < length; k++)
         {
            png_uint_32 px = pixels[pos - (png_uint_32)dist];

            pixels[pos++] = px;
            if (cache_size != 0)
               cache[(0x1e35a7bdU * px) >> (32 - cache_bits)] = px;
         }
      }
      else                            /* color cache */
      {
         int idx = s - (256 + 24);

         /* idx < cache_size by construction of the green alphabet. */
         pixels[pos++] = cache[idx];
      }
   }

   /* ---- The image is fully decoded; emit it as a PNG stream. ---- */

   row = (png_byte *)malloc((size_t)width * 4 + 1);
   if (row == NULL)
   {
      status = set_status(a, WEBP_ADAPT_OOM, "out of memory");
      goto done;
   }
   e.row = row;

   if (deflateInit2(&e.zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
        Z_DEFAULT_STRATEGY) != Z_OK)
   {
      status = set_status(a, WEBP_ADAPT_OOM, "deflateInit2 failed");
      goto done;
   }
   e.zs_ok = 1;

   emit_bytes(&e, png_sig, 8);
   {
      png_byte ihdr[13];

      be32(ihdr, width);
      be32(ihdr + 4, height);
      ihdr[8] = 8;                    /* bit depth */
      ihdr[9] = 6;                    /* color type RGBA */
      ihdr[10] = 0;                   /* compression */
      ihdr[11] = 0;                   /* filter */
      ihdr[12] = 0;                   /* interlace */
      emit_chunk(&e, "IHDR", ihdr, 13);
   }

   {
      png_uint_32 x, y;

      for (y = 0; y < height; y++)
      {
         const png_uint_32 *src = pixels + (size_t)y * width;

         row[0] = 0;                 /* filter: none */
         for (x = 0; x < width; x++)
         {
            png_uint_32 px = src[x];
            png_uint_32 al = px >> 24, r = (px >> 16) & 0xff;
            png_uint_32 g = (px >> 8) & 0xff, b = px & 0xff;

            if (subtract_green)       /* RFC 9649 3.5.3 */
            {
               r = (r + g) & 0xff;
               b = (b + g) & 0xff;
            }
            row[1 + 4 * x]     = (png_byte)r;
            row[1 + 4 * x + 1] = (png_byte)g;
            row[1 + 4 * x + 2] = (png_byte)b;
            row[1 + 4 * x + 3] = (png_byte)al;
         }

         e.zs.next_in = row;
         e.zs.avail_in = (uInt)(width * 4 + 1);
         if (!emit_deflate(&e, Z_NO_FLUSH))
         {
            status = set_status(a, WEBP_ADAPT_INTERNAL, "deflate failed");
            goto done;
         }
      }
   }

   e.zs.next_in = NULL;
   e.zs.avail_in = 0;
   if (!emit_deflate(&e, Z_FINISH))
   {
      status = set_status(a, WEBP_ADAPT_INTERNAL, "deflate finish failed");
      goto done;
   }
   emit_idat_flush(&e);
   emit_chunk(&e, "IEND", NULL, 0);

done:
   if (e.zs_ok)
      deflateEnd(&e.zs);
   free(row);
   free(pixels);
   free(cache);
   for (i = 0; i < 5; i++)
      prefix_free(&codes[i]);
   return status;
}

/* ------------------------------------------------------------------ */
/* Container dispatch                                                  */
/* ------------------------------------------------------------------ */

static const png_byte png_signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

/* Decide the format once the sniff buffer holds enough bytes (or the
 * stream ended).  Anything that is not positively RIFF/WEBP is PNG as
 * far as this adapter is concerned: libpng will diagnose it, preserving
 * the old pipeline's behavior for non-PNG garbage.
 */
static void
decide_format(webp_adapt *a)
{
   a->sniff_done = 1;

   if (a->sniff_len >= 8 && memcmp(a->sniff, png_signature, 8) == 0)
   {
      a->format = WEBP_ADAPT_FMT_PNG;
   }
   else if (a->sniff_len >= 4 && memcmp(a->sniff, "RIFF", 4) == 0 &&
       (a->sniff_len < 12 || memcmp(a->sniff + 8, "WEBP", 4) == 0))
   {
      png_uint_32 riff_size;

      a->format = WEBP_ADAPT_FMT_WEBP;
      riff_size = le32(a->sniff + 4);
      if (riff_size < 4)
      {
         set_status(a, WEBP_ADAPT_BAD_CONTAINER,
             "RIFF size %lu is too small", (unsigned long)riff_size);
         return;
      }
      if (riff_size > WEBP_ADAPT_MAX_INPUT)
      {
         set_status(a, WEBP_ADAPT_SIZE_LIMIT,
             "RIFF size %lu exceeds the input cap of %lu",
             (unsigned long)riff_size, (unsigned long)WEBP_ADAPT_MAX_INPUT);
         return;
      }
      a->riff_left = riff_size - 4;   /* 'WEBP' already consumed */
   }
   else
      a->format = WEBP_ADAPT_FMT_PNG;

   if (a->format == WEBP_ADAPT_FMT_PNG && a->sniff_len > 0)
      png_process_data(a->png_ptr, a->info_ptr, a->sniff, a->sniff_len);
}

/* Consume one chunk header (8 bytes accumulated in a->chdr). */
static int
chunk_header(webp_adapt *a)
{
   png_uint_32 chunk_size = le32(a->chdr + 4);
   png_uint_32 padded = chunk_size + (chunk_size & 1);

   memcpy(a->fourcc, a->chdr, 4);

   if (padded < chunk_size || padded > a->riff_left)
      return set_status(a, WEBP_ADAPT_BAD_CONTAINER,
          "chunk '%.4s' size %lu exceeds the RIFF bounds (%lu bytes left)",
          a->fourcc, (unsigned long)chunk_size, (unsigned long)a->riff_left);

   a->chunk_left = chunk_size;
   a->chunk_pad = (int)(chunk_size & 1);

   if (memcmp(a->fourcc, "VP8 ", 4) == 0)
      return set_status(a, WEBP_ADAPT_UNSUPPORTED_COMPRESSION,
          "lossy VP8 bitstream is not supported (lossless VP8L only)");

   if (memcmp(a->fourcc, "VP8L", 4) == 0)
   {
      if (a->vp8l != NULL || a->got_image)
         return set_status(a, WEBP_ADAPT_BAD_CONTAINER,
             "duplicate VP8L chunk");
      if (chunk_size == 0)
         return set_status(a, WEBP_ADAPT_BAD_CONTAINER,
             "empty VP8L chunk");
      a->vp8l = (png_byte *)malloc(chunk_size);
      if (a->vp8l == NULL)
         return set_status(a, WEBP_ADAPT_OOM, "out of memory");
      a->vp8l_size = chunk_size;
      a->vp8l_have = 0;
      a->wstate = WS_VP8L;
      return WEBP_ADAPT_OK;
   }

   if (memcmp(a->fourcc, "VP8X", 4) == 0)
   {
      if (chunk_size != 10)
         return set_status(a, WEBP_ADAPT_BAD_CONTAINER,
             "VP8X chunk size %lu (expected 10)", (unsigned long)chunk_size);
      a->vp8x_len = 0;
      a->wstate = WS_VP8X;
      return WEBP_ADAPT_OK;
   }

   /* ICCP, EXIF, XMP, ALPH, unknown chunks: skipped.  (ALPH only occurs
    * with lossy VP8, which is rejected when its chunk arrives.)
    */
   a->wstate = WS_SKIP;
   return WEBP_ADAPT_OK;
}

/* Advance the container state machine after a chunk payload. */
static void
chunk_done(webp_adapt *a)
{
   a->wstate = a->chunk_pad ? WS_PAD : WS_CHUNK_HEADER;
}

/* If the RIFF body is fully consumed at a chunk boundary, finish. */
static int
webp_at_boundary(webp_adapt *a)
{
   if (a->wstate == WS_CHUNK_HEADER && a->riff_left == 0)
   {
      a->wstate = WS_DONE;
      if (!a->got_image)
         return set_status(a, WEBP_ADAPT_BAD_CONTAINER,
             "RIFF body ended without a decodable image chunk");
      return WEBP_ADAPT_OK;
   }
   return a->status;
}

/* Feed bytes through the WebP container parser. */
static int
webp_consume(webp_adapt *a, const png_byte *data, size_t size)
{
   while (size > 0 && a->status == WEBP_ADAPT_OK)
   {
      if (a->wstate == WS_DONE)
         return WEBP_ADAPT_OK;        /* ignore trailing bytes */

      if (webp_at_boundary(a))
         return a->status;

      switch (a->wstate)
      {
         case WS_CHUNK_HEADER:
         {
            size_t want = 8 - a->chdr_len, take = want < size ? want : size;

            memcpy(a->chdr + a->chdr_len, data, take);
            a->chdr_len += take;
            data += take;
            size -= take;
            a->riff_left -= (png_uint_32)take;
            if (a->chdr_len == 8)
            {
               int st = chunk_header(a);

               a->chdr_len = 0;
               if (st != WEBP_ADAPT_OK)
                  return st;
            }
            break;
         }

         case WS_VP8X:
         {
            size_t want = 10 - a->vp8x_len, take = want < size ? want : size;

            memcpy(a->vp8x + a->vp8x_len, data, take);
            a->vp8x_len += take;
            data += take;
            size -= take;
            a->riff_left -= (png_uint_32)take;
            a->chunk_left -= (png_uint_32)take;
            if (a->vp8x_len == 10)
            {
               int flags = a->vp8x[0];

               if (flags & 0x02)      /* ANIM */
                  return set_status(a, WEBP_ADAPT_UNSUPPORTED_FEATURE,
                      "animated WebP (ANIM flag) is not supported");
               chunk_done(a);
            }
            break;
         }

         case WS_VP8L:
         {
            size_t want = a->vp8l_size - a->vp8l_have;
            size_t take = want < size ? want : size;

            memcpy(a->vp8l + a->vp8l_have, data, take);
            a->vp8l_have += take;
            data += take;
            size -= take;
            a->riff_left -= (png_uint_32)take;
            a->chunk_left -= (png_uint_32)take;
            if (a->vp8l_have == a->vp8l_size)
            {
               int st = vp8l_decode_emit(a, a->vp8l, a->vp8l_size);

               free(a->vp8l);
               a->vp8l = NULL;
               if (st != WEBP_ADAPT_OK)
                  return st;
               a->got_image = 1;
               chunk_done(a);
            }
            break;
         }

         case WS_SKIP:
         {
            size_t take = a->chunk_left < size ? (size_t)a->chunk_left : size;

            data += take;
            size -= take;
            a->riff_left -= (png_uint_32)take;
            a->chunk_left -= (png_uint_32)take;
            if (a->chunk_left == 0)
               chunk_done(a);
            break;
         }

         case WS_PAD:
            data++;
            size--;
            a->riff_left--;
            a->chunk_pad = 0;
            a->wstate = WS_CHUNK_HEADER;
            break;

         default:
            return set_status(a, WEBP_ADAPT_INTERNAL, "bad parser state");
      }
   }
   return a->status;
}

int
webp_adapt_push(webp_adapt *a, const png_byte *data, size_t size,
    int end_of_stream)
{
   if (a->status != WEBP_ADAPT_OK)
      return a->status;

   /* Sniffing phase: gather up to 12 bytes before deciding. */
   if (!a->sniff_done)
   {
      while (a->sniff_len < 12 && size > 0)
      {
         a->sniff[a->sniff_len++] = *data++;
         size--;
      }
      if (a->sniff_len < 12 && !end_of_stream)
         return WEBP_ADAPT_OK;
      decide_format(a);
      if (a->status != WEBP_ADAPT_OK)
         return a->status;
   }

   if (a->format == WEBP_ADAPT_FMT_PNG)
   {
      if (size > 0)
         png_process_data(a->png_ptr, a->info_ptr, (png_bytep)data, size);
      return WEBP_ADAPT_OK;
   }

   if (webp_consume(a, data, size) != WEBP_ADAPT_OK)
      return a->status;

   if (webp_at_boundary(a) != WEBP_ADAPT_OK)
      return a->status;

   if (end_of_stream && a->wstate != WS_DONE)
      return set_status(a, WEBP_ADAPT_TRUNCATED,
          "stream ended with %lu RIFF bytes unread (parser state %d)",
          (unsigned long)a->riff_left, a->wstate);

   return a->status;
}
