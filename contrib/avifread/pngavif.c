/* pngavif.c - AVIF still-image reader for the libpng codec/processing layer
 *
 * Copyright (c) 2026 libpng contributors.
 * This code is released under the libpng license (see LICENSE.md).
 *
 * See pngavif.h for the contract.  This file contains:
 *
 *   1. a minimal ISO-BMFF/AVIF container parser (ftyp/meta/pitm/iloc/iinf/
 *      iprp/iref/mdat), pure C, no dependencies;
 *   2. a run-time dlopen() binding to libdav1d for the AV1 bitstream
 *      (soft dependency: no headers, no link-time reference; a missing
 *      decoder is a clean png_error(), not a build failure);
 *   3. conversion of 8/10-bit YUV(+alpha) planes to the libpng 16-bit
 *      big-endian row format.
 *
 * Only public libpng API is used (png_error/png_warning/png_malloc/
 * png_free), so the module can live in contrib/ and link against an
 * unmodified libpng.
 */

#include "pngavif.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dlfcn.h>

/* ------------------------------------------------------------------ */
/* small utilities                                                     */
/* ------------------------------------------------------------------ */

#define AVIF_FOURCC(a,b,c,d) \
   ((png_uint_32)(png_byte)(a) << 24 | (png_uint_32)(png_byte)(b) << 16 | \
    (png_uint_32)(png_byte)(c) << 8  | (png_uint_32)(png_byte)(d))

#define PNGAVIF_MAX_ITEMS   32
#define PNGAVIF_MAX_PROPS   64
#define PNGAVIF_MAX_EXTENTS 16

typedef struct
{
   const png_byte *cur;
   const png_byte *end;
} pngavif_br;

static png_uint_32
pngavif_rd32(png_structp png_ptr, pngavif_br *br)
{
   png_uint_32 v;
   if ((size_t)(br->end - br->cur) < 4)
      png_error(png_ptr, "AVIF: truncated file");
   v = (png_uint_32)br->cur[0] << 24 | (png_uint_32)br->cur[1] << 16 |
       (png_uint_32)br->cur[2] << 8 | (png_uint_32)br->cur[3];
   br->cur += 4;
   return v;
}

static uint64_t
pngavif_rd64(png_structp png_ptr, pngavif_br *br)
{
   uint64_t hi = pngavif_rd32(png_ptr, br);
   return hi << 32 | pngavif_rd32(png_ptr, br);
}

static unsigned int
pngavif_rd16(png_structp png_ptr, pngavif_br *br)
{
   unsigned int v;
   if ((size_t)(br->end - br->cur) < 2)
      png_error(png_ptr, "AVIF: truncated file");
   v = (unsigned int)br->cur[0] << 8 | br->cur[1];
   br->cur += 2;
   return v;
}

static unsigned int
pngavif_rd8(png_structp png_ptr, pngavif_br *br)
{
   if ((size_t)(br->end - br->cur) < 1)
      png_error(png_ptr, "AVIF: truncated file");
   return *br->cur++;
}

static void
pngavif_skip(png_structp png_ptr, pngavif_br *br, size_t n)
{
   if ((size_t)(br->end - br->cur) < n)
      png_error(png_ptr, "AVIF: truncated file");
   br->cur += n;
}

/* Read one box: returns type, and *payload spanning the box contents.
 * Advances *br past the whole box.
 */
static png_uint_32
pngavif_box(png_structp png_ptr, pngavif_br *br, pngavif_br *payload)
{
   uint64_t size;
   png_uint_32 type;
   size_t header = 8;

   if ((size_t)(br->end - br->cur) < 8)
      png_error(png_ptr, "AVIF: truncated file");

   size = pngavif_rd32(png_ptr, br);
   type = pngavif_rd32(png_ptr, br);

   if (size == 1) /* 64-bit largesize */
   {
      size = pngavif_rd64(png_ptr, br);
      header = 16;
   }
   else if (size == 0) /* extends to end of enclosing box/file */
      size = (uint64_t)(br->end - br->cur) + 8;

   if (size < header || size - header > (uint64_t)(br->end - br->cur))
      png_error(png_ptr, "AVIF: invalid box size");

   payload->cur = br->cur;
   payload->end = br->cur + (size_t)(size - header);
   br->cur = payload->end;
   return type;
}

/* ------------------------------------------------------------------ */
/* container model                                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
   uint64_t offset;
   uint64_t length;
} pngavif_extent;

typedef struct
{
   png_uint_32 id;
   png_uint_32 type;          /* item_type fourcc from infe, 0 if none */
   int      present;
   /* resolved properties (via ipma): */
   int      has_ispe;
   png_uint_32 width, height;
   int      bits;             /* pixi, 0 if absent */
   int      has_colr;
   int      colr_cp, colr_tc, colr_mc, colr_fr;
   int      is_alpha_aux;     /* auxC with the alpha URN */
   int      has_av1c;
   /* extents (from iloc): */
   pngavif_extent ext[PNGAVIF_MAX_EXTENTS];
   int      n_ext;
} pngavif_item;

typedef struct
{
   png_uint_32 type;          /* property fourcc */
   png_uint_32 w, h;          /* ispe */
   int      bits;             /* pixi */
   int      cp, tc, mc, fr;   /* colr nclx */
   int      is_alpha_urn;     /* auxC */
} pngavif_prop;

