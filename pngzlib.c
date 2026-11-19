/* pngzlib.c - the default libpng compression backend, implemented on zlib
 *
 * Copyright (c) 2018-2026 Cosmin Truta
 * Copyright (c) 1998-2002,2004,2006-2018 Glenn Randers-Pehrson
 * Copyright (c) 1996-1997 Andreas Dilger
 * Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * This is the ONLY libpng source file that includes <zlib.h> and calls the
 * zlib API directly.  Everything else in the library goes through the
 * pluggable compression backend interface (png_compression_backend in
 * png.h); this file adapts that interface to zlib.
 */

#include "pngpriv.h"

#ifndef ZLIB_CONST
   /* Ensure that zlib uses 'const' in declarations. */
#  define ZLIB_CONST
#endif
#include <zlib.h>
#ifdef const
   /* zlib.h sometimes #defines const to nothing, undo this. */
#  undef const
#endif

/* The Adler-32 option relies on inflateValidate, added in zlib 1.2.9. */
#if defined(PNG_DISABLE_ADLER32_CHECK_SUPPORTED) && ZLIB_VERNUM < 0x1290
#  error DISABLE_ADLER32_CHECK requires zlib version 1.2.9 or newer
#endif

/* The backend interface status and flush codes are defined in png.h to have
 * the same numeric values as the zlib ones, which allows this adapter to
 * pass zlib return codes and libpng flush values straight through.  Verify
 * that here, so that a zlib with different values fails to compile rather
 * than misbehaving at run time.
 */
#if PNG_Z_OK != Z_OK || PNG_Z_STREAM_END != Z_STREAM_END || \
    PNG_Z_NEED_DICT != Z_NEED_DICT || PNG_Z_ERRNO != Z_ERRNO || \
    PNG_Z_STREAM_ERROR != Z_STREAM_ERROR || \
    PNG_Z_DATA_ERROR != Z_DATA_ERROR || PNG_Z_MEM_ERROR != Z_MEM_ERROR || \
    PNG_Z_BUF_ERROR != Z_BUF_ERROR || PNG_Z_VERSION_ERROR != Z_VERSION_ERROR
#  error PNG_Z_* status codes do not match the zlib ones
#endif

#if PNG_Z_NO_FLUSH != Z_NO_FLUSH || PNG_Z_SYNC_FLUSH != Z_SYNC_FLUSH || \
    PNG_Z_FINISH != Z_FINISH
#  error PNG_Z_* flush values do not match the zlib ones
#endif

#if defined(PNG_READ_SUPPORTED) || defined(PNG_WRITE_SUPPORTED)

/* Memory allocation callbacks given to zlib; they route zlib's internal
 * allocations through the libpng allocator, exactly as libpng has always
 * done.  (These used to be in png.c, visible to the whole library; now they
 * are private to this backend.)
 */
static voidpf
png_zlib_alloc(voidpf png_ptr, uInt items, uInt size)
{
   png_alloc_size_t num_bytes = size;

   if (png_ptr == NULL)
      return NULL;

   /* Extra-cautious prevention against programming errors inside zlib. */
   if (size != 0 && items >= (~(png_alloc_size_t)0) / size)
   {
      png_warning(png_voidcast(png_struct *, png_ptr),
                  "Potential overflow in png_zlib_alloc()");
      return NULL;
   }

   num_bytes *= items;
   return png_malloc_warn(png_voidcast(png_struct *, png_ptr), num_bytes);
}

static void
png_zlib_free(voidpf png_ptr, voidpf ptr)
{
   png_free(png_voidcast(const png_struct *, png_ptr), ptr);
}

/* Obtain the z_stream for the given backend stream, allocating and wiring
 * it up on first use.  Returns NULL (after setting strm->msg) on failure.
 */
static z_stream *
png_zlib_claim_stream(png_zstream *strm)
{
   z_stream *zs = png_voidcast(z_stream *, strm->state);

   if (zs == NULL)
   {
      png_struct *png_ptr = png_voidcast(png_struct *, strm->opaque);

      if (png_ptr == NULL)
      {
         strm->msg = "compression stream has no owner";
         return NULL;
      }

      zs = png_voidcast(z_stream *, png_malloc_base(png_ptr, (sizeof *zs)));

      if (zs == NULL)
      {
         strm->msg = "insufficient memory";
         return NULL;
      }

      memset(zs, 0, (sizeof *zs));
      zs->zalloc = png_zlib_alloc;
      zs->zfree = png_zlib_free;
      zs->opaque = png_ptr;
      strm->state = zs;
   }

   return zs;
}

/* Copy the I/O fields from the backend-neutral stream into the z_stream
 * before a zlib call.
 */
static void
png_zlib_sync_in(png_zstream *strm, z_stream *zs)
{
   zs->next_in = strm->next_in;
   zs->avail_in = strm->avail_in;
   zs->next_out = strm->next_out;
   zs->avail_out = strm->avail_out;
}

/* Copy the I/O fields (and any error message) back after a zlib call. */
static void
png_zlib_sync_out(png_zstream *strm, z_stream *zs)
{
   strm->next_in = zs->next_in;
   strm->avail_in = zs->avail_in;
   strm->next_out = zs->next_out;
   strm->avail_out = zs->avail_out;

   /* Only overwrite the message if zlib actually set one; libpng may have
    * stored its own message in strm->msg.
    */
   if (zs->msg != Z_NULL)
      strm->msg = zs->msg;
}

