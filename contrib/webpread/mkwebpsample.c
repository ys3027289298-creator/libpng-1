/* mkwebpsample.c
 *
 * Copyright (c) 2026 libpng contributors
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * Generate the small WebP test samples for the webpread adapter tests.
 * Everything is self-contained: a minimal VP8L *encoder* that emits
 * literal-only and LZ77/color-cache/subtract-green streams with
 * hand-assigned canonical prefix codes, plus corrupt and unsupported
 * variants.  For every valid sample a <name>.expected file records the
 * row count and the CRC32 of the final RGBA pixel data, which the test
 * driver output is checked against.
 *
 * Usage: mkwebpsample <output-directory>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

typedef unsigned char u8;
typedef unsigned int  u32;

static const char *outdir = ".";

/* ------------------------------------------------------------------ */
/* Bit writer: bytes in stream order, bits LSB-first                   */
/* ------------------------------------------------------------------ */

typedef struct
{
   u8    *buf;
   size_t len, cap;
   u32    acc;
   int    nbits;
} bitw;

static void
bw_need(bitw *w, size_t extra)
{
   if (w->len + extra > w->cap)
   {
      w->cap = (w->cap == 0) ? 4096 : w->cap;
      while (w->len + extra > w->cap)
         w->cap *= 2;
      w->buf = realloc(w->buf, w->cap);
      if (w->buf == NULL)
      {
         fprintf(stderr, "mkwebpsample: out of memory\n");
         exit(1);
      }
   }
}

/* Raw bits, LSB-first (like ReadBits in the spec). */
static void
bw_put(bitw *w, u32 v, int n)
{
   w->acc |= v << w->nbits;
   w->nbits += n;
   while (w->nbits >= 8)
   {
      bw_need(w, 1);
      w->buf[w->len++] = (u8)(w->acc & 0xff);
      w->acc >>= 8;
      w->nbits -= 8;
   }
}

/* Prefix code bits, most-significant bit first. */
static void
bw_put_code(bitw *w, u32 code, int n)
{
   int i;

   for (i = n - 1; i >= 0; i--)
      bw_put(w, (code >> i) & 1, 1);
}

static void
bw_align(bitw *w)
{
   if (w->nbits > 0)
      bw_put(w, 0, 8 - w->nbits);
}

/* ------------------------------------------------------------------ */
/* Canonical prefix codes from code lengths                            */
/* ------------------------------------------------------------------ */

static void
lengths_to_codes(const u8 *lengths, int n, u32 *codes)
{
   int count[16] = { 0 }, len, i;
   u32 next[16];

   for (i = 0; i < n; i++)
      count[lengths[i]]++;
   count[0] = 0;
   next[1] = 0;
   for (len = 1; len < 15; len++)
      next[len + 1] = (next[len] + count[len]) << 1;
   for (i = 0; i < n; i++)
      if (lengths[i] != 0)
         codes[i] = next[lengths[i]]++;
}