typedef struct
{
   pngavif_item items[PNGAVIF_MAX_ITEMS];
   int      n_items;
   pngavif_prop props[PNGAVIF_MAX_PROPS];
   int      n_props;          /* props are 1-based in ipma */
   png_uint_32 primary_id;
   int      have_primary;
} pngavif_meta;

static pngavif_item *
pngavif_item_for(png_structp png_ptr, pngavif_meta *m, png_uint_32 id)
{
   int i;
   for (i = 0; i < m->n_items; i++)
      if (m->items[i].present && m->items[i].id == id)
         return &m->items[i];
   if (m->n_items >= PNGAVIF_MAX_ITEMS)
      png_error(png_ptr, "AVIF: too many items");
   m->items[m->n_items].id = id;
   m->items[m->n_items].present = 1;
   return &m->items[m->n_items++];
}

/* ---- pitm ---- */
static void
pngavif_parse_pitm(png_structp png_ptr, pngavif_meta *m, pngavif_br *p)
{
   unsigned int version = pngavif_rd8(png_ptr, p);
   pngavif_skip(png_ptr, p, 3); /* flags */
   m->primary_id = version == 0 ? pngavif_rd16(png_ptr, p)
                                : pngavif_rd32(png_ptr, p);
   m->have_primary = 1;
}

/* ---- iloc ---- */
static void
pngavif_parse_iloc(png_structp png_ptr, pngavif_meta *m, pngavif_br *p)
{
   unsigned int version, flags_byte2, offset_size, length_size,
                base_offset_size, index_size, item_count, i;

   version = pngavif_rd8(png_ptr, p);
   pngavif_skip(png_ptr, p, 3); /* flags */
   flags_byte2 = pngavif_rd8(png_ptr, p);
   offset_size = flags_byte2 >> 4;
   length_size = flags_byte2 & 15;
   flags_byte2 = pngavif_rd8(png_ptr, p);
   base_offset_size = flags_byte2 >> 4;
   index_size = (version == 1 || version == 2) ? (flags_byte2 & 15) : 0;

   if (offset_size > 8 || length_size > 8 || base_offset_size > 8 ||
       index_size > 8)
      png_error(png_ptr, "AVIF: unsupported iloc field size");

   item_count = version < 2 ? pngavif_rd16(png_ptr, p)
                            : (unsigned int)pngavif_rd32(png_ptr, p);

   for (i = 0; i < item_count; i++)
   {
      png_uint_32 item_id;
      unsigned int construction_method = 0, extent_count, j;
      uint64_t base_offset = 0;
      pngavif_item *it;

      item_id = version < 2 ? pngavif_rd16(png_ptr, p)
                            : pngavif_rd32(png_ptr, p);
      if (version == 1 || version == 2)
         construction_method = pngavif_rd16(png_ptr, p) & 15;
      pngavif_skip(png_ptr, p, 2); /* data_reference_index */

      if (base_offset_size > 0)
      {
         unsigned int k;
         for (k = 0; k < base_offset_size; k++)
            base_offset = base_offset << 8 | pngavif_rd8(png_ptr, p);
      }

      if (construction_method != 0)
         png_error(png_ptr, "AVIF: unsupported iloc construction method");

      it = pngavif_item_for(png_ptr, m, item_id);
      extent_count = pngavif_rd16(png_ptr, p);

      for (j = 0; j < extent_count; j++)
      {
         uint64_t extent_offset = 0, extent_length = 0;
         unsigned int k;

         if ((version == 1 || version == 2) && index_size > 0)
            pngavif_skip(png_ptr, p, index_size); /* extent_index */

         for (k = 0; k < offset_size; k++)
            extent_offset = extent_offset << 8 | pngavif_rd8(png_ptr, p);
         for (k = 0; k < length_size; k++)
            extent_length = extent_length << 8 | pngavif_rd8(png_ptr, p);

         if (it->n_ext >= PNGAVIF_MAX_EXTENTS)
            png_error(png_ptr, "AVIF: too many extents");
         it->ext[it->n_ext].offset = base_offset + extent_offset;
         it->ext[it->n_ext].length = extent_length;
         it->n_ext++;
      }
   }
}

/* ---- iinf / infe ---- */
static void
pngavif_parse_iinf(png_structp png_ptr, pngavif_meta *m, pngavif_br *p)
{
   unsigned int version = pngavif_rd8(png_ptr, p);
   unsigned int entry_count, i;
   pngavif_skip(png_ptr, p, 3); /* flags */
   entry_count = version == 0 ? pngavif_rd16(png_ptr, p)
                              : (unsigned int)pngavif_rd32(png_ptr, p);

   for (i = 0; i < entry_count; i++)
   {
      pngavif_br infe;
      png_uint_32 type = pngavif_box(png_ptr, p, &infe);
      unsigned int v;
      png_uint_32 item_id, item_type;
      pngavif_item *it;

      if (type != AVIF_FOURCC('i','n','f','e'))
         continue; /* unknown entry: ignore */

      v = pngavif_rd8(png_ptr, &infe);
      pngavif_skip(png_ptr, &infe, 3); /* flags */
      if (v < 2)
         png_error(png_ptr, "AVIF: unsupported infe version");

      item_id = v == 2 ? pngavif_rd16(png_ptr, &infe)
                       : pngavif_rd32(png_ptr, &infe);
      pngavif_skip(png_ptr, &infe, 2); /* item_protection_index */
      item_type = pngavif_rd32(png_ptr, &infe);
      /* item_name etc. follow; not needed */

      it = pngavif_item_for(png_ptr, m, item_id);
      it->type = item_type;
   }
}

