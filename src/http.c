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

#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "hmac.h"

static bool
is_tchar (unsigned char c)
{
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
    return true;
  return c != 0 && strchr ("!#$%&'*+-.^_`|~", c) != nullptr;
}

static bool
ieq (const char *a, size_t alen, const char *b)
{
  size_t i = 0;
  for (; i < alen && b[i] != 0; i++)
    {
      unsigned char x = (unsigned char) a[i], y = (unsigned char) b[i];
      if (x >= 'A' && x <= 'Z')
	x = (unsigned char) (x + 32);
      if (y >= 'A' && y <= 'Z')
	y = (unsigned char) (y + 32);
      if (x != y)
	return false;
    }
  return i == alen && b[i] == 0;
}

// token list membership ("close", "keep-alive", "chunked", ...) in a header value
static bool
has_token (const char *v, size_t len, const char *tok)
{
  const size_t tl = strlen (tok);
  size_t i = 0;
  while (i < len)
    {
      while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == ','))
	i++;
      size_t j = i;
      while (j < len && v[j] != ',' && v[j] != ' ' && v[j] != '\t')
	j++;
      if (j > i && j - i == tl && ieq (v + i, j - i, tok))
	return true;
      i = j;
    }
  return false;
}

const char *
mcpc_http_hdr (const struct mcpc_http_req *rq, const char *name, size_t *len)
{
  for (size_t i = 0; i < rq->nhdrs; i++)
    if (ieq (rq->hdrs[i].name, rq->hdrs[i].name_len, name))
      {
	if (len != nullptr)
	  *len = rq->hdrs[i].val_len;
	return rq->hdrs[i].val;
      }
  return nullptr;
}

