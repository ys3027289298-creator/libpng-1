/* pngwtran.c - transforms the data in a row for PNG writers
 *
 * Copyright (c) 2018 Cosmin Truta
 * Copyright (c) 1998-2002,2004,2006-2016,2018 Glenn Randers-Pehrson
 * Copyright (c) 1996-1997 Andreas Dilger
 * Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 */

#include "pngpriv.h"

#ifdef PNG_WRITE_SUPPORTED
#ifdef PNG_WRITE_TRANSFORMS_SUPPORTED

#ifdef PNG_WRITE_PACK_SUPPORTED
/* Pack pixels into bytes.  Pass the true bit depth in bit_depth.  The
 * row_info bit depth should be 8 (one pixel per byte).  The channels
 * should be 1 (this only happens on grayscale and paletted images).
 */
static void
png_do_pack(png_row_info *row_info, png_byte *row, png_uint_32 bit_depth)
{
   png_debug(1, "in png_do_pack");

   if (row_info->bit_depth == 8 &&
      row_info->channels == 1)
   {
      switch ((int)bit_depth)
      {
         case 1:
         {
            png_byte *sp, *dp;
            int mask, v;
            png_uint_32 i;
            png_uint_32 row_width = row_info->width;

            sp = row;
            dp = row;
            mask = 0x80;
            v = 0;

            for (i = 0; i < row_width; i++)
            {
               if (*sp != 0)
                  v |= mask;

               sp++;

               if (mask > 1)
                  mask >>= 1;

               else
               {
                  mask = 0x80;
                  *dp = (png_byte)v;
                  dp++;
                  v = 0;
               }
            }

            if (mask != 0x80)
               *dp = (png_byte)v;

            break;
         }

         case 2:
         {
            png_byte *sp, *dp;
            unsigned int shift;
            int v;
            png_uint_32 i;
            png_uint_32 row_width = row_info->width;

            sp = row;
            dp = row;
            shift = 6;
            v = 0;

            for (i = 0; i < row_width; i++)
            {
               png_byte value;

               value = (png_byte)(*sp & 0x03);
               v |= (value << shift);

               if (shift == 0)
               {
                  shift = 6;
                  *dp = (png_byte)v;
                  dp++;
                  v = 0;
               }

               else
                  shift -= 2;

               sp++;
            }

            if (shift != 6)
               *dp = (png_byte)v;

            break;
         }

         case 4:
         {
            png_byte *sp, *dp;
            unsigned int shift;
            int v;
            png_uint_32 i;
            png_uint_32 row_width = row_info->width;

            sp = row;
            dp = row;
            shift = 4;
            v = 0;

            for (i = 0; i < row_width; i++)
            {
               png_byte value;

               value = (png_byte)(*sp & 0x0f);
               v |= (value << shift);

               if (shift == 0)
               {
                  shift = 4;
                  *dp = (png_byte)v;
                  dp++;
                  v = 0;
               }

               else
                  shift -= 4;

               sp++;
            }

            if (shift != 4)
               *dp = (png_byte)v;

            break;
         }

         default:
            break;
      }

      row_info->bit_depth = (png_byte)bit_depth;
      row_info->pixel_depth = (png_byte)(bit_depth * row_info->channels);
      row_info->rowbytes = PNG_ROWBYTES(row_info->pixel_depth,
          row_info->width);
   }
}
#endif

#ifdef PNG_WRITE_SHIFT_SUPPORTED
/* Shift pixel values to take advantage of whole range.  Pass the
 * true number of bits in bit_depth.  The row should be packed
 * according to row_info->bit_depth.  Thus, if you had a row of
 * bit depth 4, but the pixels only had values from 0 to 7, you
 * would pass 3 as bit_depth, and this routine would translate the
 * data to 0 to 15.
 */