/* ---- iprp: ipco + ipma ---- */
static void
pngavif_parse_ipco(png_structp png_ptr, pngavif_meta *m, pngavif_br *p)
{
   while (p->cur < p->end)
   {
      pngavif_br prop;
      png_uint_32 type = pngavif_box(png_ptr, p, &prop);
      pngavif_prop *pr;

      if (m->n_props >= PNGAVIF_MAX_PROPS)
         png_error(png_ptr, "AVIF: too many item properties");
      pr = &m->props[m->n_props++];
      memset(pr, 0, sizeof *pr);
      pr->type = type;

      if (type == AVIF_FOURCC('i','s','p','e'))
      {
         pngavif_skip(png_ptr, &prop, 4); /* version+flags */
         pr->w = pngavif_rd32(png_ptr, &prop);
         pr->h = pngavif_rd32(png_ptr, &prop);
      }
      else if (type == AVIF_FOURCC('p','i','x','i'))
      {
         unsigned int nch, k;
         pngavif_skip(png_ptr, &prop, 4); /* version+flags */
         nch = pngavif_rd8(png_ptr, &prop);
         pr->bits = nch > 0 ? pngavif_rd8(png_ptr, &prop) : 0;
         for (k = 1; k < nch; k++)
            pngavif_skip(png_ptr, &prop, 1);
      }
      else if (type == AVIF_FOURCC('a','v','1','C'))
      {
         /* AV1 codec configuration: the decoder re-reads the sequence
          * header from the bitstream itself, so the contents are not
          * needed here; presence is what matters.
          */
      }
      else if (type == AVIF_FOURCC('c','o','l','r'))
      {
         png_uint_32 colour_type = pngavif_rd32(png_ptr, &prop);
         if (colour_type == AVIF_FOURCC('n','c','l','x'))
         {
            pr->cp = pngavif_rd16(png_ptr, &prop);
            pr->tc = pngavif_rd16(png_ptr, &prop);
            pr->mc = pngavif_rd16(png_ptr, &prop);
            pr->fr = (pngavif_rd8(png_ptr, &prop) & 0x80) != 0;
         }
         /* rICC/prof: ICC profiles are not interpreted by this module */
      }
      else if (type == AVIF_FOURCC('a','u','x','C'))
      {
         static const char alpha_urn[] =
            "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha";
         pngavif_skip(png_ptr, &prop, 4); /* version+flags */
         if ((size_t)(prop.end - prop.cur) >= sizeof alpha_urn - 1 &&
             memcmp(prop.cur, alpha_urn, sizeof alpha_urn - 1) == 0)
            pr->is_alpha_urn = 1;
      }
      /* all other properties (clap, irot, imir, pasp, ...) are ignored:
       * the pixel matrix is delivered as coded.
       */
   }
}

static void
pngavif_parse_ipma(png_structp png_ptr, pngavif_meta *m, pngavif_br *p)
{
   unsigned int version = pngavif_rd8(png_ptr, p);
   unsigned int flags, entry_count, i, wide;

   flags = pngavif_rd8(png_ptr, p) << 16;
   flags |= pngavif_rd8(png_ptr, p) << 8;
   flags |= pngavif_rd8(png_ptr, p);
   wide = (flags & 1) != 0; /* 15-bit property indices */

   entry_count = (unsigned int)pngavif_rd32(png_ptr, p);

   for (i = 0; i < entry_count; i++)
   {
      png_uint_32 item_id = version < 1 ? pngavif_rd16(png_ptr, p)
                                        : pngavif_rd32(png_ptr, p);
      unsigned int assoc_count = pngavif_rd8(png_ptr, p);
      pngavif_item *it = pngavif_item_for(png_ptr, m, item_id);
      unsigned int j;

      for (j = 0; j < assoc_count; j++)
      {
         unsigned int index;
         if (wide)
            index = pngavif_rd16(png_ptr, p) & 0x7fff;
         else
            index = pngavif_rd8(png_ptr, p) & 0x7f;

         if (index == 0 || index > (unsigned int)m->n_props)
            png_error(png_ptr, "AVIF: invalid property index");

         {
            const pngavif_prop *pr = &m->props[index - 1];
            if (pr->type == AVIF_FOURCC('i','s','p','e'))
            {
               it->has_ispe = 1;
               it->width = pr->w;
               it->height = pr->h;
            }
            else if (pr->type == AVIF_FOURCC('p','i','x','i'))
               it->bits = pr->bits;
            else if (pr->type == AVIF_FOURCC('a','v','1','C'))
               it->has_av1c = 1;
            else if (pr->type == AVIF_FOURCC('c','o','l','r'))
            {
               it->has_colr = 1;
               it->colr_cp = pr->cp;
               it->colr_tc = pr->tc;
               it->colr_mc = pr->mc;
               it->colr_fr = pr->fr;
            }
            else if (pr->type == AVIF_FOURCC('a','u','x','C') &&
                     pr->is_alpha_urn)
               it->is_alpha_aux = 1;
         }
      }
   }
}

