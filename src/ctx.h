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

#ifndef h_ctx
#define h_ctx

#include <mcpc/ctx.h>

#include "server.h"

// State of the request a callback is serving. Lives on the stack of the handler; the
// pointers refer to the request frame, which stays valid until the handler returns.
struct mcpc_ctx
{
  mcpc_server_t *sv;
  mcpc_conn_t *conn;
  mcpc_sock_t csock;
  unsigned feat;		// MCPC_FEAT_* of the negotiated revision
  const char *args;		// "arguments" object
  int args_len;
  const char *token;		// params._meta.progressToken as raw JSON, NULL when not requested
  int token_len;
  const char *reqid;		// request id as raw JSON
  int reqid_len;
  bool cancelled;

  // 2026-07-28 multi round-trip requests
  bool modern;
  const char *in_resp;		// params.inputResponses object, NULL when absent
  int in_resp_len;
  const char *method;		// what the state token is bound to
  char target[256 + 1];
  char *state_in;		// verified user state of params.requestState
  char *state_out;		// state to hand to the client with the next InputRequiredResult
  unsigned seq;			// questions asked so far, they are named mcpc-1, mcpc-2, ...
  struct mcpc_inreq
  {
    char *key;
    const char *method;
    char *params;
  } *inreqs;
  size_t n_inreqs;
};

// Release what the context allocated.
void mcpc_ctx_release (struct mcpc_ctx *ctx);

// requestState tokens, see mcpc_ctx_state_set. seal returns a heap string or NULL.
char *mcpc_state_seal (const mcpc_server_t * sv, const char *method, const char *target, const char *state);

// Check a token and return the state it carries (heap string), NULL if it is forged, expired or for another request.
char *mcpc_state_open (const mcpc_server_t * sv, const char *method, const char *target, const char *token, size_t token_len);

// Transport access used by the context, implemented in server.c.
void mcpc_srv_send (mcpc_server_t * sv, mcpc_sock_t csock, const char *p, size_t len);

// One byte from the client's stream, -1 at end of stream or on error.
int mcpc_srv_read_byte (mcpc_server_t * sv, mcpc_sock_t csock);

// Park a complete frame received while a callback waited for the client; it is served afterwards.
bool mcpc_srv_defer (mcpc_conn_t * conn, const char *frame, size_t len);

#endif