int
mcpc_http_parse_head (const char *buf, size_t len, struct mcpc_http_req *rq, const char **err)
{
  static const char *none = "";
  if (err == nullptr)
    err = &none;
  *err = "";

  // the head ends at the first empty line; look only as far as the limit allows
  const size_t scan = len < MCPC_HTTP_MAX_HEAD ? len : MCPC_HTTP_MAX_HEAD;
  size_t end = 0;
  bool found = false;
  for (size_t i = 0; i + 4 <= scan; i++)
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
      {
	end = i + 4;
	found = true;
	break;
      }
  // strict framing, checked on whatever has arrived so a bad head is refused at once instead of waiting
  // for a terminator that a bare-LF sender never writes: every LF follows a CR, every CR is followed by
  // LF (a CR at the very end may still get its LF), no NUL
  const size_t check_to = found ? end : scan;
  for (size_t i = 0; i < check_to; i++)
    {
      const char ch = buf[i];
      if (ch == 0 || (ch == '\n' && (i == 0 || buf[i - 1] != '\r')) || (ch == '\r' && i + 1 < check_to && buf[i + 1] != '\n'))
	{
	  *err = "bare CR or LF in request head";
	  return MCPC_HTTP_BAD;
	}
    }

  if (!found)
    {
      if (len >= MCPC_HTTP_MAX_HEAD)
	{
	  *err = "request head too large";
	  return MCPC_HTTP_TOOBIG;
	}
      return MCPC_HTTP_INCOMPLETE;
    }

  memset (rq, 0, sizeof (*rq));
  rq->head_len = end;

  // request line: METHOD SP target SP HTTP/1.x
  size_t p = 0;
  size_t m = 0;
  while (p < end && buf[p] != ' ')
    {
      if (!is_tchar ((unsigned char) buf[p]) || m + 1 >= sizeof (rq->method))
	{
	  *err = "bad method";
	  return MCPC_HTTP_BAD;
	}
      rq->method[m++] = buf[p++];
    }
  if (m == 0 || p >= end)
    {
      *err = "bad request line";
      return MCPC_HTTP_BAD;
    }
  p++;
  const size_t t0 = p;
  while (p < end && buf[p] != ' ' && buf[p] != '\r')
    {
      const unsigned char c = (unsigned char) buf[p];
      if (c < 0x21 || c == 0x7f)
	{
	  *err = "bad request target";
	  return MCPC_HTTP_BAD;
	}
      p++;
    }
  if (p == t0 || buf[t0] != '/' || p >= end || buf[p] != ' ')
    {
      *err = "bad request target";
      return MCPC_HTTP_BAD;
    }
  rq->target = buf + t0;
  rq->target_len = p - t0;
  p++;
  if (end - p < 10 || memcmp (buf + p, "HTTP/1.", 7) != 0 || (buf[p + 7] != '0' && buf[p + 7] != '1') || buf[p + 8] != '\r')
    {
      *err = "unsupported HTTP version";
      return MCPC_HTTP_BAD;
    }
  rq->minor = buf[p + 7] - '0';
  p += 10;			// version CRLF

  // header fields
  while (p + 2 < end)
    {
      if (buf[p] == ' ' || buf[p] == '\t')
	{
	  *err = "obsolete line folding";
	  return MCPC_HTTP_BAD;
	}
      const size_t n0 = p;
      while (p < end && buf[p] != ':' && buf[p] != '\r')
	{
	  if (!is_tchar ((unsigned char) buf[p]))
	    {
	      *err = "bad header name";
	      return MCPC_HTTP_BAD;
	    }
	  p++;
	}
      if (p == n0 || buf[p] != ':')
	{
	  *err = "bad header line";
	  return MCPC_HTTP_BAD;
	}
      if (rq->nhdrs >= MCPC_HTTP_MAX_HEADERS)
	{
	  *err = "too many headers";
	  return MCPC_HTTP_TOOBIG;
	}
      struct mcpc_http_hdr *h = &rq->hdrs[rq->nhdrs++];
      h->name = buf + n0;
      h->name_len = p - n0;
      p++;
      while (p < end && (buf[p] == ' ' || buf[p] == '\t'))
	p++;
      const size_t v0 = p;
      while (p < end && buf[p] != '\r')
	{
	  const unsigned char c = (unsigned char) buf[p];
	  if ((c < 0x20 && c != '\t') || c == 0x7f)
	    {
	      *err = "control character in header value";
	      return MCPC_HTTP_BAD;
	    }
	  p++;
	}
      size_t v1 = p;
      while (v1 > v0 && (buf[v1 - 1] == ' ' || buf[v1 - 1] == '\t'))
	v1--;
      h->val = buf + v0;
      h->val_len = v1 - v0;
      p += 2;			// CRLF
    }

  // framing headers
  size_t cl_count = 0, te_count = 0;
  for (size_t i = 0; i < rq->nhdrs; i++)
    {
      const struct mcpc_http_hdr *h = &rq->hdrs[i];
      if (ieq (h->name, h->name_len, "content-length"))
	{
	  size_t v = 0;
	  if (h->val_len == 0 || h->val_len > 15)
	    {
	      *err = "bad Content-Length";
	      return MCPC_HTTP_BAD;
	    }
	  for (size_t k = 0; k < h->val_len; k++)
	    {
	      if (h->val[k] < '0' || h->val[k] > '9')
		{
		  *err = "bad Content-Length";
		  return MCPC_HTTP_BAD;
		}
	      v = v * 10 + (size_t) (h->val[k] - '0');
	    }
	  // a repeated Content-Length is only tolerable when it says the same thing
	  if (cl_count++ > 0 && v != rq->content_length)
	    {
	      *err = "conflicting Content-Length";
	      return MCPC_HTTP_BAD;
	    }
	  rq->content_length = v;
	  rq->has_cl = true;
	}
      else if (ieq (h->name, h->name_len, "transfer-encoding"))
	{
	  te_count++;
	  if (!ieq (h->val, h->val_len, "chunked"))
	    {
	      *err = "unsupported Transfer-Encoding";
	      return MCPC_HTTP_UNSUPPORTED;
	    }
	  rq->chunked = true;
	}
    }
  if (te_count > 1 || (rq->chunked && rq->has_cl))
    {
      *err = "ambiguous message framing";
      return MCPC_HTTP_BAD;
    }
  if (rq->chunked && rq->minor == 0)
    {
      *err = "chunked body in HTTP/1.0";
      return MCPC_HTTP_BAD;
    }

  // Host is mandatory in HTTP/1.1
  size_t hl = 0;
  const char *host = mcpc_http_hdr (rq, "host", &hl);
  if (rq->minor == 1 && (host == nullptr || hl == 0))
    {
      *err = "missing Host";
      return MCPC_HTTP_BAD;
    }

  size_t cl = 0;
  const char *conn = mcpc_http_hdr (rq, "connection", &cl);
  rq->keep_alive = rq->minor == 1 ? !(conn != nullptr && has_token (conn, cl, "close")) :
    (conn != nullptr && has_token (conn, cl, "keep-alive"));

  size_t el = 0;
  const char *expect = mcpc_http_hdr (rq, "expect", &el);
  rq->expect_continue = expect != nullptr && ieq (expect, el, "100-continue");
  if (expect != nullptr && !rq->expect_continue)
    {
      *err = "unsupported Expect";
      return MCPC_HTTP_UNSUPPORTED;
    }

  return MCPC_HTTP_OK;
}