/* ---- meta ---- */
static void
pngavif_parse_meta(png_structp png_ptr, pngavif_meta *m, pngavif_br *meta)
{
   pngavif_skip(png_ptr, meta, 4); /* full-box version+flags */

   while (meta->cur < meta->end)
   {
      pngavif_br box;
      png_uint_32 type = pngavif_box(png_ptr, meta, &box);

      if (type == AVIF_FOURCC('p','i','t','m'))
         pngavif_parse_pitm(png_ptr, m, &box);
      else if (type == AVIF_FOURCC('i','l','o','c'))
         pngavif_parse_iloc(png_ptr, m, &box);
      else if (type == AVIF_FOURCC('i','i','n','f'))
         pngavif_parse_iinf(png_ptr, m, &box);
      else if (type == AVIF_FOURCC('i','p','r','p'))
      {
         while (box.cur < box.end)
         {
            pngavif_br sub;
            png_uint_32 stype = pngavif_box(png_ptr, &box, &sub);
            if (stype == AVIF_FOURCC('i','p','c','o'))
               pngavif_parse_ipco(png_ptr, m, &sub);
            else if (stype == AVIF_FOURCC('i','p','m','a'))
               pngavif_parse_ipma(png_ptr, m, &sub);
         }
      }
      /* hdlr, iref, dinf, ... : not needed for a single still image.
       * (The alpha item is located via its auxC property, so walking the
       * iref/auxl references is unnecessary.)
       */
   }
}

/* ------------------------------------------------------------------ */
/* libdav1d run-time binding                                           */
/* ------------------------------------------------------------------ */

/* Only prefixes of the real dav1d structures are declared here; every
 * object is kept in an oversized buffer so the layout can grow without
 * breaking the binding.  Field order below matches dav1d 1.x public
 * headers (include/dav1d/{dav1d,picture,data}.h), which only append
 * members between releases.
 */
typedef struct
{
   void *seq_hdr;
   void *frame_hdr;
   void *data[3];        /* Y, U, V planes */
   ptrdiff_t stride[2];  /* luma, chroma (bytes) */
   struct
   {
      int w, h;
      int layout;        /* 0=I400 1=I420 2=I422 3=I444 */
      int bpc;           /* bits per component: 8 or 10 */
   } p;
   /* Dav1dDataProps and the remaining members are not used. */
} pngavif_d1_picture;

#define PNGAVIF_D1_SETTINGS_BUFSIZE 256
#define PNGAVIF_D1_DATA_BUFSIZE     128
#define PNGAVIF_D1_PICTURE_BUFSIZE  512

typedef struct
{
   void *handle;
   const char *(*version)(void);
   void (*default_settings)(void *settings);
   int (*open)(void **ctx_out, const void *settings);
   int (*send_data)(void *ctx, void *data);
   int (*get_picture)(void *ctx, void *picture);
   void (*picture_unref)(void *picture);
   void (*close)(void **ctx_out);
   png_byte *(*data_create)(void *data, size_t sz);
} pngavif_d1;

