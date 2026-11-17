/* Hand-written config for building libmp3lame 3.100 inside MicFX (no autoconf). */
#ifndef MICFX_LAME_CONFIG_H
#define MICFX_LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define SIZEOF_DOUBLE 8
#define SIZEOF_FLOAT 4
#define SIZEOF_INT 4
#define SIZEOF_LONG_DOUBLE 16
#define SIZEOF_SHORT 2
#define SIZEOF_UNSIGNED_INT 4
#define SIZEOF_UNSIGNED_LONG 8
#define SIZEOF_UNSIGNED_SHORT 2
#define LAME_LIBRARY_BUILD 1

typedef float ieee754_float32_t;
typedef double ieee754_float64_t;

#endif
