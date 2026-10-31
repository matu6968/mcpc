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

#ifndef h_pub_ctx
#define h_pub_ctx

#include <stddef.h>

#include "_c23_keywords.h"
#include "_c23_uchar.h"
#include "errcode.h"
#include "visb.h"

// RFC 5424 severities, in the order the MCP logging utility defines them.
enum mcpc_loglevel
{
  MCPC_LOG_DEBUG = 0,
  MCPC_LOG_INFO,
  MCPC_LOG_NOTICE,
  MCPC_LOG_WARNING,
  MCPC_LOG_ERROR,
  MCPC_LOG_CRITICAL,
  MCPC_LOG_ALERT,
  MCPC_LOG_EMERGENCY,
};
typedef enum mcpc_loglevel mcpc_loglevel_t;

// ---------------------------- request context ----------------------------
//
// A tool callback registered with mcpc_tool_set_call_cb2 receives a context for the
// request it serves. The context is valid only inside the callback.

typedef struct mcpc_ctx mcpc_ctx_t;

// Protocol revision negotiated with the client, e.g. "2025-11-25".
MCPC_API const char *mcpc_ctx_get_protocol_version (const mcpc_ctx_t *ctx);

// The raw JSON "arguments" object of the call ("{}" when there were none).
MCPC_API mcpc_errcode_t mcpc_ctx_get_args_json (const mcpc_ctx_t *ctx, const char8_t **json, size_t *json_len);

// One argument by name. The *_json variant returns the raw token (a quoted string stays quoted).
MCPC_API mcpc_errcode_t mcpc_ctx_arg_json (const mcpc_ctx_t *ctx, const char8_t *name_nt, const char8_t **json, size_t *json_len);

// Unescaped string argument, NUL-terminated; MCPC_EC_BUFCAP when ret_cap is too small.
MCPC_API mcpc_errcode_t mcpc_ctx_arg_str (const mcpc_ctx_t *ctx, const char8_t *name_nt, char8_t *ret, size_t ret_cap, size_t *ret_len);

MCPC_API mcpc_errcode_t mcpc_ctx_arg_f64 (const mcpc_ctx_t *ctx, const char8_t *name_nt, double *ret);

MCPC_API mcpc_errcode_t mcpc_ctx_arg_bool (const mcpc_ctx_t *ctx, const char8_t *name_nt, bool *ret);

// ---- progress, logging, cancellation ----

// Report progress of the request. total <= 0 means unknown, message_nt may be NULL (it is only
// sent from 2025-03-26 on). Does nothing, and succeeds, when the client did not ask for progress.
MCPC_API mcpc_errcode_t mcpc_ctx_progress (mcpc_ctx_t *ctx, double progress, double total, const char8_t *message_nt);

// Send a log message to the client when logging is enabled on the server and level is at or
// above what the client asked for with logging/setLevel. logger_nt may be NULL.
MCPC_API mcpc_errcode_t mcpc_ctx_log (mcpc_ctx_t *ctx, mcpc_loglevel_t level, const char8_t *logger_nt, const char8_t *fmt, ...);

// Whether the client cancelled this request. The server serves one request at a time per
// connection, so the flag changes only while the callback waits for the client in one of the
// mcpc_ctx_request_* / mcpc_ctx_elicit_* / mcpc_ctx_sample / mcpc_ctx_list_roots calls.
MCPC_API bool mcpc_ctx_cancelled (const mcpc_ctx_t *ctx);

// ---- asking the client ----
//
// A callback can ask the client for input (elicitation, sampling, roots). How the question travels
// depends on the negotiated revision, the calls look the same:
//
//  - initialize-based revisions (2025-11-25 and earlier): the request is sent and the call waits for
//    the answer; messages the client sends in the meantime are served after the callback returns.
//  - 2026-07-28: servers cannot send requests. The first call records the question and returns
//    MCPC_EC_NEEDS_INPUT; the callback must then return promptly. The server answers the client's
//    request with an InputRequiredResult, the client asks its user and retries the same request with
//    the answers, the callback is run AGAIN from the start and the same calls now return them.
//    A callback must therefore be able to run repeatedly, deriving everything from the request.
//    Calls made in the same order get the same answers; ask several questions before returning
//    when they are independent, the client answers them in one round trip.
//
// They fail with MCPC_EC_CLIENT_NOCAPA when the client did not declare the capability, and with
// MCPC_EC_UNSUPPORTED when the negotiated revision lacks the feature.

// Any request, for initialize-based revisions only (MCPC_EC_UNSUPPORTED on 2026-07-28, use the typed
// functions below). params_json_nt may be NULL. The client's "result" is copied to ret, NUL-terminated.
MCPC_API mcpc_errcode_t mcpc_ctx_request_client (mcpc_ctx_t *ctx, const char8_t *method_nt, const char8_t *params_json_nt,
						 char8_t *ret, size_t ret_cap, size_t *ret_len);

enum mcpc_elicit_action
{
  MCPC_ELICIT_ACCEPT = 0,
  MCPC_ELICIT_DECLINE,
  MCPC_ELICIT_CANCEL,
};
typedef enum mcpc_elicit_action mcpc_elicit_action_t;

// Ask the user for structured input (2025-06-18+). requested_schema_json_nt is a flat object
// schema. On accept the submitted values are copied to content as a JSON object.
MCPC_API mcpc_errcode_t mcpc_ctx_elicit_form (mcpc_ctx_t *ctx, const char8_t *message_nt, const char8_t *requested_schema_json_nt,
					      mcpc_elicit_action_t *action, char8_t *content, size_t content_cap, size_t *content_len);

// Send the user to a URL to complete something out of band (2025-11-25+).
MCPC_API mcpc_errcode_t mcpc_ctx_elicit_url (mcpc_ctx_t *ctx, const char8_t *message_nt, const char8_t *url_nt,
					     const char8_t *elicitation_id_nt, mcpc_elicit_action_t *action);

// Ask the client's model to generate a message. params_json_nt is the params of
// sampling/createMessage; the whole result object is copied to ret.
MCPC_API mcpc_errcode_t mcpc_ctx_sample (mcpc_ctx_t *ctx, const char8_t *params_json_nt, char8_t *ret, size_t ret_cap, size_t *ret_len);

// The client's roots; the "roots" array is copied to ret.
MCPC_API mcpc_errcode_t mcpc_ctx_list_roots (mcpc_ctx_t *ctx, char8_t *ret, size_t ret_cap, size_t *ret_len);

// ---- requestState (2026-07-28) ----
//
// Server state that travels with a multi round-trip request: set it before returning
// MCPC_EC_NEEDS_INPUT, read it back when the callback runs again. The client sees an opaque,
// HMAC-protected token (bound to the request's method and target, with an expiry), so it can
// neither read meaning into it nor alter it. Needs mcpc_server_set_state_secret.

MCPC_API mcpc_errcode_t mcpc_ctx_state_set (mcpc_ctx_t *ctx, const char8_t *state_nt);

// MCPC_EC_ANYDATA_NOTFOUND when the request carries no (valid) state.
MCPC_API mcpc_errcode_t mcpc_ctx_state_get (const mcpc_ctx_t *ctx, char8_t *ret, size_t ret_cap, size_t *ret_len);

#endif
