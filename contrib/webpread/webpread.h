/* webpread.h
 *
 * Copyright (c) 2026 libpng contributors
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * WebP reading adapter for the libpng progressive row-callback interface.
 *
 * The "old pipeline" speaks the libpng progressive read protocol:
 *
 *    png_set_progressive_read_fn(png_ptr, app, info_cb, row_cb, end_cb);
 *    png_process_data(png_ptr, info_ptr, buf, len);   // repeat
 *
 * This adapter sits in front of that pipeline and sniffs the container:
 *
 *    - PNG signature: bytes are forwarded to png_process_data() unchanged.
 *      The PNG state machine is byte-for-byte untouched.
 *    - RIFF/WEBP container: the VP8L (lossless) bitstream is decoded to
 *      pixels, the pixels are re-encoded on the fly as a valid PNG image
 *      stream (8-bit RGBA, non-interlaced) and that stream is fed to
 *      png_process_data().  The application's info/row/end callbacks fire
 *      with exactly the same semantics as for a PNG file.
 *    - Anything else: forwarded to libpng, which reports the error exactly
 *      as the old pipeline did.
 *
 * The adapter adds no public libpng API and changes none; it is a
 * codec/processing-layer module with its own private interface.  Its only
 * dependency beyond libpng is zlib, which libpng already requires.
 *
 * Error handling: WebP-side problems are reported as explicit status codes
 * (enum webp_adapt_status) with a human-readable detail message; corrupt
 * data never reaches the PNG state machine, never longjmps and never
 * crashes.  PNG-side problems keep the old behavior (png_error/longjmp
 * through the caller's own setjmp handler).
 */
#ifndef WEBPREAD_H
#define WEBPREAD_H

#if defined(HAVE_CONFIG_H) && !defined(PNG_NO_CONFIG_H)
#  include <config.h>
#endif

#ifdef PNG_FREESTANDING_TESTS
#  include <png.h>
#else
#  include "../../png.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Explicit status codes returned by webp_adapt_push().  0 means "no error
 * so far"; any non-zero value is sticky (further pushes return it again).
 */
enum webp_adapt_status
{
   WEBP_ADAPT_OK = 0,
   WEBP_ADAPT_TRUNCATED,               /* stream ended mid-structure       */
   WEBP_ADAPT_BAD_CONTAINER,           /* RIFF/WebP chunk layout invalid   */
   WEBP_ADAPT_BAD_BITSTREAM,           /* VP8L entropy data invalid        */
   WEBP_ADAPT_UNSUPPORTED_COMPRESSION, /* VP8 (lossy) bitstream            */
   WEBP_ADAPT_UNSUPPORTED_TRANSFORM,   /* predictor/color/palette transform*/
   WEBP_ADAPT_UNSUPPORTED_FEATURE,     /* animation, meta-prefix, version  */
   WEBP_ADAPT_SIZE_LIMIT,              /* exceeds a configured bound       */
   WEBP_ADAPT_OOM,                     /* allocation failure               */
   WEBP_ADAPT_INTERNAL                 /* invariant violation (zlib etc.)  */
};

enum webp_adapt_format
{
   WEBP_ADAPT_FMT_PENDING = 0,  /* not enough bytes seen yet             */
   WEBP_ADAPT_FMT_PNG,          /* PNG (or unknown -> libpng reports it) */
   WEBP_ADAPT_FMT_WEBP          /* RIFF/WEBP container                   */
};

/* Resource bounds (compile-time configurable).  These are the memory
 * upper-bound levers; see README.md for the full budget.
 */
#ifndef WEBP_ADAPT_MAX_INPUT
#  define WEBP_ADAPT_MAX_INPUT (64u*1024u*1024u) /* RIFF size cap         */
#endif
#ifndef WEBP_ADAPT_MAX_PIXELS
#  define WEBP_ADAPT_MAX_PIXELS (1u<<24)         /* pixel-count cap       */
#endif

typedef struct webp_adapt webp_adapt;

/* Create an adapter bound to an existing progressive read struct.  The
 * caller keeps ownership of png_ptr/info_ptr and must have installed the
 * progressive callbacks before pushing data.  Returns NULL on OOM.
 */
webp_adapt *webp_adapt_create(png_structp png_ptr, png_infop info_ptr);

/* Push the next chunk of input.  Pass end_of_stream != 0 with the final
 * (possibly empty) chunk.  Returns a webp_adapt_status value: WEBP_ADAPT_OK
 * (0) while healthy, otherwise a sticky error code.  After an error the
 * caller should stop pushing and destroy the adapter; the png_struct was
 * never given any WebP-derived bytes, so it can be destroyed normally.
 */
int webp_adapt_push(webp_adapt *adapt, const png_byte *data, size_t size,
    int end_of_stream);

/* Current status (same value the last push returned). */
int webp_adapt_status(const webp_adapt *adapt);

/* Detected container format (enum webp_adapt_format). */
int webp_adapt_format(const webp_adapt *adapt);

/* Detail string for the current status (empty string when OK). */
const char *webp_adapt_message(const webp_adapt *adapt);

/* Status code as a static string, e.g. "TRUNCATED". */
const char *webp_adapt_status_name(int status);

/* Destroy the adapter (does not touch the png structs).  NULL-safe. */
void webp_adapt_destroy(webp_adapt **adapt_ptr);

#ifdef __cplusplus
}
#endif

#endif /* WEBPREAD_H */