static int
png_zlib_inflate_init(png_zstream *strm, int window_bits)
{
   z_stream *zs = png_zlib_claim_stream(strm);

   if (zs == NULL)
      return PNG_Z_MEM_ERROR;

   /* A stream that already has zlib inflate state is reinitialized with the
    * new window size (this is the inflateReset2 case); a fresh stream is
    * initialized from scratch.
    */
   if (zs->state != Z_NULL)
      return inflateReset2(zs, window_bits);

   return inflateInit2(zs, window_bits);
}

static int
png_zlib_inflate_reset(png_zstream *strm)
{
   z_stream *zs = png_voidcast(z_stream *, strm->state);
   int ret;

   if (zs == NULL)
   {
      strm->msg = "inflate stream not initialized";
      return PNG_Z_STREAM_ERROR;
   }

   ret = inflateReset(zs);
   strm->msg = zs->msg;
   return ret;
}

static int
png_zlib_inflate(png_zstream *strm, int flush)
{
   z_stream *zs = png_voidcast(z_stream *, strm->state);
   int ret;

   if (zs == NULL)
   {
      strm->msg = "inflate stream not initialized";
      return PNG_Z_STREAM_ERROR;
   }

   png_zlib_sync_in(strm, zs);
   ret = inflate(zs, flush);
   png_zlib_sync_out(strm, zs);
   return ret;
}

static int
png_zlib_inflate_end(png_zstream *strm)
{
   z_stream *zs = png_voidcast(z_stream *, strm->state);
   int ret = PNG_Z_OK;

   if (zs != NULL)
   {
      png_struct *png_ptr = png_voidcast(png_struct *, strm->opaque);

      ret = inflateEnd(zs);
      strm->state = NULL;
      strm->msg = zs->msg;

      /* The z_stream itself was allocated by this backend (see
       * png_zlib_claim_stream); inflateEnd only releases zlib's internal
       * state.
       */
      png_free(png_ptr, zs);
   }

   return ret;
}

#ifdef PNG_DISABLE_ADLER32_CHECK_SUPPORTED
static int
png_zlib_inflate_validate(png_zstream *strm, int check)
{
   z_stream *zs = png_voidcast(z_stream *, strm->state);

   if (zs == NULL)
   {
      strm->msg = "inflate stream not initialized";
      return PNG_Z_STREAM_ERROR;
   }

   return inflateValidate(zs, check);
}
#endif /* DISABLE_ADLER32_CHECK */

static int
png_zlib_deflate_init(png_zstream *strm, int level, int method,
    int window_bits, int mem_level, int strategy)
{
   z_stream *zs = png_zlib_claim_stream(strm);

   if (zs == NULL)
      return PNG_Z_MEM_ERROR;

   return deflateInit2(zs, level, method, window_bits, mem_level, strategy);
}

static int
png_zlib_deflate_reset(png_zstream *strm)
{
   z_stream *zs = png_voidcast(z_stream *, strm->state);
   int ret;

   if (zs == NULL)
   {
      strm->msg = "deflate stream not initialized";
      return PNG_Z_STREAM_ERROR;
   }

   ret = deflateReset(zs);
   strm->msg = zs->msg;
   return ret;
}

static int
png_zlib_deflate(png_zstream *strm, int flush)
{
   z_stream *zs = png_voidcast(z_stream *, strm->state);
   int ret;

   if (zs == NULL)
   {
      strm->msg = "deflate stream not initialized";
      return PNG_Z_STREAM_ERROR;
   }

   png_zlib_sync_in(strm, zs);
   ret = deflate(zs, flush);
   png_zlib_sync_out(strm, zs);
   return ret;
}

static int
png_zlib_deflate_end(png_zstream *strm)
{
   z_stream *zs = png_voidcast(z_stream *, strm->state);
   int ret = PNG_Z_OK;

   if (zs != NULL)
   {
      png_struct *png_ptr = png_voidcast(png_struct *, strm->opaque);

      ret = deflateEnd(zs);
      strm->state = NULL;
      strm->msg = zs->msg;
      png_free(png_ptr, zs);
   }

   return ret;
}

static png_uint_32
png_zlib_crc32(png_uint_32 crc, png_const_bytep data, size_t length)
{
   /* 'uLong' is defined in zlib.h as unsigned long; this means that on some
    * systems it is a 64-bit value.  crc32, however, returns 32 bits so the
    * following cast is safe.  'uInt' may be no more than 16 bits, so it is
    * necessary to perform a loop here.
    */
   uLong z_crc = crc;

   if (data == NULL)
      return (png_uint_32)crc32(z_crc, Z_NULL, 0);

   while (length > 0)
   {
      uInt safe_length = (uInt)length;

      if (safe_length == 0)
         safe_length = (uInt)-1; /* evil, but safe */

      z_crc = crc32(z_crc, data, safe_length);

      data += safe_length;
      length -= safe_length;
   }

   /* The following cast is always safe because the crc is only 32 bits. */
   return (png_uint_32)z_crc;
}

/* The default compression backend. */
const png_compression_backend png_zlib_backend =
{
   "zlib",
   png_zlib_inflate_init,
   png_zlib_inflate_reset,
   png_zlib_inflate,
   png_zlib_inflate_end,
#ifdef PNG_DISABLE_ADLER32_CHECK_SUPPORTED
   png_zlib_inflate_validate,
#else
   NULL,
#endif
   png_zlib_deflate_init,
   png_zlib_deflate_reset,
   png_zlib_deflate,
   png_zlib_deflate_end,
   png_zlib_crc32
};

#endif /* READ || WRITE */
