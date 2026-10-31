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

#ifndef h_frame
#define h_frame

#include <stddef.h>

#include <mcpc/_c23_keywords.h>

// Splits a byte stream into complete top-level JSON objects. It understands
// strings and escapes, so braces inside string values never confuse it, and it
// accepts both newline-delimited and back-to-back objects. Bytes between
// objects (whitespace, stray text) are skipped.
typedef struct mcpc_framer
{
  char *buf;
  size_t len;
  size_t cap;
  size_t max;			// largest accepted object, in bytes
  unsigned depth;
  bool in_str;
  bool esc;
  bool overflow;		// current object exceeded max, bytes are dropped until it ends
} mcpc_framer_t;

enum mcpc_frame_stat
{
  MCPC_FRAME_MORE = 0,		// need more bytes
  MCPC_FRAME_READY,		// frame.buf[0..len) holds one NUL-terminated object
  MCPC_FRAME_TOOBIG,		// an object larger than max was dropped
  MCPC_FRAME_NOMEM,
};
typedef enum mcpc_frame_stat mcpc_frame_stat_t;

#define MCPC_FRAME_DEFAULT_MAX (16u * 1024u * 1024u)

void mcpc_framer_init (mcpc_framer_t * f, size_t max);

void mcpc_framer_free (mcpc_framer_t * f);

// Feed one byte. After MCPC_FRAME_READY the caller consumes the frame and
// calls mcpc_framer_next before feeding more.
mcpc_frame_stat_t mcpc_framer_push (mcpc_framer_t * f, unsigned char c);

void mcpc_framer_next (mcpc_framer_t * f);

#endif
