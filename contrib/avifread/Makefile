# Makefile for contrib/avifread - AVIF still-image reader + test driver.
#
# Builds against an unmodified libpng.  By default it uses the build tree
# in ../../build-base (cmake -B build-base -S . && cmake --build build-base);
# override with PNG_SRC=/path/to/libpng PNG_BUILD=/path/to/build.
#
# No AVIF/AV1 library is needed at build time: the AV1 decoder (libdav1d)
# is dlopen()ed at run time.

PNG_SRC   ?= ../..
PNG_BUILD ?= ../../build-base

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
CPPFLAGS += -I$(PNG_SRC) -I$(PNG_BUILD)

LIBPNG_A := $(firstword $(wildcard $(PNG_BUILD)/libpng18.a \
            $(PNG_BUILD)/libpng16.a $(PNG_BUILD)/libpng.a))

all: avif2png

avif2png: avif2png.c pngavif.c pngavif.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ avif2png.c pngavif.c \
	    $(LIBPNG_A) -lz -lm -ldl

selftest: avif2png
	./avif2png --selftest

clean:
	rm -f avif2png

.PHONY: all selftest clean