/* = = = = = = = = = = = = = = chunked bodies = = = = = = = = = = = = = = */

enum
{
  CH_SIZE = 0,			// hex digits of the chunk size
  CH_EXT,			// chunk extension, skipped up to CR
  CH_SIZE_LF,
  CH_DATA,
  CH_DATA_CR,
  CH_DATA_LF,
  CH_TRAILER_START,		// start of a trailer line, or the final empty line
  CH_TRAILER,			// inside a trailer line
  CH_TRAILER_LF,
  CH_END_LF,
  CH_DONE
};

void
mcpc_chunk_init (struct mcpc_chunk_dec *d, size_t max)
{
  memset (d, 0, sizeof (*d));
  d->max = max;
}

int
mcpc_chunk_feed (struct mcpc_chunk_dec *d, const char *in, size_t n, size_t *consumed, char *out, size_t *out_len)
{
  size_t i = 0, o = 0;
  int rc = MCPC_HTTP_INCOMPLETE;

  while (i < n && d->state != CH_DONE)
    {
      const unsigned char c = (unsigned char) in[i];
      switch (d->state)
	{
	case CH_SIZE:
	  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
	    {
	      if (d->size_digits >= 8)
		{
		  rc = MCPC_HTTP_TOOBIG;
		  goto done;
		}
	      d->left = d->left * 16 + (size_t) (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
	      d->size_digits++;
	      i++;
	    }
	  else if (d->size_digits > 0 && (c == ';' || c == '\r'))
	    d->state = c == ';' ? CH_EXT : CH_SIZE_LF;
	  else
	    {
	      rc = MCPC_HTTP_BAD;
	      goto done;
	    }
	  if (d->state == CH_EXT || d->state == CH_SIZE_LF)
	    i++;
	  break;
	case CH_EXT:
	  if (c == '\r')
	    d->state = CH_SIZE_LF;
	  else if (c == '\n' || c == 0)
	    {
	      rc = MCPC_HTTP_BAD;
	      goto done;
	    }
	  i++;
	  break;
	case CH_SIZE_LF:
	  if (c != '\n')
	    {
	      rc = MCPC_HTTP_BAD;
	      goto done;
	    }
	  i++;
	  d->size_digits = 0;
	  d->state = d->left == 0 ? CH_TRAILER_START : CH_DATA;
	  if (d->left > d->max || d->total + d->left > d->max)
	    {
	      rc = MCPC_HTTP_TOOBIG;
	      goto done;
	    }
	  break;
	case CH_DATA:
	  {
	    size_t take = n - i < d->left ? n - i : d->left;
	    memcpy (out + o, in + i, take);
	    o += take;
	    i += take;
	    d->left -= take;
	    d->total += take;
	    if (d->left == 0)
	      d->state = CH_DATA_CR;
	    break;
	  }
	case CH_DATA_CR:
	  if (c != '\r')
	    {
	      rc = MCPC_HTTP_BAD;
	      goto done;
	    }
	  i++;
	  d->state = CH_DATA_LF;
	  break;
	case CH_DATA_LF:
	  if (c != '\n')
	    {
	      rc = MCPC_HTTP_BAD;
	      goto done;
	    }
	  i++;
	  d->state = CH_SIZE;
	  break;
	case CH_TRAILER_START:
	  if (c == '\r')
	    d->state = CH_END_LF;
	  else
	    d->state = CH_TRAILER;
	  i++;
	  break;
	case CH_TRAILER:
	  if (c == '\r')
	    d->state = CH_TRAILER_LF;
	  else if (c == '\n' || c == 0)
	    {
	      rc = MCPC_HTTP_BAD;
	      goto done;
	    }
	  i++;
	  break;
	case CH_TRAILER_LF:
	  if (c != '\n')
	    {
	      rc = MCPC_HTTP_BAD;
	      goto done;
	    }
	  i++;
	  d->state = CH_TRAILER_START;
	  break;
	case CH_END_LF:
	  if (c != '\n')
	    {
	      rc = MCPC_HTTP_BAD;
	      goto done;
	    }
	  i++;
	  d->state = CH_DONE;
	  break;
	default:
	  rc = MCPC_HTTP_BAD;
	  goto done;
	}
    }
  if (d->state == CH_DONE)
    rc = MCPC_HTTP_OK;
done:
  *consumed = i;
  *out_len = o;
  return rc;
}

/* = = = = = = = = = = = = = = header values = = = = = = = = = = = = = = */

char *
mcpc_http_decode_value (const char *val, size_t len, size_t *out_len)
{
  char *res;
  if (len >= 11 && memcmp (val, "=?base64?", 9) == 0 && memcmp (val + len - 2, "?=", 2) == 0)
    {
      const size_t enc = len - 11;
      uint8_t *tmp = mcpc_alloc (enc / 4 * 3 + 4);
      if (tmp == nullptr)
	return nullptr;
      const long n = mcpc_b64_decode (val + 9, enc, tmp);
      if (n < 0)
	{
	  free (tmp);
	  return nullptr;
	}
      tmp[n] = 0;
      if (out_len != nullptr)
	*out_len = (size_t) n;
      return (char *) tmp;
    }
  res = mcpc_alloc (len + 1);
  if (res == nullptr)
    return nullptr;
  memcpy (res, val, len);
  res[len] = 0;
  if (out_len != nullptr)
    *out_len = len;
  return res;
}

/* = = = = = = = = = = = = = = reply channel = = = = = = = = = = = = = = */

static const char *
reason (int status)
{
  switch (status)
    {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

void
mcpc_http_chan_init (struct mcpc_http_chan *ch, mcpc_fd_t fd)
{
  memset (ch, 0, sizeof (*ch));
  ch->fd = fd;
  ch->status = 200;
  ch->hdr_pver = MCPC_PV_2025_03_26;
}

static void
chan_write (struct mcpc_http_chan *ch, const void *p, size_t n)
{
  if (!ch->broken && !mcpc_net_write (ch->fd, p, n))
    ch->broken = true;
}

static void
chan_chunk (struct mcpc_http_chan *ch, const char *p, size_t n)
{
  if (!ch->chunked)
    {
      chan_write (ch, p, n);
      return;
    }
  char head[24];
  const int hl = snprintf (head, sizeof (head), "%zx\r\n", n);
  // one write per chunk keeps small events in a single packet
  char *buf = mcpc_alloc ((size_t) hl + n + 2);
  if (buf == nullptr)
    {
      ch->broken = true;
      return;
    }
  memcpy (buf, head, (size_t) hl);
  memcpy (buf + hl, p, n);
  memcpy (buf + hl + n, "\r\n", 2);
  chan_write (ch, buf, (size_t) hl + n + 2);
  free (buf);
}

static void
chan_start_sse (struct mcpc_http_chan *ch)
{
  char head[400];
  char sess[64] = "";
  if (ch->session_out[0] != 0)
    snprintf (sess, sizeof (sess), "Mcp-Session-Id: %s\r\n", ch->session_out);
  const int hl = snprintf (head, sizeof (head),
			   "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache, no-transform\r\n"
			   "X-Accel-Buffering: no\r\n%s%s%s\r\n", sess, ch->chunked ? "Transfer-Encoding: chunked\r\n" : "",
			   ch->keep_alive && ch->chunked ? "" : "Connection: close\r\n");
  chan_write (ch, head, (size_t) hl);
  ch->state = MCPC_CHAN_SSE;
}

static void
chan_sse_event (struct mcpc_http_chan *ch, const char *msg, size_t len)
{
  // "event: message\ndata: <json>\n\n"; the JSON holds no raw newline, strings escape them
  while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
    len--;
  char *buf = mcpc_alloc (len + 32);
  if (buf == nullptr)
    {
      ch->broken = true;
      return;
    }
  size_t n = (size_t) snprintf (buf, 32, "event: message\ndata: ");
  memcpy (buf + n, msg, len);
  n += len;
  memcpy (buf + n, "\n\n", 2);
  n += 2;
  chan_chunk (ch, buf, n);
  free (buf);
}

void
mcpc_http_chan_event (struct mcpc_http_chan *ch, const char *msg, size_t len)
{
  if (ch->state == MCPC_CHAN_DONE || ch->broken)
    return;
  if (ch->state == MCPC_CHAN_IDLE)
    {
      if (!ch->sse_ok)
	return;			// a plain JSON answer cannot carry notifications
      chan_start_sse (ch);
    }
  chan_sse_event (ch, msg, len);
}

void
mcpc_http_chan_end (struct mcpc_http_chan *ch)
{
  if (ch->state == MCPC_CHAN_SSE && ch->chunked)
    chan_write (ch, "0\r\n\r\n", 5);
  ch->state = MCPC_CHAN_DONE;
}

void
mcpc_http_chan_comment (struct mcpc_http_chan *ch)
{
  if (ch->state == MCPC_CHAN_SSE && !ch->broken)
    chan_chunk (ch, ":\r\n", 3);
}

void
mcpc_http_chan_final (struct mcpc_http_chan *ch, const char *msg, size_t len)
{
  if (ch->state == MCPC_CHAN_DONE || ch->broken)
    return;
  while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
    len--;
  ch->responded = true;

  if (ch->state == MCPC_CHAN_SSE)
    {
      chan_sse_event (ch, msg, len);
      mcpc_http_chan_end (ch);
      return;
    }

  char head[320];
  char sess[64] = "";
  if (ch->session_out[0] != 0)
    snprintf (sess, sizeof (sess), "Mcp-Session-Id: %s\r\n", ch->session_out);
  const int hl = snprintf (head, sizeof (head),
			   "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n%s%s\r\n",
			   ch->status, reason (ch->status), len, sess, ch->keep_alive ? "" : "Connection: close\r\n");
  char *buf = mcpc_alloc ((size_t) hl + len);
  if (buf == nullptr)
    {
      ch->broken = true;
      return;
    }
  memcpy (buf, head, (size_t) hl);
  memcpy (buf + hl, msg, len);
  chan_write (ch, buf, (size_t) hl + len);
  free (buf);
  ch->state = MCPC_CHAN_DONE;
}