static const u8 cl_order[19] =
   { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

/* Emit a normal code length code for the given per-symbol lengths.
 * The code-length code itself uses equal lengths padded with dummy
 * symbols to a power of two, so it is always a complete tree.
 */
static void
emit_normal_code(bitw *w, const u8 *lengths, int alphabet)
{
   int  present[16] = { 0 };
   int  vals[16], nv = 0, v, i, s, target, L, need, ncl, maxpos;
   u8   cl[19] = { 0 };
   u32  cl_codes[19];

   for (s = 0; s < alphabet; s++)
      present[lengths[s]] = 1;
   for (v = 0; v < 16; v++)
      if (present[v])
         vals[nv++] = v;

   if (nv == 1)
      cl[vals[0]] = 1;               /* single-leaf code-length code */
   else
   {
      target = 1;
      while (target < nv)
         target <<= 1;
      for (L = 0, i = target; i > 1; i >>= 1)
         L++;
      need = target - nv;
      for (v = 1; v < 16 && need > 0; v++)   /* pad with unused values */
         if (!present[v])
         {
            vals[nv++] = v;
            present[v] = 1;
            need--;
         }
      for (i = 0; i < nv; i++)
         cl[vals[i]] = (u8)L;
   }

   maxpos = 3;
   for (i = 0; i < 19; i++)
      if (cl[cl_order[i]] != 0 && i > maxpos)
         maxpos = i;
   ncl = maxpos + 1;

   lengths_to_codes(cl, 19, cl_codes);

   bw_put(w, 0, 1);                  /* normal code length code */
   bw_put(w, (u32)(ncl - 4), 4);
   for (i = 0; i < ncl; i++)
      bw_put(w, cl[cl_order[i]], 3);
   bw_put(w, 0, 1);                  /* max_symbol = alphabet size */
   for (s = 0; s < alphabet; s++)
      bw_put_code(w, cl_codes[lengths[s]], cl[lengths[s]]);
}

static void
emit_simple1(bitw *w, int symbol)
{
   bw_put(w, 1, 1);                  /* simple */
   bw_put(w, 0, 1);                  /* 1 symbol */
   bw_put(w, 1, 1);                  /* first symbol is 8 bits */
   bw_put(w, (u32)symbol, 8);
}

static void
emit_simple2(bitw *w, int symbol0, int symbol1)
{
   bw_put(w, 1, 1);                  /* simple */
   bw_put(w, 1, 1);                  /* 2 symbols */
   bw_put(w, 1, 1);                  /* first symbol is 8 bits */
   bw_put(w, (u32)symbol0, 8);
   bw_put(w, (u32)symbol1, 8);
}

/* Assign equal code lengths to the used symbols, padded with dummy
 * symbols to a power of two (always a complete tree).  Duplicates in
 * used[] are ignored.
 */
static void
build_lengths(u8 *lengths, int alphabet, const int *used, int nused)
{
   int target = 1, L, i, s, need, ndistinct = 0;

   memset(lengths, 0, (size_t)alphabet);
   for (i = 0; i < nused; i++)
      if (lengths[used[i]] == 0)
      {
         lengths[used[i]] = 1;            /* mark */
         ndistinct++;
      }
   while (target < ndistinct)
      target <<= 1;
   for (L = 0, i = target; i > 1; i >>= 1)
      L++;
   for (s = 0; s < alphabet; s++)
      if (lengths[s] != 0)
         lengths[s] = (u8)L;
   need = target - ndistinct;
   for (s = 0; s < alphabet && need > 0; s++)
      if (lengths[s] == 0)
      {
         lengths[s] = (u8)L;
         need--;
      }
}

/* ------------------------------------------------------------------ */
/* LZ77 prefix coding helpers                                          */
/* ------------------------------------------------------------------ */

typedef struct { int code, extra, ebits; } prefix_t;

static prefix_t
value_to_prefix(int value)
{
   prefix_t p;
   int      code;

   for (code = 0; code < 40; code++)
   {
      int lo, hi;

      if (code < 4)
      {
         lo = hi = code + 1;
         p.ebits = 0;
      }
      else
      {
         int ebits = (code - 2) >> 1;
         int off = (2 + (code & 1)) << ebits;

         lo = off + 1;
         hi = off + (1 << ebits);
         p.ebits = ebits;
      }
      if (value >= lo && value <= hi)
      {
         p.code = code;
         p.extra = value - lo;
         return p;
      }
   }
   fprintf(stderr, "mkwebpsample: value %d out of range\n", value);
   exit(1);
}

/* Distance codes 1..120: 2-D neighborhood offsets (RFC 9649 Figure 20). */
static const short dist_map[120][2] =
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
/* Token stream simulation (mirrors the decoder)                       */
/* ------------------------------------------------------------------ */

typedef struct
{
   int type;                         /* 0=literal 1=copy 2=cache */
   u32 argb;                         /* literal stream pixel     */
   int len, dist_code;               /* copy                     */
   int cache_idx;                    /* cache                    */
} token;

static u32 cache_insert(u32 *cache, int cache_bits, u32 px)
{
   u32 idx = (0x1e35a7bdU * px) >> (32 - cache_bits);

   cache[idx] = px;
   return idx;
}

/* Run the token stream, producing the stream-domain pixels. */
static void
simulate(const token *tokens, int ntokens, int width, int height,
    int cache_bits, u32 *pixels)
{
   u32  cache[2048] = { 0 };
   int  cache_size = cache_bits != 0 ? 1 << cache_bits : 0;
   int  npixels = width * height, pos = 0, t, k;

   for (t = 0; t < ntokens; t++)
   {
      if (tokens[t].type == 0)
      {
         u32 px = tokens[t].argb;

         pixels[pos++] = px;
         if (cache_size != 0)
            cache_insert(cache, cache_bits, px);
      }
      else if (tokens[t].type == 1)
      {
         int dist_code = tokens[t].dist_code, dist, len = tokens[t].len;

         if (dist_code > 120)
            dist = dist_code - 120;
         else
         {
            dist = dist_map[dist_code - 1][0] +
               dist_map[dist_code - 1][1] * width;
            if (dist < 1)
               dist = 1;
         }
         if (dist > pos || len > npixels - pos)
         {
            fprintf(stderr, "mkwebpsample: bad copy token %d\n", t);
            exit(1);
         }
         for (k = 0; k < len; k++)
         {
            u32 px = pixels[pos - dist];

            pixels[pos++] = px;
            if (cache_size != 0)
               cache_insert(cache, cache_bits, px);
         }
      }
      else
      {
         u32 px = cache[tokens[t].cache_idx];

         pixels[pos++] = px;
         if (cache_size != 0)
            cache_insert(cache, cache_bits, px);
      }
   }
   if (pos != npixels)
   {
      fprintf(stderr, "mkwebpsample: token stream fills %d of %d pixels\n",
          pos, npixels);
      exit(1);
   }
}

/* ------------------------------------------------------------------ */
/* VP8L payload assembly                                               */
/* ------------------------------------------------------------------ */

/* Channel code description: either a normal code (lengths+codes) or a
 * simple 1/2-symbol code.
 */
typedef struct
{
   int  mode;                        /* 0=normal 1=simple1 2=simple2 */
   int  s0, s1;                      /* simple symbols               */
   int  alphabet;
   u8  *lengths;
   u32 *codes;
} chan;

static void
chan_emit_desc(bitw *w, chan *c)
{
   if (c->mode == 1)
      emit_simple1(w, c->s0);
   else if (c->mode == 2)
      emit_simple2(w, c->s0, c->s1);
   else
      emit_normal_code(w, c->lengths, c->alphabet);
}

static void
chan_emit_sym(bitw *w, chan *c, int sym)
{
   if (c->mode != 0)
      return;                        /* simple codes: 0 or 1 bits... */
   bw_put_code(w, c->codes[sym], c->lengths[sym]);
}

/* Simple 2-symbol codes use 1 bit per symbol; simple 1-symbol codes use
 * none.  The canonical assignment gives code 0 to the smaller symbol.
 */
static void
chan_emit_sym_simple(bitw *w, chan *c, int sym)
{
   if (c->mode == 2)
   {
      int lo = c->s0 < c->s1 ? c->s0 : c->s1;

      bw_put_code(w, sym == lo ? 0 : 1, 1);
   }
   /* mode 1: zero bits */
}

/* Build a VP8L payload from a token stream.  Channels with more than
 * two used symbols must be normal; the caller picks simple modes for
 * the rest.  Returns the payload (malloc'd) and sets *payload_size.
 */
static u8 *
build_vp8l(size_t *payload_size, int width, int height, int subtract_green,
    int cache_bits, const token *tokens, int ntokens,
    chan *green, chan *red, chan *blue, chan *alpha, chan *dist)
{
   bitw w;
   int  t;

   memset(&w, 0, sizeof w);
   bw_need(&w, 1);
   w.buf[w.len++] = 0x2f;             /* VP8L signature byte */

   bw_put(&w, (u32)(width - 1), 14);
   bw_put(&w, (u32)(height - 1), 14);
   bw_put(&w, 1, 1);                  /* alpha is used */
   bw_put(&w, 0, 3);                  /* version */

   if (subtract_green)
   {
      bw_put(&w, 1, 1);               /* transform present */
      bw_put(&w, 2, 2);               /* subtract-green */
   }
   bw_put(&w, 0, 1);                  /* no more transforms */

   if (cache_bits != 0)
   {
      bw_put(&w, 1, 1);
      bw_put(&w, (u32)cache_bits, 4);
   }
   else
      bw_put(&w, 0, 1);

   bw_put(&w, 0, 1);                  /* no meta prefix codes */

   chan_emit_desc(&w, green);
   chan_emit_desc(&w, red);
   chan_emit_desc(&w, blue);
   chan_emit_desc(&w, alpha);
   chan_emit_desc(&w, dist);

   for (t = 0; t < ntokens; t++)
   {
      const token *tk = &tokens[t];

      if (tk->type == 0)              /* literal */
      {
         u32 px = tk->argb;

         if (green->mode == 0)
            chan_emit_sym(&w, green, (int)(px >> 8) & 0xff);
         else
            chan_emit_sym_simple(&w, green, (int)(px >> 8) & 0xff);
         if (red->mode == 0)
            chan_emit_sym(&w, red, (int)(px >> 16) & 0xff);
         else
            chan_emit_sym_simple(&w, red, (int)(px >> 16) & 0xff);
         if (blue->mode == 0)
            chan_emit_sym(&w, blue, (int)px & 0xff);
         else
            chan_emit_sym_simple(&w, blue, (int)px & 0xff);
         if (alpha->mode == 0)
            chan_emit_sym(&w, alpha, (int)(px >> 24) & 0xff);
         else
            chan_emit_sym_simple(&w, alpha, (int)(px >> 24) & 0xff);
      }
      else if (tk->type == 1)         /* LZ77 copy */
      {
         prefix_t lp = value_to_prefix(tk->len);
         prefix_t dp = value_to_prefix(tk->dist_code);

         chan_emit_sym(&w, green, 256 + lp.code);
         bw_put(&w, (u32)lp.extra, lp.ebits);
         if (dist->mode == 0)
            chan_emit_sym(&w, dist, dp.code);
         else
            chan_emit_sym_simple(&w, dist, dp.code);
         bw_put(&w, (u32)dp.extra, dp.ebits);
      }
      else                            /* color cache */
         chan_emit_sym(&w, green, 256 + 24 + tk->cache_idx);
   }

   bw_align(&w);
   *payload_size = w.len;
   return w.buf;
}

/* ------------------------------------------------------------------ */
/* File writing                                                        */
/* ------------------------------------------------------------------ */

static void
put_le32(u8 *p, u32 v)
{
   p[0] = (u8)v;
   p[1] = (u8)(v >> 8);
   p[2] = (u8)(v >> 16);
   p[3] = (u8)(v >> 24);
}

static void
write_file(const char *name, const u8 *data, size_t size)
{
   char   path[1024];
   FILE  *fp;

   snprintf(path, sizeof path, "%s/%s", outdir, name);
   fp = fopen(path, "wb");
   if (fp == NULL || fwrite(data, 1, size, fp) != size || fclose(fp) != 0)
   {
      fprintf(stderr, "mkwebpsample: cannot write %s\n", path);
      exit(1);
   }
   printf("wrote %s (%lu bytes)\n", path, (unsigned long)size);
}

/* Wrap a VP8L payload in a RIFF/WEBP container, optionally preceded by
 * a VP8X chunk with the given flags (-1: no VP8X).
 */
static void
write_webp(const char *name, const u8 *vp8l, size_t vp8l_size, int vp8x_flags,
    int width, int height)
{
   u8     buf[64];
   u8    *file, *p;
   size_t vp8x_size = vp8x_flags >= 0 ? 18 : 0;
   size_t vp8l_chunk = 8 + vp8l_size + (vp8l_size & 1);
   size_t riff_size = 4 + vp8x_size + vp8l_chunk;
   size_t total = 8 + riff_size;

   file = malloc(total);
   if (file == NULL)
      exit(1);
   p = file;
   memcpy(p, "RIFF", 4);
   put_le32(p + 4, (u32)riff_size);
   p += 8;
   memcpy(p, "WEBP", 4);
   p += 4;
   if (vp8x_flags >= 0)
   {
      memcpy(p, "VP8X", 4);
      put_le32(p + 4, 10);
      p[8] = (u8)vp8x_flags;
      p[9] = p[10] = p[11] = 0;
      p[12] = (u8)((width - 1) & 0xff);
      p[13] = (u8)(((width - 1) >> 8) & 0xff);
      p[14] = (u8)(((width - 1) >> 16) & 0xff);
      p[15] = (u8)((height - 1) & 0xff);
      p[16] = (u8)(((height - 1) >> 8) & 0xff);
      p[17] = (u8)(((height - 1) >> 16) & 0xff);
      p += 18;
   }
   memcpy(p, "VP8L", 4);
   put_le32(p + 4, (u32)vp8l_size);
   p += 8;
   memcpy(p, vp8l, vp8l_size);
   p += vp8l_size;
   if (vp8l_size & 1)
      *p++ = 0;
   if ((size_t)(p - file) != total)
   {
      fprintf(stderr, "mkwebpsample: internal size mismatch\n");
      exit(1);
   }
   write_file(name, file, total);
   free(file);
   (void)buf;
}

/* Write the <name>.expected file: row count and RGBA CRC32. */
static void
write_expected(const char *name, int height, const u8 *rgba, size_t rgba_size)
{
   char   path[1024];
   FILE  *fp;
   uLong  crc = crc32(0L, Z_NULL, 0);

   crc = crc32(crc, rgba, (uInt)rgba_size);
   snprintf(path, sizeof path, "%s/%s.expected", outdir, name);
   fp = fopen(path, "w");
   if (fp == NULL)
   {
      fprintf(stderr, "mkwebpsample: cannot write %s\n", path);
      exit(1);
   }
   fprintf(fp, "rows=%d crc32=0x%08lx\n", height, (unsigned long)crc);
   fclose(fp);
   printf("wrote %s\n", path);
}

/* Convert stream-domain pixels to final RGBA bytes (applying the
 * subtract-green inverse when used) and record the expectation.
 */
static void
expect_image(const char *name, int width, int height, int subtract_green,
    const u32 *pixels)
{
   u8    *rgba = malloc((size_t)width * height * 4);
   size_t i, n = (size_t)width * height;

   if (rgba == NULL)
      exit(1);
   for (i = 0; i < n; i++)
   {
      u32 px = pixels[i];
      u32 a = px >> 24, r = (px >> 16) & 0xff, g = (px >> 8) & 0xff,
          b = px & 0xff;

      if (subtract_green)
      {
         r = (r + g) & 0xff;
         b = (b + g) & 0xff;
      }
      rgba[4 * i]     = (u8)r;
      rgba[4 * i + 1] = (u8)g;
      rgba[4 * i + 2] = (u8)b;
      rgba[4 * i + 3] = (u8)a;
   }
   write_expected(name, height, rgba, n * 4);
   free(rgba);
}

/* ------------------------------------------------------------------ */
/* Sample 1: 13x7, literals only, no transforms, no color cache        */
/* ------------------------------------------------------------------ */

#define S1_W 13
#define S1_H 7

static const u32 s1_colors[5] =        /* ARGB */
{
   0xff0a141e,                        /* A=255 R=10  G=20  B=30  */
   0xffc86432,                        /* A=255 R=200 G=100 B=50  */
   0xff0080ff,                        /* A=255 R=0   G=128 B=255 */
   0xffff0080,                        /* A=255 R=255 G=0   B=128 */
   0xc85a5a5a                         /* A=200 R=90  G=90  B=90  */
};

static u8 *
build_sample1(size_t *payload_size, u32 *pixels)
{
   token  tokens[S1_W * S1_H];
   int    x, y, ntokens = 0;
   chan   green, red, blue, alpha, dist;
   int    g_used[8], r_used[8], b_used[8], nused;
   u8     g_lengths[280], r_lengths[256], b_lengths[256];
   u32    g_codes[280], r_codes[256], b_codes[256];

   for (y = 0; y < S1_H; y++)
      for (x = 0; x < S1_W; x++)
      {
         u32 c = s1_colors[(x * 7 + y * 3) % 5];

         tokens[ntokens].type = 0;
         tokens[ntokens].argb = c;
         ntokens++;
      }
   simulate(tokens, ntokens, S1_W, S1_H, 0, pixels);

   /* Green values {20,100,128,0,90}, red {10,200,0,255,90},
    * blue {30,50,255,128,90}: 5 used symbols each, padded to 8.
    */
   {
      static const int gu[5] = { 20, 100, 128, 0, 90 };
      static const int ru[5] = { 10, 200, 0, 255, 90 };
      static const int bu[5] = { 30, 50, 255, 128, 90 };

      memcpy(g_used, gu, sizeof gu);
      memcpy(r_used, ru, sizeof ru);
      memcpy(b_used, bu, sizeof bu);
   }
   nused = 5;
   build_lengths(g_lengths, 280, g_used, nused);
   build_lengths(r_lengths, 256, r_used, nused);
   build_lengths(b_lengths, 256, b_used, nused);
   lengths_to_codes(g_lengths, 280, g_codes);
   lengths_to_codes(r_lengths, 256, r_codes);
   lengths_to_codes(b_lengths, 256, b_codes);

   green.mode = 0;
   green.alphabet = 280;
   green.lengths = g_lengths;
   green.codes = g_codes;
   red.mode = 0;
   red.alphabet = 256;
   red.lengths = r_lengths;
   red.codes = r_codes;
   blue.mode = 0;
   blue.alphabet = 256;
   blue.lengths = b_lengths;
   blue.codes = b_codes;
   alpha.mode = 2;                    /* simple 2-symbol: 255, 200 */
   alpha.s0 = 255;
   alpha.s1 = 200;
   dist.mode = 1;                     /* simple 1-symbol: 0 (unused) */
   dist.s0 = 0;

   return build_vp8l(payload_size, S1_W, S1_H, 0, 0, tokens, ntokens,
       &green, &red, &blue, &alpha, &dist);
}

/* ------------------------------------------------------------------ */
/* Sample 2: 16x8, subtract-green + color cache + LZ77                 */
/* ------------------------------------------------------------------ */

#define S2_W 16
#define S2_H 8
#define S2_CACHE_BITS 4

static const u32 s2_colors[4] =        /* ARGB, original image colors */
{
   0xff2850a0,                        /* A=255 R=40  G=80  B=160 */
   0xffc81e5a,                        /* A=255 R=200 G=30  B=90  */
   0x800afa3c,                        /* A=128 R=10  G=250 B=60  */
   0xff0000ff                         /* A=255 R=0   G=0   B=255 */
};

static u8 *
build_sample2(size_t *payload_size, u32 *pixels)
{
   token  tokens[64];
   int    ntokens = 0, i;
   u32    tcol[4];                    /* stream-domain (transformed) */
   u32    cache_state[1 << S2_CACHE_BITS] = { 0 };
   chan   green, red, blue, alpha, dist;
   u8     g_lengths[296], r_lengths[256], b_lengths[256];
   u32    g_codes[296], r_codes[256], b_codes[256];
   int    g_used[32], r_used[4], b_used[4], gn = 0;

   for (i = 0; i < 4; i++)
   {
      u32 c = s2_colors[i];
      u32 a = c >> 24, r = (c >> 16) & 0xff, g = (c >> 8) & 0xff,
          b = c & 0xff;

      tcol[i] = (a << 24) | (((r - g) & 0xff) << 16) | (g << 8) |
          ((b - g) & 0xff);
   }

   /* Pixels 0..15: literals, the four colors cycling. */
   for (i = 0; i < 16; i++)
   {
      tokens[ntokens].type = 0;
      tokens[ntokens].argb = tcol[i % 4];
      ntokens++;
   }
   /* Pixels 16..79: LZ77 copy, length 64, distance 16 (dist_code 136). */
   tokens[ntokens].type = 1;
   tokens[ntokens].len = 64;
   tokens[ntokens].dist_code = 136;
   ntokens++;
   /* Pixels 80..95: color cache hits where possible (else literals). */
   {
      /* Reproduce the cache state after the first 80 pixels. */
      u32 sim[80];
      int pos = 0;

      for (i = 0; i < 16; i++)
      {
         sim[pos] = tcol[i % 4];
         cache_insert(cache_state, S2_CACHE_BITS, sim[pos]);
         pos++;
      }
      while (pos < 80)
      {
         u32 px = sim[pos - 16];

         sim[pos] = px;
         cache_insert(cache_state, S2_CACHE_BITS, px);
         pos++;
      }
      for (i = 0; i < 16; i++)
      {
         u32 want = tcol[i % 4];
         u32 idx = (0x1e35a7bdU * want) >> (32 - S2_CACHE_BITS);

         if (cache_state[idx] == want)
         {
            tokens[ntokens].type = 2;
            tokens[ntokens].cache_idx = (int)idx;
         }
         else
         {
            tokens[ntokens].type = 0;
            tokens[ntokens].argb = want;
         }
         ntokens++;
      }
   }
   /* Pixels 96..127: LZ77 copy, length 32, dist_code 1 (LUT: one row
    * up, distance 16); overlaps its own output.
    */
   tokens[ntokens].type = 1;
   tokens[ntokens].len = 32;
   tokens[ntokens].dist_code = 1;
   ntokens++;

   simulate(tokens, ntokens, S2_W, S2_H, S2_CACHE_BITS, pixels);

   /* Symbol usage. */
   {
      int gu_green[4] = { 80, 30, 250, 0 };       /* tcol greens */
      int ru[4] = { 216, 170, 16, 0 };            /* tcol reds   */
      int bu[4] = { 80, 60, 66, 255 };            /* tcol blues  */

      for (i = 0; i < 4; i++)
      {
         g_used[gn++] = gu_green[i];
         r_used[i] = ru[i];
         b_used[i] = bu[i];
      }
   }
   g_used[gn++] = 256 + 11;           /* length 64  -> prefix 11 */
   g_used[gn++] = 256 + 9;            /* length 32  -> prefix 9  */
   for (i = 0; i < ntokens; i++)
      if (tokens[i].type == 2)
         g_used[gn++] = 256 + 24 + tokens[i].cache_idx;

   build_lengths(g_lengths, 296, g_used, gn);
   build_lengths(r_lengths, 256, r_used, 4);
   build_lengths(b_lengths, 256, b_used, 4);
   lengths_to_codes(g_lengths, 296, g_codes);
   lengths_to_codes(r_lengths, 256, r_codes);
   lengths_to_codes(b_lengths, 256, b_codes);

   green.mode = 0;
   green.alphabet = 296;
   green.lengths = g_lengths;
   green.codes = g_codes;
   red.mode = 0;
   red.alphabet = 256;
   red.lengths = r_lengths;
   red.codes = r_codes;
   blue.mode = 0;
   blue.alphabet = 256;
   blue.lengths = b_lengths;
   blue.codes = b_codes;
   alpha.mode = 2;                    /* simple 2-symbol: 255, 128 */
   alpha.s0 = 255;
   alpha.s1 = 128;
   dist.mode = 2;                     /* simple 2-symbol: 14, 0 */
   dist.s0 = 14;
   dist.s1 = 0;

   return build_vp8l(payload_size, S2_W, S2_H, 1, S2_CACHE_BITS,
       tokens, ntokens, &green, &red, &blue, &alpha, &dist);
}

/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
   u8    *vp8l1, *vp8l2;
   size_t vp8l1_size, vp8l2_size;
   u32   *pixels1, *pixels2;
   u8    *file1;
   size_t file1_size, i;
   u8     tmp[32];
   bitw   w;

   if (argc > 1)
      outdir = argv[1];

   pixels1 = malloc(S1_W * S1_H * sizeof(u32));
   pixels2 = malloc(S2_W * S2_H * sizeof(u32));
   if (pixels1 == NULL || pixels2 == NULL)
      return 1;

   /* Valid samples. */
   vp8l1 = build_sample1(&vp8l1_size, pixels1);
   write_webp("valid-literal.webp", vp8l1, vp8l1_size, -1, S1_W, S1_H);
   expect_image("valid-literal.webp", S1_W, S1_H, 0, pixels1);

   vp8l2 = build_sample2(&vp8l2_size, pixels2);
   write_webp("valid-lz77-cache.webp", vp8l2, vp8l2_size, -1, S2_W, S2_H);
   expect_image("valid-lz77-cache.webp", S2_W, S2_H, 1, pixels2);

   /* Same image in an extended (VP8X) container: must decode the same. */
   write_webp("valid-vp8x.webp", vp8l1, vp8l1_size, 0x10, S1_W, S1_H);
   expect_image("valid-vp8x.webp", S1_W, S1_H, 0, pixels1);

   /* Corrupt variants of sample 1. */
   {
      /* Rebuild the container bytes of valid-literal.webp. */
      size_t vp8l_chunk = 8 + vp8l1_size + (vp8l1_size & 1);
      size_t riff_size = 4 + vp8l_chunk;

      file1_size = 8 + riff_size;
      file1 = malloc(file1_size);
      if (file1 == NULL)
         return 1;
      memcpy(file1, "RIFF", 4);
      put_le32(file1 + 4, (u32)riff_size);
      memcpy(file1 + 8, "WEBP", 4);
      memcpy(file1 + 12, "VP8L", 4);
      put_le32(file1 + 16, (u32)vp8l1_size);
      memcpy(file1 + 20, vp8l1, vp8l1_size);
      if (vp8l1_size & 1)
         file1[20 + vp8l1_size] = 0;
   }

   write_file("corrupt-truncated.webp", file1, 40);

   {
      u8 *c = malloc(file1_size);

      memcpy(c, file1, file1_size);
      c[20] ^= 0xff;                  /* VP8L signature byte */
      write_file("corrupt-sigflip.webp", c, file1_size);

      memcpy(c, file1, file1_size);
      c[27] ^= 0xff;                  /* inside the first prefix code */
      write_file("corrupt-bitflip.webp", c, file1_size);
      free(c);
   }

   /* Unsupported: lossy VP8 chunk. */
   {
      u8 lossy[24];

      memcpy(lossy, "RIFF", 4);
      put_le32(lossy + 4, 4 + 8 + 4);
      memcpy(lossy + 8, "WEBP", 4);
      memcpy(lossy + 12, "VP8 ", 4);
      put_le32(lossy + 16, 4);
      lossy[20] = 0x30;               /* fake VP8 frame tag bytes */
      lossy[21] = 0x02;
      lossy[22] = 0x00;
      lossy[23] = 0x9d;
      write_file("unsupported-lossy.webp", lossy, sizeof lossy);
   }

   /* Unsupported: predictor transform. */
   memset(&w, 0, sizeof w);
   bw_need(&w, 1);
   w.buf[w.len++] = 0x2f;
   bw_put(&w, S1_W - 1, 14);
   bw_put(&w, S1_H - 1, 14);
   bw_put(&w, 1, 1);
   bw_put(&w, 0, 3);
   bw_put(&w, 1, 1);                  /* transform present */
   bw_put(&w, 0, 2);                  /* predictor transform */
   bw_align(&w);
   write_webp("unsupported-transform.webp", w.buf, w.len, -1, S1_W, S1_H);
   free(w.buf);

   /* Unsupported: animation flag in VP8X. */
   write_webp("unsupported-anim.webp", vp8l1, vp8l1_size, 0x02, S1_W, S1_H);

   /* Neither PNG nor WebP: old-pipeline error behavior. */
   for (i = 0; i < sizeof tmp; i++)
      tmp[i] = (u8)(i * 37 + 11);
   write_file("garbage.bin", tmp, sizeof tmp);

   write_file("empty.bin", tmp, 0);

   free(file1);
   free(vp8l1);
   free(vp8l2);
   free(pixels1);
   free(pixels2);
   return 0;
}