static void
png_do_shift(png_row_info *row_info, png_byte *row,
    const png_color_8 *bit_depth)
{
   png_debug(1, "in png_do_shift");

   if (row_info->color_type != PNG_COLOR_TYPE_PALETTE)
   {
      int shift_start[4], shift_dec[4];
      unsigned int channels = 0;

      if ((row_info->color_type & PNG_COLOR_MASK_COLOR) != 0)
      {
         shift_start[channels] = row_info->bit_depth - bit_depth->red;
         shift_dec[channels] = bit_depth->red;
         channels++;

         shift_start[channels] = row_info->bit_depth - bit_depth->green;
         shift_dec[channels] = bit_depth->green;
         channels++;

         shift_start[channels] = row_info->bit_depth - bit_depth->blue;
         shift_dec[channels] = bit_depth->blue;
         channels++;
      }

      else
      {
         shift_start[channels] = row_info->bit_depth - bit_depth->gray;
         shift_dec[channels] = bit_depth->gray;
         channels++;
      }

      if ((row_info->color_type & PNG_COLOR_MASK_ALPHA) != 0)
      {
         shift_start[channels] = row_info->bit_depth - bit_depth->alpha;
         shift_dec[channels] = bit_depth->alpha;
         channels++;
      }

      /* With low row depths, could only be grayscale, so one channel */
      if (row_info->bit_depth < 8)
      {
         png_byte *bp = row;
         size_t i;
         unsigned int mask;
         size_t row_bytes = row_info->rowbytes;

         if (bit_depth->gray == 1 && row_info->bit_depth == 2)
            mask = 0x55;

         else if (row_info->bit_depth == 4 && bit_depth->gray == 3)
            mask = 0x11;

         else
            mask = 0xff;

         for (i = 0; i < row_bytes; i++, bp++)
         {
            int j;
            unsigned int v, out;

            v = *bp;
            out = 0;

            for (j = shift_start[0]; j > -shift_dec[0]; j -= shift_dec[0])
            {
               if (j > 0)
                  out |= v << j;

               else
                  out |= (v >> (-j)) & mask;
            }

            *bp = (png_byte)(out & 0xff);
         }
      }

      else if (row_info->bit_depth == 8)
      {
         png_byte *bp = row;
         png_uint_32 i;
         png_uint_32 istop = channels * row_info->width;

         for (i = 0; i < istop; i++, bp++)
         {
            unsigned int c = i%channels;
            int j;
            unsigned int v, out;

            v = *bp;
            out = 0;

            for (j = shift_start[c]; j > -shift_dec[c]; j -= shift_dec[c])
            {
               if (j > 0)
                  out |= v << j;

               else
                  out |= v >> (-j);
            }

            *bp = (png_byte)(out & 0xff);
         }
      }

      else
      {
         png_byte *bp;
         png_uint_32 i;
         png_uint_32 istop = channels * row_info->width;

         for (bp = row, i = 0; i < istop; i++)
         {
            unsigned int c = i%channels;
            int j;
            unsigned int value, v;

            v = png_get_uint_16(bp);
            value = 0;

            for (j = shift_start[c]; j > -shift_dec[c]; j -= shift_dec[c])
            {
               if (j > 0)
                  value |= v << j;

               else
                  value |= v >> (-j);
            }
            *bp++ = (png_byte)((value >> 8) & 0xff);
            *bp++ = (png_byte)(value & 0xff);
         }
      }
   }
}
#endif

