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

#ifndef h_http
#define h_http

#include <stddef.h>

#include <mcpc/_c23_keywords.h>

#include "net.h"
#include "proto.h"

// ---------------------------------------------------------------------------------------------
// A small, strict HTTP/1.1 request reader. It exists to serve one endpoint safely rather than to
// be a general server: anything ambiguous (the things request smuggling feeds on) is rejected.

#define MCPC_HTTP_MAX_HEADERS 64
#define MCPC_HTTP_MAX_HEAD 16384

struct mcpc_http_hdr
{
  const char *name;
  size_t name_len;
  const char *val;
  size_t val_len;
};

struct mcpc_http_req
{
  char method[16];
  const char *target;		// request target, points into the buffer that was parsed
  size_t target_len;
  int minor;			// HTTP/1.<minor>
  struct mcpc_http_hdr hdrs[MCPC_HTTP_MAX_HEADERS];
  size_t nhdrs;
  size_t head_len;		// bytes up to and including the blank line
  bool has_cl;
  size_t content_length;
  bool chunked;
  bool keep_alive;
  bool expect_continue;
};

enum mcpc_http_parse
{
  MCPC_HTTP_OK = 0,
  MCPC_HTTP_INCOMPLETE = 1,	// need more bytes
  MCPC_HTTP_BAD = -1,		// malformed or ambiguous, *err says why
  MCPC_HTTP_TOOBIG = -2,	// head larger than MCPC_HTTP_MAX_HEAD or too many headers
  MCPC_HTTP_UNSUPPORTED = -3,	// valid but not something we implement (e.g. a Transfer-Encoding other than chunked)
};

int mcpc_http_parse_head (const char *buf, size_t len, struct mcpc_http_req *rq, const char **err);

// Value of the first header called name (compared without regard to case), NULL if absent.
const char *mcpc_http_hdr (const struct mcpc_http_req *rq, const char *name, size_t *len);

// Incremental decoder of a chunked body.
struct mcpc_chunk_dec
{
  int state;
  size_t left;			// bytes still due in the current chunk
  size_t size_digits;
  size_t total;			// decoded bytes so far
  size_t max;			// limit on total
};

void mcpc_chunk_init (struct mcpc_chunk_dec *d, size_t max);

// Decode from in[0..n) appending to out (which has room for n bytes). *consumed is how much of in
// was used; *out_len how much was written. Returns MCPC_HTTP_OK once the final chunk and trailers
// are done, MCPC_HTTP_INCOMPLETE when more input is needed, MCPC_HTTP_BAD / MCPC_HTTP_TOOBIG otherwise.
int mcpc_chunk_feed (struct mcpc_chunk_dec *d, const char *in, size_t n, size_t *consumed, char *out, size_t *out_len);

// "=?base64?...?=" header values (Mcp-Name, Mcp-Param-*): returns a heap string with the decoded value,
// or a copy of the plain value; NULL on invalid input.
char *mcpc_http_decode_value (const char *val, size_t len, size_t *out_len);

// ---------------------------------------------------------------------------------------------
// The reply channel of the request being served on a connection. The core writes notifications and
// the final response through it, whatever the transport; here that means choosing between a plain JSON
// answer and an SSE stream, and between the status codes.

enum mcpc_chan_state
{
  MCPC_CHAN_IDLE = 0,		// nothing sent yet
  MCPC_CHAN_SSE,		// event stream started, more events may follow
  MCPC_CHAN_DONE,		// the response is complete
};

struct mcpc_http_chan
{
  mcpc_fd_t fd;
  int state;
  int status;			// HTTP status of a plain JSON final response
  bool sse_ok;			// the client accepts text/event-stream
  bool chunked;			// events go out in chunked encoding (HTTP/1.1), else the stream ends with the connection
  bool keep_alive;
  bool broken;			// a write failed, the peer is gone
  bool responded;		// a final JSON-RPC response was produced for the request
  bool listen_open;		// the request opened a subscriptions/listen stream that outlives it
  mcpc_pver_t hdr_pver;		// protocol version named by the MCP-Protocol-Version header of a legacy request
  char session[33];		// Mcp-Session-Id of the request, empty when it has none
  char session_out[33];		// session created by this request (initialize), to be named in the response
  char *caps;			// client capabilities stored with the session, copied for the request (heap, owned by the connection)
  size_t caps_len;
};

void mcpc_http_chan_init (struct mcpc_http_chan *ch, mcpc_fd_t fd);

// One JSON-RPC message (without trailing newline) as an SSE event; starts the stream when needed.
// Dropped when the client cannot take a stream.
void mcpc_http_chan_event (struct mcpc_http_chan *ch, const char *msg, size_t len);

// The final response of the request.
void mcpc_http_chan_final (struct mcpc_http_chan *ch, const char *msg, size_t len);

// SSE comment line as keep-alive.
void mcpc_http_chan_comment (struct mcpc_http_chan *ch);

// End of an event stream.
void mcpc_http_chan_end (struct mcpc_http_chan *ch);

#endif
