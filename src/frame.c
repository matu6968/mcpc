// This file is part of the mcpc library.
//
// Copyright (C) 2026 matu6968
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

#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "frame.h"

#define FRAME_INIT_CAP 1024

void
mcpc_framer_init (mcpc_framer_t *f, size_t max)
{
  memset (f, 0, sizeof (*f));
  f->max = max > 0 ? max : MCPC_FRAME_DEFAULT_MAX;
}

void
mcpc_framer_init_lines (mcpc_framer_t *f, size_t max)
{
  mcpc_framer_init (f, max);
  f->line_mode = true;
}

void
mcpc_framer_free (mcpc_framer_t *f)
{
  free (f->buf);
  memset (f, 0, sizeof (*f));
}

void
mcpc_framer_next (mcpc_framer_t *f)
{
  f->len = 0;
  f->depth = 0;
  f->in_str = false;
  f->esc = false;
  f->overflow = false;
}

static bool
_framer_append (mcpc_framer_t *f, char c)
{
  // keep one spare byte for the NUL terminator
  if (f->len + 2 > f->cap)
    {
      size_t ncap = f->cap > 0 ? f->cap * 2 : FRAME_INIT_CAP;
      char *nb = mcpc_realloc (f->buf, ncap);
      if (nb == nullptr)
	return false;
      f->buf = nb;
      f->cap = ncap;
    }
  f->buf[f->len++] = c;
  return true;
}

mcpc_frame_stat_t
mcpc_framer_push (mcpc_framer_t *f, unsigned char c)
{
  if (f->depth == 0 && !f->overflow)
    {
      if (c != '{')
	return MCPC_FRAME_MORE;	// between objects
    }
  else if (f->line_mode && c == '\n')
    {
      // the object is cut short by the end of the line: forget it and start over at the next one
      const bool had_overflow = f->overflow;
      mcpc_framer_next (f);
      return had_overflow ? MCPC_FRAME_TOOBIG : MCPC_FRAME_BROKEN;
    }

  // track structure first so an oversized object is still skipped to its end
  bool closes = false;
  if (f->in_str)
    {
      if (f->esc)
	f->esc = false;
      else if (c == '\\')
	f->esc = true;
      else if (c == '"')
	f->in_str = false;
    }
  else if (c == '"')
    f->in_str = true;
  else if (c == '{')
    f->depth++;
  else if (c == '}')
    {
      if (f->depth > 0)
	f->depth--;
      closes = (f->depth == 0);
    }

  if (!f->overflow)
    {
      if (f->len >= f->max)
	{
	  f->overflow = true;
	  f->len = 0;
	}
      else if (!_framer_append (f, (char) c))
	{
	  mcpc_framer_next (f);
	  return MCPC_FRAME_NOMEM;
	}
    }

  if (closes)
    {
      if (f->overflow)
	{
	  mcpc_framer_next (f);
	  return MCPC_FRAME_TOOBIG;
	}
      f->buf[f->len] = '\0';
      return MCPC_FRAME_READY;
    }

  return MCPC_FRAME_MORE;
}
