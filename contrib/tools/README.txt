PNG tools
=========

This directory contains tools contributed by the authors of libpng.
Each file states its own copyright and license.

pngautolevel
------------

pngautolevel applies percentile-based auto-levels (a contrast stretch)
to a 16-bit grayscale PNG.  It builds a full-resolution histogram with one
64-bit counter per possible sample value (65536 buckets), finds the low and
high cut points at configurable percentiles, and remaps every pixel through
the resulting stretch table.  The histogram and the mapping table can be
written out for inspection.

Usage:

    pngautolevel [--clip-low=PER-MILLE] [--clip-high=PER-MILLE]
                 [--histogram=FILE] [--lut=FILE] input.png output.png

The clip ratios are in parts per thousand, independently configurable for
each end, and default to 5 (0.5%).  Exactly floor(total * ratio / 1000)
samples are clipped at each end; the cut-point samples themselves are
retained and map to 0 and 65535.  --histogram writes the 65536 bucket
counts and --lut writes the 65536 output values of the mapping table, one
number per line.

Only 16-bit grayscale images, with or without an alpha channel, are
accepted; the alpha channel is passed through unchanged.

The histogram implementation itself lives in contrib/libtests/pnghist16.c
and does not depend on libpng, so it can be reused outside a PNG pipeline.