static void
pngavif_d1_load(png_structp png_ptr, pngavif_d1 *d)
{
   static const char *const sonames[] =
   {
      "libdav1d.so.7", "libdav1d.so.6", "libdav1d.so.5", "libdav1d.so"
   };
   const char *override_path;
   size_t i;

   memset(d, 0, sizeof *d);

   /* Test hook + documentation: simulate a system without the decoder. */
   if (getenv("PNGAVIF_NO_DAV1D") != NULL)
      png_error(png_ptr, "AVIF: AV1 decoder unavailable "
                "(libdav1d disabled by PNGAVIF_NO_DAV1D)");

   override_path = getenv("PNGAVIF_DAV1D");
   if (override_path != NULL)
      d->handle = dlopen(override_path, RTLD_NOW | RTLD_LOCAL);

   for (i = 0; d->handle == NULL && i < sizeof sonames / sizeof *sonames; i++)
      d->handle = dlopen(sonames[i], RTLD_NOW | RTLD_LOCAL);

   if (d->handle == NULL)
      png_error(png_ptr, "AVIF: AV1 decoder unavailable "
                "(libdav1d not found; set PNGAVIF_DAV1D to its path)");

#define PNGAVIF_D1_SYM(name)                                                \
   do {                                                                     \
      *(void **)(&d->name) = dlsym(d->handle, "dav1d_" #name);              \
      if (d->name == NULL)                                                  \
         png_error(png_ptr, "AVIF: libdav1d is missing dav1d_" #name);      \
   } while (0)

   PNGAVIF_D1_SYM(version);
   PNGAVIF_D1_SYM(default_settings);
   PNGAVIF_D1_SYM(open);
   PNGAVIF_D1_SYM(send_data);
   PNGAVIF_D1_SYM(get_picture);
   PNGAVIF_D1_SYM(picture_unref);
   PNGAVIF_D1_SYM(close);
   PNGAVIF_D1_SYM(data_create);
#undef PNGAVIF_D1_SYM
}

/* Decode one AV1 item bitstream into pic_buf (a pngavif_d1_picture
 * prefix).  On success *ctx_out holds the open decoder (the caller must
 * unref the picture and close the context after consuming the pixels).
 */
static void
pngavif_d1_decode(png_structp png_ptr, const pngavif_d1 *d,
    const png_byte *bitstream, size_t bitstream_size,
    void **ctx_out, png_byte *pic_buf, pngavif_d1_picture **pic_out)
{
   png_byte settings[PNGAVIF_D1_SETTINGS_BUFSIZE];
   png_byte data[PNGAVIF_D1_DATA_BUFSIZE];
   png_byte eos[PNGAVIF_D1_DATA_BUFSIZE];
   void *ctx = NULL;
   png_byte *dst;
   int rc, got = 0;

   memset(settings, 0, sizeof settings);
   d->default_settings(settings);
   /* Dav1dSettings field order (dav1d 1.x): n_threads, max_frame_delay,
    * apply_grain, operating_point, all_layers, frame_size_limit, ...
    * n_threads=1 and max_frame_delay=1 give the minimal, deterministic
    * memory footprint; apply_grain=0 keeps output bit-exact;
    * frame_size_limit mirrors our own pixel cap.
    */
   ((int *)settings)[0] = 1;                        /* n_threads */
   ((int *)settings)[1] = 1;                        /* max_frame_delay */
   ((int *)settings)[2] = 0;                        /* apply_grain */
   ((unsigned int *)settings)[5] = PNGAVIF_MAX_PIXELS; /* frame_size_limit */

   rc = d->open(&ctx, settings);
   if (rc != 0 || ctx == NULL)
      png_error(png_ptr, "AVIF: failed to initialize AV1 decoder");

   memset(data, 0, sizeof data);
   dst = d->data_create(data, bitstream_size);
   if (dst == NULL)
   {
      d->close(&ctx);
      png_error(png_ptr, "AVIF: out of memory");
   }
   memcpy(dst, bitstream, bitstream_size);

   for (;;)
   {
      rc = d->send_data(ctx, data);
      if (rc != -EAGAIN)
         break;
      /* Decoder output queue is full: drain one frame and retry. */
      {
         png_byte tmp[PNGAVIF_D1_PICTURE_BUFSIZE];
         memset(tmp, 0, sizeof tmp);
         if (d->get_picture(ctx, tmp) != 0)
         {
            d->close(&ctx);
            png_error(png_ptr, "AVIF: AV1 decoder congested");
         }
         d->picture_unref(tmp);
      }
   }
   if (rc != 0)
   {
      d->close(&ctx);
      png_error(png_ptr, "AVIF: AV1 bitstream rejected by decoder");
   }

   /* Signal end of stream: a Dav1dData with data == NULL. */
   memset(eos, 0, sizeof eos);
   (void)d->send_data(ctx, eos);

   for (;;)
   {
      memset(pic_buf, 0, PNGAVIF_D1_PICTURE_BUFSIZE);
      rc = d->get_picture(ctx, pic_buf);
      if (rc == 0)
      {
         if (got)
         {
            /* A still image must decode to exactly one frame. */
            d->picture_unref(pic_buf);
            png_warning(png_ptr, "AVIF: extra frames in item ignored");
            continue;
         }
         got = 1;
         break; /* keep this picture referenced */
      }
      if (rc == -EAGAIN)
         break;
      d->close(&ctx);
      png_error(png_ptr, "AVIF: AV1 decode failed (corrupt bitstream)");
   }

   if (!got)
   {
      d->close(&ctx);
      png_error(png_ptr, "AVIF: no frame decoded from item");
   }

   *ctx_out = ctx;
   *pic_out = (pngavif_d1_picture *)pic_buf;
}

/* ------------------------------------------------------------------ */
/* colour conversion (see pngavif.h for the rounding rules)            */
/* ------------------------------------------------------------------ */

png_uint_16
pngavif_scale8(unsigned int v)
{
   return (png_uint_16)((v << 8) | v); /* v * 257: exact */
}

png_uint_16
pngavif_scale10(unsigned int v)
{
   return (png_uint_16)((v * 65535u + 511u) / 1023u); /* round to nearest */
}

static png_int_32
pngavif_q16(double x)
{
   return (png_int_32)(x * 65536.0 + 0.5);
}

void
pngavif_csc_init(pngavif_csc *csc, int matrix_coefficients, int full_range,
    int bit_depth)
{
   double kr, kb, kg;
   double maxval = bit_depth == 10 ? 1023.0 : 255.0;
   double yrng, crng, cy, cc;

   memset(csc, 0, sizeof *csc);
   csc->bit_depth = bit_depth;

   if (matrix_coefficients == 0) /* identity / GBR */
   {
      csc->identity = 1;
      return;
   }

   switch (matrix_coefficients)
   {
      case 1:  kr = 0.2126; kb = 0.0722; break; /* BT.709 */
      case 4:  kr = 0.30;   kb = 0.11;   break; /* FCC */
      case 5:  /* BT.470BG */
      case 6:  kr = 0.299;  kb = 0.114;  break; /* BT.601 */
      case 7:  kr = 0.212;  kb = 0.087;  break; /* SMPTE 240M */
      case 9:  /* BT.2020 non-constant luminance */
      case 10: kr = 0.2627; kb = 0.0593; break; /* BT.2020 CL: approximated */
      default: kr = 0.2126; kb = 0.0722; break; /* unspecified: BT.709 */
   }
   kg = 1.0 - kr - kb;

   /* Studio/full swing in *source* units (exact per bit depth). */
   if (full_range)
   {
      yrng = maxval;
      crng = maxval;
      csc->yoff = 0;
   }
   else
   {
      yrng = bit_depth == 10 ? 876.0 : 219.0;
      crng = bit_depth == 10 ? 896.0 : 224.0;
      csc->yoff = bit_depth == 10 ? 64 : 16;
   }
   csc->coff = (png_int_32)((maxval + 1.0) / 2.0); /* 128 or 512 */

   cy = 65535.0 / yrng;
   cc = 65535.0 / crng;

   csc->cy  = pngavif_q16(cy);
   csc->crv = pngavif_q16(2.0 * (1.0 - kr) * cc);
   csc->cbu = pngavif_q16(2.0 * (1.0 - kb) * cc);
   csc->cgv = pngavif_q16(kr * 2.0 * (1.0 - kr) / kg * cc);
   csc->cgu = pngavif_q16(kb * 2.0 * (1.0 - kb) / kg * cc);
}

static png_uint_16
pngavif_clip16(int64_t v)
{
   if (v < 0)
      return 0;
   if (v > 65535)
      return 65535;
   return (png_uint_16)v;
}

void
pngavif_csc_apply(const pngavif_csc *csc, unsigned int y, unsigned int u,
    unsigned int v, png_uint_16 rgb[3])
{
   if (csc->identity)
   {
      rgb[0] = csc->bit_depth == 10 ? pngavif_scale10(y) : pngavif_scale8(y);
      rgb[1] = csc->bit_depth == 10 ? pngavif_scale10(u) : pngavif_scale8(u);
      rgb[2] = csc->bit_depth == 10 ? pngavif_scale10(v) : pngavif_scale8(v);
      return;
   }

   {
      int64_t yy = (int64_t)y - csc->yoff;
      int64_t uu = (int64_t)u - csc->coff;
      int64_t vv = (int64_t)v - csc->coff;
      int64_t ly = csc->cy * yy;

      rgb[0] = pngavif_clip16((ly + csc->crv * vv + 32768) >> 16);
      rgb[1] = pngavif_clip16((ly - csc->cgu * uu - csc->cgv * vv + 32768) >> 16);
      rgb[2] = pngavif_clip16((ly + csc->cbu * uu + 32768) >> 16);
   }
}

/* ------------------------------------------------------------------ */
/* plane access + row conversion                                       */
/* ------------------------------------------------------------------ */

static unsigned int
pngavif_plane_at(const pngavif_d1_picture *pic, int plane,
    png_uint_32 x, png_uint_32 y)
{
   const png_byte *row = (const png_byte *)pic->data[plane] +
                         (size_t)y * (size_t)pic->stride[plane != 0];
   if (pic->p.bpc > 8)
      return ((const png_uint_16 *)row)[x]; /* 10-bit in LSBs */
   return row[x];
}

/* ------------------------------------------------------------------ */
/* top-level decode                                                    */
/* ------------------------------------------------------------------ */

static void
pngavif_check_item(png_structp png_ptr, const pngavif_item *it,
    const char *what)
{
   if (it->type != AVIF_FOURCC('a','v','0','1'))
      png_error(png_ptr, what);
   if (!it->has_av1c)
      png_error(png_ptr, "AVIF: item has no av1C configuration");
   if (it->n_ext == 0)
      png_error(png_ptr, "AVIF: item has no data extents");
}

static void
pngavif_gather(png_structp png_ptr, const pngavif_item *it,
    const png_byte *file, size_t file_size, png_byte **out, size_t *out_size)
{
   size_t total = 0, pos = 0;
   int i;
   png_byte *buf;

   for (i = 0; i < it->n_ext; i++)
   {
      if (it->ext[i].offset > file_size ||
          it->ext[i].length > file_size - (size_t)it->ext[i].offset)
         png_error(png_ptr, "AVIF: item extent outside file");
      total += (size_t)it->ext[i].length;
   }
   if (total == 0)
      png_error(png_ptr, "AVIF: empty item");

   buf = (png_byte *)png_malloc(png_ptr, total);
   for (i = 0; i < it->n_ext; i++)
   {
      memcpy(buf + pos, file + (size_t)it->ext[i].offset,
             (size_t)it->ext[i].length);
      pos += (size_t)it->ext[i].length;
   }
   *out = buf;
   *out_size = total;
}

int
png_avif_read_memory(png_structp png_ptr, png_const_bytep data,
    size_t data_size, png_avif_info *info, png_avif_row_ptr row_fn,
    png_voidp user_data)
{
   pngavif_br top, ftyp;
   pngavif_meta meta;
   const pngavif_item *color = NULL, *alpha = NULL;
   int have_meta = 0, i;

   if (png_ptr == NULL || data == NULL || info == NULL || row_fn == NULL)
      png_error(png_ptr, "AVIF: invalid argument");

   top.cur = data;
   top.end = data + data_size;
   memset(&meta, 0, sizeof meta);

   /* ---- top-level boxes: ftyp, meta (mdat is located via iloc) ---- */
   while (top.cur < top.end)
   {
      pngavif_br box;
      png_uint_32 type = pngavif_box(png_ptr, &top, &box);

      if (type == AVIF_FOURCC('f','t','y','p'))
      {
         png_uint_32 major;
         int ok = 0;
         ftyp = box;
         major = pngavif_rd32(png_ptr, &ftyp);
         pngavif_skip(png_ptr, &ftyp, 4); /* minor_version */
         if (major == AVIF_FOURCC('a','v','i','f') ||
             major == AVIF_FOURCC('a','v','i','s'))
            ok = 1;
         while (!ok && ftyp.cur < ftyp.end)
         {
            png_uint_32 compat = pngavif_rd32(png_ptr, &ftyp);
            if (compat == AVIF_FOURCC('a','v','i','f') ||
                compat == AVIF_FOURCC('a','v','i','s'))
               ok = 1;
         }
         if (!ok)
            png_error(png_ptr, "AVIF: not an AVIF file (ftyp brand)");
      }
      else if (type == AVIF_FOURCC('m','e','t','a'))
      {
         pngavif_parse_meta(png_ptr, &meta, &box);
         have_meta = 1;
      }
   }

   if (!have_meta || !meta.have_primary)
      png_error(png_ptr, "AVIF: not an AVIF file (no meta/pitm)");

   /* ---- locate colour and alpha items ---- */
   for (i = 0; i < meta.n_items; i++)
   {
      pngavif_item *it = &meta.items[i];
      if (!it->present)
         continue;
      if (it->id == meta.primary_id)
         color = it;
      else if (it->is_alpha_aux)
         alpha = it;
   }

   if (color == NULL)
      png_error(png_ptr, "AVIF: primary item not found");
   if (color->type == AVIF_FOURCC('g','r','i','d'))
      png_error(png_ptr, "AVIF: grid images are not supported");
   pngavif_check_item(png_ptr, color, "AVIF: primary item is not AV1");
   if (alpha != NULL)
      pngavif_check_item(png_ptr, alpha, "AVIF: alpha item is not AV1");

   if (!color->has_ispe || color->width == 0 || color->height == 0)
      png_error(png_ptr, "AVIF: missing image dimensions (ispe)");
   if ((uint64_t)color->width * color->height > PNGAVIF_MAX_PIXELS)
      png_error(png_ptr, "AVIF: image too large (PNGAVIF_MAX_PIXELS)");

   /* ---- decode ---- */
   {
      pngavif_d1 d1;
      void *color_ctx = NULL, *alpha_ctx = NULL;
      png_byte *color_pic_buf, *alpha_pic_buf = NULL;
      pngavif_d1_picture *cpic, *apic = NULL;
      png_byte *bitstream = NULL, *alpha_bitstream = NULL;
      size_t bitstream_size = 0, alpha_bitstream_size = 0;
      pngavif_csc csc;
      png_bytep row;
      size_t row_bytes;
      png_uint_32 y;
      int mc, fr;

      pngavif_d1_load(png_ptr, &d1);

      color_pic_buf = (png_byte *)png_malloc(png_ptr,
          PNGAVIF_D1_PICTURE_BUFSIZE);

      pngavif_gather(png_ptr, color, data, data_size, &bitstream,
          &bitstream_size);

      pngavif_d1_decode(png_ptr, &d1, bitstream, bitstream_size,
          &color_ctx, color_pic_buf, &cpic);

      if (cpic->p.bpc != 8 && cpic->p.bpc != 10)
         png_error(png_ptr, "AVIF: unsupported bit depth (need 8 or 10)");

      if (alpha != NULL)
      {
         alpha_pic_buf = (png_byte *)png_malloc(png_ptr,
             PNGAVIF_D1_PICTURE_BUFSIZE);
         pngavif_gather(png_ptr, alpha, data, data_size, &alpha_bitstream,
             &alpha_bitstream_size);
         pngavif_d1_decode(png_ptr, &d1, alpha_bitstream,
             alpha_bitstream_size, &alpha_ctx, alpha_pic_buf, &apic);
         if (apic->p.w != cpic->p.w || apic->p.h != cpic->p.h)
            png_error(png_ptr, "AVIF: alpha dimensions mismatch");
         if (apic->p.bpc != cpic->p.bpc)
            png_error(png_ptr, "AVIF: alpha bit depth mismatch");
      }

      /* ---- colour description ---- */
      if (color->has_colr)
      {
         info->colour_primaries = color->colr_cp;
         info->transfer_characteristics = color->colr_tc;
         mc = color->colr_mc;
         fr = color->colr_fr;
      }
      else
      {
         info->colour_primaries = 2;      /* unspecified */
         info->transfer_characteristics = 2;
         mc = 1;                          /* assume BT.709 ... */
         fr = 0;                          /* ... studio range */
         png_warning(png_ptr,
             "AVIF: no colr box, assuming BT.709 studio range");
      }
      if (mc == 2)
         mc = 1; /* CICP "unspecified": assume BT.709, no warning */
      else if (mc != 0 && mc != 1 && mc != 4 && mc != 5 && mc != 6 &&
               mc != 7 && mc != 9 && mc != 10)
      {
         png_warning(png_ptr,
             "AVIF: unsupported matrix coefficients, using BT.709");
         mc = 1;
      }

      info->width = (png_uint_32)cpic->p.w;
      info->height = (png_uint_32)cpic->p.h;
      info->bit_depth = cpic->p.bpc;
      info->has_alpha = alpha != NULL;
      info->num_channels = alpha != NULL ? 4 : 3;
      info->matrix_coefficients = mc;
      info->full_range = fr;

      if ((uint64_t)info->width * info->height > PNGAVIF_MAX_PIXELS)
         png_error(png_ptr, "AVIF: image too large (PNGAVIF_MAX_PIXELS)");

      pngavif_csc_init(&csc, mc, fr, info->bit_depth);

      /* ---- convert and deliver rows ---- */
      row_bytes = (size_t)info->width * (size_t)info->num_channels * 2;
      row = (png_bytep)png_malloc(png_ptr, row_bytes);

      for (y = 0; y < info->height; y++)
      {
         png_uint_32 x;
         png_byte *out = row;
         png_uint_32 cy = y, ay = y;

         if (cpic->p.layout == 1) /* I420: chroma is half-res both ways */
            cy = y >> 1;

         for (x = 0; x < info->width; x++)
         {
            png_uint_16 rgb[3];
            unsigned int Y = pngavif_plane_at(cpic, 0, x, y);
            unsigned int U, V;

            if (cpic->p.layout == 3) /* I444 */
            {
               U = pngavif_plane_at(cpic, 1, x, y);
               V = pngavif_plane_at(cpic, 2, x, y);
            }
            else if (cpic->p.layout == 2) /* I422 */
            {
               U = pngavif_plane_at(cpic, 1, x >> 1, y);
               V = pngavif_plane_at(cpic, 2, x >> 1, y);
            }
            else if (cpic->p.layout == 1) /* I420: nearest-neighbour */
            {
               U = pngavif_plane_at(cpic, 1, x >> 1, cy);
               V = pngavif_plane_at(cpic, 2, x >> 1, cy);
            }
            else /* I400: grayscale */
            {
               U = V = (unsigned int)csc.coff;
            }

            pngavif_csc_apply(&csc, Y, U, V, rgb);

            *out++ = (png_byte)(rgb[0] >> 8);
            *out++ = (png_byte)(rgb[0] & 0xff);
            *out++ = (png_byte)(rgb[1] >> 8);
            *out++ = (png_byte)(rgb[1] & 0xff);
            *out++ = (png_byte)(rgb[2] >> 8);
            *out++ = (png_byte)(rgb[2] & 0xff);

            if (apic != NULL)
            {
               unsigned int a = pngavif_plane_at(apic, 0, x, ay);
               png_uint_16 a16 = info->bit_depth == 10
                   ? pngavif_scale10(a) : pngavif_scale8(a);
               *out++ = (png_byte)(a16 >> 8);
               *out++ = (png_byte)(a16 & 0xff);
            }
         }

         row_fn(png_ptr, row, y, user_data);
      }

      /* ---- cleanup (also on the success path) ---- */
      png_free(png_ptr, row);
      d1.picture_unref(color_pic_buf);
      d1.close(&color_ctx);
      if (apic != NULL)
      {
         d1.picture_unref(alpha_pic_buf);
         d1.close(&alpha_ctx);
      }
      png_free(png_ptr, color_pic_buf);
      if (alpha_pic_buf != NULL)
         png_free(png_ptr, alpha_pic_buf);
      png_free(png_ptr, bitstream);
      if (alpha_bitstream != NULL)
         png_free(png_ptr, alpha_bitstream);
      dlclose(d1.handle);
   }

   return 0;
}

int
png_avif_read_file(png_structp png_ptr, png_const_charp file_name,
    png_avif_info *info, png_avif_row_ptr row_fn, png_voidp user_data)
{
   FILE *fp;
   long fsize;
   png_bytep buf;
   int rc;

   fp = fopen(file_name, "rb");
   if (fp == NULL)
      png_error(png_ptr, "AVIF: cannot open file");
   if (fseek(fp, 0, SEEK_END) != 0 || (fsize = ftell(fp)) < 0 ||
       fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      png_error(png_ptr, "AVIF: cannot determine file size");
   }
   if ((uint64_t)fsize > PNGAVIF_MAX_FILE_BYTES)
   {
      fclose(fp);
      png_error(png_ptr, "AVIF: file too large (PNGAVIF_MAX_FILE_BYTES)");
   }

   buf = (png_bytep)png_malloc(png_ptr, (size_t)fsize == 0 ? 1 : (size_t)fsize);
   if (fsize > 0 && fread(buf, 1, (size_t)fsize, fp) != (size_t)fsize)
   {
      fclose(fp);
      png_free(png_ptr, buf);
      png_error(png_ptr, "AVIF: cannot read file");
   }
   fclose(fp);

   rc = png_avif_read_memory(png_ptr, buf, (size_t)fsize, info, row_fn,
       user_data);
   png_free(png_ptr, buf);
   return rc;
}