#ifdef PNG_WRITE_SWAP_ALPHA_SUPPORTED
static void
png_do_write_swap_alpha(png_row_info *row_info, png_byte *row)
{
   png_debug(1, "in png_do_write_swap_alpha");

   {
      if (row_info->color_type == PNG_COLOR_TYPE_RGB_ALPHA)
      {
         if (row_info->bit_depth == 8)
         {
            /* This converts from ARGB to RGBA */
            png_byte *sp, *dp;
            png_uint_32 i;
            png_uint_32 row_width = row_info->width;

            for (i = 0, sp = dp = row; i < row_width; i++)
            {
               png_byte save = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = save;
            }
         }

#ifdef PNG_WRITE_16BIT_SUPPORTED
         else
         {
            /* This converts from AARRGGBB to RRGGBBAA */
            png_byte *sp, *dp;
            png_uint_32 i;
            png_uint_32 row_width = row_info->width;

            for (i = 0, sp = dp = row; i < row_width; i++)
            {
               png_byte save[2];
               save[0] = *(sp++);
               save[1] = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = save[0];
               *(dp++) = save[1];
            }
         }
#endif /* WRITE_16BIT */
      }

      else if (row_info->color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
      {
         if (row_info->bit_depth == 8)
         {
            /* This converts from AG to GA */
            png_byte *sp, *dp;
            png_uint_32 i;
            png_uint_32 row_width = row_info->width;

            for (i = 0, sp = dp = row; i < row_width; i++)
            {
               png_byte save = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = save;
            }
         }

#ifdef PNG_WRITE_16BIT_SUPPORTED
         else
         {
            /* This converts from AAGG to GGAA */
            png_byte *sp, *dp;
            png_uint_32 i;
            png_uint_32 row_width = row_info->width;

            for (i = 0, sp = dp = row; i < row_width; i++)
            {
               png_byte save[2];
               save[0] = *(sp++);
               save[1] = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = save[0];
               *(dp++) = save[1];
            }
         }
#endif /* WRITE_16BIT */
      }
   }
}
#endif

#ifdef PNG_WRITE_INVERT_ALPHA_SUPPORTED
static void
png_do_write_invert_alpha(png_row_info *row_info, png_byte *row)
{
   png_debug(1, "in png_do_write_invert_alpha");

   {
      if (row_info->color_type == PNG_COLOR_TYPE_RGB_ALPHA)
      {
         if (row_info->bit_depth == 8)
         {
            /* This inverts the alpha channel in RGBA */
            png_byte *sp, *dp;
            png_uint_32 i;
            png_uint_32 row_width = row_info->width;

            for (i = 0, sp = dp = row; i < row_width; i++)
            {
               /* Does nothing
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               */
               sp+=3; dp = sp;
               *dp = (png_byte)(255 - *(sp++));
            }
         }

#ifdef PNG_WRITE_16BIT_SUPPORTED
         else
         {
            /* This inverts the alpha channel in RRGGBBAA */
            png_byte *sp, *dp;
            png_uint_32 i;
            png_uint_32 row_width = row_info->width;

            for (i = 0, sp = dp = row; i < row_width; i++)
            {
               /* Does nothing
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               */
               sp+=6; dp = sp;
               *(dp++) = (png_byte)(255 - *(sp++));
               *dp     = (png_byte)(255 - *(sp++));
            }
         }
#endif /* WRITE_16BIT */
      }

      else if (row_info->color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
      {
         if (row_info->bit_depth == 8)
         {
            /* This inverts the alpha channel in GA */
            png_byte *sp, *dp;
            png_uint_32 i;
            png_uint_32 row_width = row_info->width;

            for (i = 0, sp = dp = row; i < row_width; i++)
            {
               *(dp++) = *(sp++);
               *(dp++) = (png_byte)(255 - *(sp++));
            }
         }

#ifdef PNG_WRITE_16BIT_SUPPORTED
         else
         {
            /* This inverts the alpha channel in GGAA */
            png_byte *sp, *dp;
            png_uint_32 i;
            png_uint_32 row_width = row_info->width;

            for (i = 0, sp = dp = row; i < row_width; i++)
            {
               /* Does nothing
               *(dp++) = *(sp++);
               *(dp++) = *(sp++);
               */
               sp+=2; dp = sp;
               *(dp++) = (png_byte)(255 - *(sp++));
               *dp     = (png_byte)(255 - *(sp++));
            }
         }
#endif /* WRITE_16BIT */
      }
   }
}
#endif

#ifdef PNG_WRITE_BACKGROUND_SUPPORTED
/* Exact fixed-point division by 255 and 65535 for the compositing sums.
 *
 * For 0 <= x <= 2^16-2: floor(x/255)   = (x + (x>>8)  + 1) >> 8
 * For 0 <= x <= 2^32-2: floor(x/65535) = (x + (x>>16) + 1) >> 16
 *
 * (Proof: write x = 2^n*h + l; then x/(2^n-1) = h + (h+l)/(2^n-1), and
 * h+l is small enough that floor((h+l)/(2^n-1)) = floor((h+l+1)/2^n),
 * except for the single value x = 2^(2n)-1, which the compositing sums
 * below never reach: they are at most 65152 and 4294868992.  Both
 * identities have been verified exhaustively over the stated ranges.)
 *
 * These evaluate to a few shifts and adds, unlike a plain division by a
 * 16-bit constant, which requires a 64-bit multiply.
 */
#define PNG_DIV_255(x)   (((x) + ((x) >> 8) + 1) >> 8)
#define PNG_DIV_65535(x) (((x) + ((x) >> 16) + 1) >> 16)

/* On x86, unaligned 16-bit accesses are safe, and GCC and Clang provide
 * __builtin_bswap16; accessing the row as an array of png_uint_16 lets
 * the compiler vectorize the 16-bit compositing loops.  Everywhere else
 * the portable byte-oriented code below is used.  (The row data is one
 * byte past the start of the row buffer, so 16-bit accesses are always
 * unaligned; do not enable this on platforms where that faults.)
 */
#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
#  define PNG_COMPOSE_16BIT_FAST_PATH 1
#  define PNG_BSWAP_16(x) __builtin_bswap16(x)
#endif

/* Composite the alpha channel of an RGBA or GA row over a solid background
 * color.  The compositing is done in fixed point with round-to-nearest:
 *
 *    output = (foreground * alpha + background * (max - alpha) + max/2) / max
 *
 * where max is 255 or 65535.  All intermediate results are exact integers,
 * so this reproduces, bit for bit, the result of evaluating the compositing
 * formula in infinite precision and rounding half up.  If keep_alpha is
 * zero the alpha channel is removed from the row (and row_info is updated
 * to match), otherwise it is passed through unchanged.
 */
static void
png_do_write_compose_background(png_row_info *row_info, png_byte *row,
    const png_color_16 *background, int keep_alpha)
{
   png_debug(1, "in png_do_write_compose_background");

   if (row_info->bit_depth == 8)
   {
      if (row_info->channels == 4) /* RGBA */
      {
         const png_uint_32 bg_r = background->red;
         const png_uint_32 bg_g = background->green;
         const png_uint_32 bg_b = background->blue;
         /* Constant part of each compositing sum: background*255 + 127.
          * The per-pixel sum is background*(255-alpha) + foreground*alpha +
          * 127, evaluated here as base + alpha*(foreground-background);
          * the result is always in the range [127, 65152], so the unsigned
          * arithmetic below cannot wrap.
          */
         const png_uint_32 base_r = bg_r * 255 + 127;
         const png_uint_32 base_g = bg_g * 255 + 127;
         const png_uint_32 base_b = bg_b * 255 + 127;
         png_bytep sp = row;
         png_bytep dp = row;
         png_uint_32 i;
         png_uint_32 row_width = row_info->width;

         if (keep_alpha != 0)
         {
            for (i = 0; i < row_width; i++)
            {
               png_uint_32 a = sp[3];

               sp[0] = (png_byte)PNG_DIV_255(base_r +
                   a * (png_uint_32)((int)sp[0] - (int)bg_r));
               sp[1] = (png_byte)PNG_DIV_255(base_g +
                   a * (png_uint_32)((int)sp[1] - (int)bg_g));
               sp[2] = (png_byte)PNG_DIV_255(base_b +
                   a * (png_uint_32)((int)sp[2] - (int)bg_b));
               sp += 4;
            }
         }

         else /* strip the alpha channel */
         {
            for (i = 0; i < row_width; i++)
            {
               png_uint_32 a = sp[3];

               dp[0] = (png_byte)PNG_DIV_255(base_r +
                   a * (png_uint_32)((int)sp[0] - (int)bg_r));
               dp[1] = (png_byte)PNG_DIV_255(base_g +
                   a * (png_uint_32)((int)sp[1] - (int)bg_g));
               dp[2] = (png_byte)PNG_DIV_255(base_b +
                   a * (png_uint_32)((int)sp[2] - (int)bg_b));
               sp += 4;
               dp += 3;
            }
         }
      }

      else if (row_info->channels == 2) /* GA */
      {
         const png_uint_32 bg_g = background->gray;
         const png_uint_32 base_g = bg_g * 255 + 127;
         png_bytep sp = row;
         png_bytep dp = row;
         png_uint_32 i;
         png_uint_32 row_width = row_info->width;

         if (keep_alpha != 0)
         {
            for (i = 0; i < row_width; i++)
            {
               png_uint_32 a = sp[1];

               sp[0] = (png_byte)PNG_DIV_255(base_g +
                   a * (png_uint_32)((int)sp[0] - (int)bg_g));
               sp += 2;
            }
         }

         else /* strip the alpha channel */
         {
            for (i = 0; i < row_width; i++)
            {
               png_uint_32 a = sp[1];

               *(dp++) = (png_byte)PNG_DIV_255(base_g +
                   a * (png_uint_32)((int)sp[0] - (int)bg_g));
               sp += 2;
            }
         }
      }

      else
         return; /* nothing to composite */
   }

#ifdef PNG_WRITE_16BIT_SUPPORTED
   else if (row_info->bit_depth == 16)
   {
      if (row_info->channels == 4) /* RRGGBBAA */
      {
         const png_uint_32 bg_r = background->red;
         const png_uint_32 bg_g = background->green;
         const png_uint_32 bg_b = background->blue;
         /* Constant part of each compositing sum: background*65535 + 32767.
          * The per-pixel sum is background*(65535-alpha) +
          * foreground*alpha + 32767, evaluated here as
          * base + alpha*(foreground-background); the result is always in
          * the range [32767, 4294868992], so the unsigned arithmetic below
          * cannot wrap.
          */
         const png_uint_32 base_r = bg_r * 65535 + 32767;
         const png_uint_32 base_g = bg_g * 65535 + 32767;
         const png_uint_32 base_b = bg_b * 65535 + 32767;
         png_uint_32 i;
         png_uint_32 row_width = row_info->width;

         if (keep_alpha != 0)
         {
#ifdef PNG_COMPOSE_16BIT_FAST_PATH
            png_uint_16 *rp = (png_uint_16 *)row;

            for (i = 0; i < row_width; i++, rp += 4)
            {
               png_uint_32 a = PNG_BSWAP_16(rp[3]);
               png_uint_32 v;

               v = PNG_DIV_65535(base_r +
                   a * (png_uint_32)((int)PNG_BSWAP_16(rp[0]) - (int)bg_r));
               rp[0] = PNG_BSWAP_16((png_uint_16)v);
               v = PNG_DIV_65535(base_g +
                   a * (png_uint_32)((int)PNG_BSWAP_16(rp[1]) - (int)bg_g));
               rp[1] = PNG_BSWAP_16((png_uint_16)v);
               v = PNG_DIV_65535(base_b +
                   a * (png_uint_32)((int)PNG_BSWAP_16(rp[2]) - (int)bg_b));
               rp[2] = PNG_BSWAP_16((png_uint_16)v);
            }
#else
            png_bytep sp = row;

            for (i = 0; i < row_width; i++)
            {
               png_uint_32 a = png_get_uint_16(sp + 6);
               png_uint_32 v;

               v = PNG_DIV_65535(base_r +
                   a * (png_uint_32)((int)png_get_uint_16(sp) - (int)bg_r));
               sp[0] = (png_byte)(v >> 8);
               sp[1] = (png_byte)(v & 0xff);
               v = PNG_DIV_65535(base_g +
                   a * (png_uint_32)((int)png_get_uint_16(sp + 2) - (int)bg_g));
               sp[2] = (png_byte)(v >> 8);
               sp[3] = (png_byte)(v & 0xff);
               v = PNG_DIV_65535(base_b +
                   a * (png_uint_32)((int)png_get_uint_16(sp + 4) - (int)bg_b));
               sp[4] = (png_byte)(v >> 8);
               sp[5] = (png_byte)(v & 0xff);
               sp += 8;
            }
#endif /* COMPOSE_16BIT_FAST_PATH */
         }

         else /* strip the alpha channel */
         {
#ifdef PNG_COMPOSE_16BIT_FAST_PATH
            png_uint_16 *rp = (png_uint_16 *)row;
            png_uint_16 *wp = rp;

            for (i = 0; i < row_width; i++, rp += 4, wp += 3)
            {
               png_uint_32 a = PNG_BSWAP_16(rp[3]);
               png_uint_32 v;

               v = PNG_DIV_65535(base_r +
                   a * (png_uint_32)((int)PNG_BSWAP_16(rp[0]) - (int)bg_r));
               wp[0] = PNG_BSWAP_16((png_uint_16)v);
               v = PNG_DIV_65535(base_g +
                   a * (png_uint_32)((int)PNG_BSWAP_16(rp[1]) - (int)bg_g));
               wp[1] = PNG_BSWAP_16((png_uint_16)v);
               v = PNG_DIV_65535(base_b +
                   a * (png_uint_32)((int)PNG_BSWAP_16(rp[2]) - (int)bg_b));
               wp[2] = PNG_BSWAP_16((png_uint_16)v);
            }
#else
            png_bytep sp = row;
            png_bytep dp = row;

            for (i = 0; i < row_width; i++)
            {
               png_uint_32 a = png_get_uint_16(sp + 6);
               png_uint_32 v;

               v = PNG_DIV_65535(base_r +
                   a * (png_uint_32)((int)png_get_uint_16(sp) - (int)bg_r));
               dp[0] = (png_byte)(v >> 8);
               dp[1] = (png_byte)(v & 0xff);
               v = PNG_DIV_65535(base_g +
                   a * (png_uint_32)((int)png_get_uint_16(sp + 2) - (int)bg_g));
               dp[2] = (png_byte)(v >> 8);
               dp[3] = (png_byte)(v & 0xff);
               v = PNG_DIV_65535(base_b +
                   a * (png_uint_32)((int)png_get_uint_16(sp + 4) - (int)bg_b));
               dp[4] = (png_byte)(v >> 8);
               dp[5] = (png_byte)(v & 0xff);
               sp += 8;
               dp += 6;
            }
#endif /* COMPOSE_16BIT_FAST_PATH */
         }
      }

      else if (row_info->channels == 2) /* GGAA */
      {
         const png_uint_32 bg_g = background->gray;
         const png_uint_32 base_g = bg_g * 65535 + 32767;
         png_uint_32 i;
         png_uint_32 row_width = row_info->width;

         if (keep_alpha != 0)
         {
#ifdef PNG_COMPOSE_16BIT_FAST_PATH
            png_uint_16 *rp = (png_uint_16 *)row;

            for (i = 0; i < row_width; i++, rp += 2)
            {
               png_uint_32 a = PNG_BSWAP_16(rp[1]);
               png_uint_32 v = PNG_DIV_65535(base_g +
                   a * (png_uint_32)((int)PNG_BSWAP_16(rp[0]) - (int)bg_g));

               rp[0] = PNG_BSWAP_16((png_uint_16)v);
            }
#else
            png_bytep sp = row;

            for (i = 0; i < row_width; i++)
            {
               png_uint_32 a = png_get_uint_16(sp + 2);
               png_uint_32 v = PNG_DIV_65535(base_g +
                   a * (png_uint_32)((int)png_get_uint_16(sp) - (int)bg_g));

               sp[0] = (png_byte)(v >> 8);
               sp[1] = (png_byte)(v & 0xff);
               sp += 4;
            }
#endif /* COMPOSE_16BIT_FAST_PATH */
         }

         else /* strip the alpha channel */
         {
#ifdef PNG_COMPOSE_16BIT_FAST_PATH
            png_uint_16 *rp = (png_uint_16 *)row;
            png_uint_16 *wp = rp;

            for (i = 0; i < row_width; i++, rp += 2, wp += 1)
            {
               png_uint_32 a = PNG_BSWAP_16(rp[1]);
               png_uint_32 v = PNG_DIV_65535(base_g +
                   a * (png_uint_32)((int)PNG_BSWAP_16(rp[0]) - (int)bg_g));

               wp[0] = PNG_BSWAP_16((png_uint_16)v);
            }
#else
            png_bytep sp = row;
            png_bytep dp = row;

            for (i = 0; i < row_width; i++)
            {
               png_uint_32 a = png_get_uint_16(sp + 2);
               png_uint_32 v = PNG_DIV_65535(base_g +
                   a * (png_uint_32)((int)png_get_uint_16(sp) - (int)bg_g));

               dp[0] = (png_byte)(v >> 8);
               dp[1] = (png_byte)(v & 0xff);
               sp += 4;
               dp += 2;
            }
#endif /* COMPOSE_16BIT_FAST_PATH */
         }
      }

      else
         return; /* nothing to composite */
   }
#endif /* WRITE_16BIT */

   else
      return; /* bad bit depth */

   if (keep_alpha == 0)
   {
      /* The alpha channel has been stripped from the row; update row_info
       * to describe the row that remains.
       */
      row_info->channels = (png_byte)(row_info->channels - 1);
      row_info->pixel_depth =
          (png_byte)(row_info->pixel_depth - row_info->bit_depth);
      row_info->rowbytes =
          PNG_ROWBYTES(row_info->pixel_depth, row_info->width);

      if (row_info->color_type == PNG_COLOR_TYPE_RGB_ALPHA)
         row_info->color_type = PNG_COLOR_TYPE_RGB;

      else if (row_info->color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
         row_info->color_type = PNG_COLOR_TYPE_GRAY;
   }
}

#undef PNG_DIV_255
#undef PNG_DIV_65535
#undef PNG_COMPOSE_16BIT_FAST_PATH
#undef PNG_BSWAP_16
#endif /* WRITE_BACKGROUND */

/* Transform the data according to the user's wishes.  The order of
 * transformations is significant.
 */
void /* PRIVATE */
png_do_write_transformations(png_struct *png_ptr, png_row_info *row_info)
{
   png_debug(1, "in png_do_write_transformations");

   if (png_ptr == NULL)
      return;

#ifdef PNG_WRITE_USER_TRANSFORM_SUPPORTED
   if ((png_ptr->transformations & PNG_USER_TRANSFORM) != 0)
      if (png_ptr->write_user_transform_fn != NULL)
         (*(png_ptr->write_user_transform_fn)) /* User write transform
                                                 function */
             (png_ptr,  /* png_ptr */
             row_info,  /* row_info: */
                /*  png_uint_32 width;       width of row */
                /*  size_t rowbytes;         number of bytes in row */
                /*  png_byte color_type;     color type of pixels */
                /*  png_byte bit_depth;      bit depth of samples */
                /*  png_byte channels;       number of channels (1-4) */
                /*  png_byte pixel_depth;    bits per pixel (depth*channels) */
             png_ptr->row_buf + 1);      /* start of pixel data for row */
#endif

#ifdef PNG_WRITE_BACKGROUND_SUPPORTED
   if ((png_ptr->transformations & PNG_WRITE_COMPOSE) != 0)
      png_do_write_compose_background(row_info, png_ptr->row_buf + 1,
          &(png_ptr->write_background),
          png_ptr->write_background_keep_alpha);
#endif

#ifdef PNG_WRITE_FILLER_SUPPORTED
   if ((png_ptr->transformations & PNG_FILLER) != 0)
      png_do_strip_channel(row_info, png_ptr->row_buf + 1,
          !(png_ptr->flags & PNG_FLAG_FILLER_AFTER));
#endif

#ifdef PNG_WRITE_PACKSWAP_SUPPORTED
   if ((png_ptr->transformations & PNG_PACKSWAP) != 0)
      png_do_packswap(row_info, png_ptr->row_buf + 1);
#endif

#ifdef PNG_WRITE_PACK_SUPPORTED
   if ((png_ptr->transformations & PNG_PACK) != 0)
      png_do_pack(row_info, png_ptr->row_buf + 1,
          (png_uint_32)png_ptr->bit_depth);
#endif

#ifdef PNG_WRITE_SWAP_SUPPORTED
#  ifdef PNG_16BIT_SUPPORTED
   if ((png_ptr->transformations & PNG_SWAP_BYTES) != 0)
      png_do_swap(row_info, png_ptr->row_buf + 1);
#  endif
#endif

#ifdef PNG_WRITE_SHIFT_SUPPORTED
   if ((png_ptr->transformations & PNG_SHIFT) != 0)
      png_do_shift(row_info, png_ptr->row_buf + 1,
           &(png_ptr->shift));
#endif

#ifdef PNG_WRITE_SWAP_ALPHA_SUPPORTED
   if ((png_ptr->transformations & PNG_SWAP_ALPHA) != 0)
      png_do_write_swap_alpha(row_info, png_ptr->row_buf + 1);
#endif

#ifdef PNG_WRITE_INVERT_ALPHA_SUPPORTED
   if ((png_ptr->transformations & PNG_INVERT_ALPHA) != 0)
      png_do_write_invert_alpha(row_info, png_ptr->row_buf + 1);
#endif

#ifdef PNG_WRITE_BGR_SUPPORTED
   if ((png_ptr->transformations & PNG_BGR) != 0)
      png_do_bgr(row_info, png_ptr->row_buf + 1);
#endif

#ifdef PNG_WRITE_INVERT_SUPPORTED
   if ((png_ptr->transformations & PNG_INVERT_MONO) != 0)
      png_do_invert(row_info, png_ptr->row_buf + 1);
#endif
}
#endif /* WRITE_TRANSFORMS */
#endif /* WRITE */
