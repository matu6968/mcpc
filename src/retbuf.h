// This file is part of the mcpc library.
//
// Copyright (C) 2025 Michael Lee <micl2e2 AT proton.me>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions: 
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software. 
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef h_retbuf
#define h_retbuf

#include <stdarg.h>

#include <mcpc/_c23_uchar.h>

#include "ext_stdint.h"
#include "mjson.h"

// Growable output buffer. Every write reallocates on demand, so serialized
// output is never truncated. A write that cannot be satisfied (allocation
// failure or size cap exceeded) sets the error flag, see retbuf_is_ok.
typedef struct retbuf
{
  struct mjson_fixedbuf fb;
  bool oom;
} *retbuf_t;

// upper bound of a single buffer (mjson_fixedbuf uses int sizes)
#define MCPC_RETBUF_MAXCAP (1024 * 1024 * 1024)

// mjson print callback appending to a growable mjson_fixedbuf, keeps NUL terminator
int mcpc_print_grow (const char *ptr, int len, void *fn_data);

// release the storage owned by a mjson_fixedbuf filled by mcpc_print_grow
void mcpc_print_grow_free (struct mjson_fixedbuf *fb);

retbuf_t retbuf_new (size_t cap_hint);

void retbuf_free (retbuf_t rtbf);

const char *retbuf_data (const retbuf_t rtbf);

size_t retbuf_len (const retbuf_t rtbf);

bool retbuf_is_ok (const retbuf_t rtbf);

u8_t rtbuf_printf_mjfmt (retbuf_t rtbf, const char8_t * u8str, ...);

u8_t rtbuf_vprintf_mjfmt (retbuf_t rtbf, const char8_t * fmt8, va_list * vargs);


#endif
