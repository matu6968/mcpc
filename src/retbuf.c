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

#include "retbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "mjson.h"

int
mcpc_print_grow (const char *ptr, int len, void *fn_data)
{
  struct mjson_fixedbuf *fb = fn_data;

  if (len <= 0)
    return 0;

  const size_t need = (size_t) fb->len + (size_t) len + 1;	// +1 for NUL
  if (need > (size_t) fb->size)
    {
      if (need > MCPC_RETBUF_MAXCAP)
	return 0;

      size_t ncap = fb->size > 0 ? (size_t) fb->size : 256;
      while (ncap < need)
	ncap *= 2;
      if (ncap > MCPC_RETBUF_MAXCAP)
	ncap = MCPC_RETBUF_MAXCAP;

      char *np = mcpc_realloc (fb->ptr, ncap);
      if (np == nullptr)
	return 0;
      fb->ptr = np;
      fb->size = (int) ncap;
    }

  memcpy (fb->ptr + fb->len, ptr, (size_t) len);
  fb->len += len;
  fb->ptr[fb->len] = '\0';
  return len;
}

void
mcpc_print_grow_free (struct mjson_fixedbuf *fb)
{
  free (fb->ptr);
  fb->ptr = nullptr;
  fb->size = 0;
  fb->len = 0;
}

retbuf_t
retbuf_new (size_t cap_hint)
{
  retbuf_t x = mcpc_alloc (sizeof (struct retbuf));
  if (x == nullptr)
    return nullptr;

  x->fb.ptr = nullptr;
  x->fb.size = 0;
  x->fb.len = 0;
  x->oom = false;

  // reserve the hint up front, an empty buffer is still a valid NUL-terminated string
  if (cap_hint > 0 && cap_hint < MCPC_RETBUF_MAXCAP)
    {
      x->fb.ptr = mcpc_alloc (cap_hint + 1);
      if (x->fb.ptr != nullptr)
	{
	  x->fb.size = (int) (cap_hint + 1);
	  x->fb.ptr[0] = '\0';
	}
    }

  return x;
}

void
retbuf_free (retbuf_t rtbf)
{
  if (rtbf == nullptr)
    return;
  mcpc_print_grow_free (&rtbf->fb);
  free (rtbf);
}

const char *
retbuf_data (const retbuf_t rtbf)
{
  return rtbf->fb.ptr != nullptr ? rtbf->fb.ptr : "";
}

size_t
retbuf_len (const retbuf_t rtbf)
{
  return (size_t) rtbf->fb.len;
}

bool
retbuf_is_ok (const retbuf_t rtbf)
{
  return !rtbf->oom;
}

// like mcpc_print_grow, but a short write marks the whole buffer as failed
static int
retbuf_sink (const char *ptr, int len, void *fn_data)
{
  retbuf_t rtbf = fn_data;
  int n = mcpc_print_grow (ptr, len, &rtbf->fb);
  if (n < len)
    rtbf->oom = true;
  return n;
}

u8_t
rtbuf_vprintf_mjfmt (retbuf_t rtbf, const char8_t *fmt8, va_list *vargs)
{
  const char *fmt = (const char *) fmt8;
  mjson_vprintf (&retbuf_sink, rtbf, fmt, vargs);
  return rtbf->oom ? 1 : 0;
}

u8_t
rtbuf_printf_mjfmt (retbuf_t rtbf, const char8_t *fmt8, ...)
{
  va_list args;
  va_start (args, fmt8);
  u8_t ret = rtbuf_vprintf_mjfmt (rtbf, fmt8, &args);
  va_end (args);
  return ret;
}
