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

#include "ctx.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "ext_string.h"
#include "frame.h"
#include "hmac.h"
#include "jsonutil.h"
#include "net.h"
#include <time.h>
#include "mjson.h"
#include "proto.h"
#include "retbuf.h"

MCPC_API const char *
mcpc_ctx_get_protocol_version (const mcpc_ctx_t *ctx)
{
  return mcpc_pver_str (ctx->conn->pver);
}

MCPC_API mcpc_errcode_t
mcpc_ctx_get_args_json (const mcpc_ctx_t *ctx, const char8_t **json, size_t *json_len)
{
  *json = (const char8_t *) ctx->args;
  *json_len = (size_t) ctx->args_len;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_ctx_arg_json (const mcpc_ctx_t *ctx, const char8_t *name_nt, const char8_t **json, size_t *json_len)
{
  char path[2 + 255 + 1];
  if (strlen ((const char *) name_nt) > 255)
    return MCPC_EC_ANYDATA_NOTFOUND;
  snprintf (path, sizeof (path), "$.%s", (const char *) name_nt);

  const char *tok = nullptr;
  int tok_len = 0;
  const int tt = mjson_find (ctx->args, ctx->args_len, path, &tok, &tok_len);
  if (tt == MJSON_TOK_INVALID || tt == MJSON_TOK_NULL)
    return MCPC_EC_ANYDATA_NOTFOUND;
  *json = (const char8_t *) tok;
  *json_len = (size_t) tok_len;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_ctx_arg_str (const mcpc_ctx_t *ctx, const char8_t *name_nt, char8_t *ret, size_t ret_cap, size_t *ret_len)
{
  const char8_t *tok = nullptr;
  size_t tok_len = 0;
  if (mcpc_ctx_arg_json (ctx, name_nt, &tok, &tok_len) != MCPC_EC_0)
    return MCPC_EC_ANYDATA_NOTFOUND;
  if (tok_len < 2 || tok[0] != '"')
    return MCPC_EC_JSON_FIELD_INVALID;

  // unescaping never grows the text, so tok_len bytes always suffice
  char *buf = mcpc_alloc (tok_len + 1);
  if (buf == nullptr)
    return MCPC_EC_BUG;
  const int got = mjson_get_string ((const char *) tok, (int) tok_len, "$", buf, (int) tok_len + 1);
  mcpc_errcode_t ec = MCPC_EC_0;
  if (got < 0)
    ec = MCPC_EC_JSON_FIELD_INVALID;
  else
    {
      if (ret_len != nullptr)
	*ret_len = (size_t) got;
      if ((size_t) got >= ret_cap)
	ec = MCPC_EC_BUFCAP;
      else
	{
	  memcpy (ret, buf, (size_t) got);
	  ret[got] = 0;
	}
    }
  free (buf);
  return ec;
}

MCPC_API mcpc_errcode_t
mcpc_ctx_arg_f64 (const mcpc_ctx_t *ctx, const char8_t *name_nt, double *ret)
{
  const char8_t *tok = nullptr;
  size_t tok_len = 0;
  if (mcpc_ctx_arg_json (ctx, name_nt, &tok, &tok_len) != MCPC_EC_0)
    return MCPC_EC_ANYDATA_NOTFOUND;
  char num[64];
  if (tok_len >= sizeof (num) || !((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '-'))
    return MCPC_EC_JSON_FIELD_INVALID;
  memcpy (num, tok, tok_len);
  num[tok_len] = 0;
  *ret = strtod (num, nullptr);
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_ctx_arg_bool (const mcpc_ctx_t *ctx, const char8_t *name_nt, bool *ret)
{
  const char8_t *tok = nullptr;
  size_t tok_len = 0;
  if (mcpc_ctx_arg_json (ctx, name_nt, &tok, &tok_len) != MCPC_EC_0)
    return MCPC_EC_ANYDATA_NOTFOUND;
  if (tok_len == 4 && memcmp (tok, "true", 4) == 0)
    *ret = true;
  else if (tok_len == 5 && memcmp (tok, "false", 5) == 0)
    *ret = false;
  else
    return MCPC_EC_JSON_FIELD_INVALID;
  return MCPC_EC_0;
}

/* = = = = = = = = = = = = = = progress, logging = = = = = = = = = = = = = = */

// build and send "{jsonrpc, method, params}" where params is the printf-built object in rtbf
static mcpc_errcode_t
_ctx_notify (mcpc_ctx_t *ctx, const char *method, retbuf_t params)
{
  if (!retbuf_is_ok (params))
    return MCPC_EC_BUG;
  char *msg = mjson_aprintf ("{\"jsonrpc\":\"2.0\",\"method\":%Q,\"params\":%.*s}\n", method, (int) retbuf_len (params),
			     retbuf_data (params));
  if (msg == nullptr)
    return MCPC_EC_BUG;
  mcpc_srv_send (ctx->sv, ctx->csock, msg, strlen (msg));
  free (msg);
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_ctx_progress (mcpc_ctx_t *ctx, double progress, double total, const char8_t *message_nt)
{
  if (ctx->token == nullptr)
    return MCPC_EC_0;		// the client did not ask for progress

  retbuf_t p = retbuf_new (128);
  if (p == nullptr)
    return MCPC_EC_BUG;
  rtbuf_printf_mjfmt (p, u8c1 ("{%Q:%.*s,%Q:%g"), "progressToken", ctx->token_len, ctx->token, "progress", progress);
  if (total > 0)
    rtbuf_printf_mjfmt (p, u8c1 (",%Q:%g"), "total", total);
  if (message_nt != nullptr && (ctx->feat & MCPC_FEAT_PROGRESS_MSG))
    rtbuf_printf_mjfmt (p, u8c1 (",%Q:%Q"), "message", (const char *) message_nt);
  rtbuf_printf_mjfmt (p, u8c1 ("}"));

  const mcpc_errcode_t ec = _ctx_notify (ctx, "notifications/progress", p);
  retbuf_free (p);
  return ec;
}

static const char *const loglevel_names[] = { "debug", "info", "notice", "warning", "error", "critical", "alert", "emergency" };

MCPC_API mcpc_errcode_t
mcpc_ctx_log (mcpc_ctx_t *ctx, mcpc_loglevel_t level, const char8_t *logger_nt, const char8_t *fmt, ...)
{
  if (!mcpc_server_supp_log (ctx->sv))
    return MCPC_EC_CAPA_OFF;
  if ((int) level < 0 || (int) level > MCPC_LOG_EMERGENCY)
    return MCPC_EC_BUG;
  if ((int) level < ctx->conn->log_level)
    return MCPC_EC_0;		// below the threshold the client set

  char text[1024];
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (text, sizeof (text), (const char *) fmt, ap);
  va_end (ap);

  retbuf_t p = retbuf_new (256);
  if (p == nullptr)
    return MCPC_EC_BUG;
  rtbuf_printf_mjfmt (p, u8c1 ("{%Q:%Q"), "level", loglevel_names[level]);
  if (logger_nt != nullptr)
    rtbuf_printf_mjfmt (p, u8c1 (",%Q:%Q"), "logger", (const char *) logger_nt);
  rtbuf_printf_mjfmt (p, u8c1 (",%Q:%Q}"), "data", text);

  const mcpc_errcode_t ec = _ctx_notify (ctx, "notifications/message", p);
  retbuf_free (p);
  return ec;
}

MCPC_API bool
mcpc_ctx_cancelled (const mcpc_ctx_t *ctx)
{
  // over HTTP a client that stops waiting closes its connection, which is how it cancels
  if (!ctx->cancelled && ctx->sv->typ == MCPC_SV_HTTP && ctx->conn->http != nullptr && mcpc_net_peer_gone (ctx->conn->http->fd))
    ((mcpc_ctx_t *) ctx)->cancelled = true;
  return ctx->cancelled;
}

/* = = = = = = = = = = = = = = asking the client = = = = = = = = = = = = = = */

// does the cancellation notification in frame name our request?
static bool
_is_cancel_of (const mcpc_ctx_t *ctx, const char *frame, int len)
{
  const char *tok = nullptr;
  int tl = 0;
  if (mjson_find (frame, len, "$.method", &tok, &tl) != MJSON_TOK_STRING || tl != 2 + 23 ||
      memcmp (tok, "\"notifications/cancelled\"", 25) != 0)
    return false;
  if (mjson_find (frame, len, "$.params.requestId", &tok, &tl) == MJSON_TOK_INVALID)
    return false;
  return tl == ctx->reqid_len && memcmp (tok, ctx->reqid, (size_t) tl) == 0;
}

MCPC_API mcpc_errcode_t
mcpc_ctx_request_client (mcpc_ctx_t *ctx, const char8_t *method_nt, const char8_t *params_json_nt, char8_t *ret,
			 size_t ret_cap, size_t *ret_len)
{
  if (ctx->modern)
    return MCPC_EC_UNSUPPORTED;	// 2026-07-28 has no server requests
  if (ctx->sv->typ == MCPC_SV_HTTP)
    {
      // the question goes out on this request's event stream and the answer comes back as another POST
      const mcpc_errcode_t ec = mcpc_http_request_client (ctx->sv, ctx->conn, (const char *) method_nt,
							  params_json_nt != nullptr ? (const char *) params_json_nt : "{}", (char *) ret, ret_cap, ret_len);
      if (ec == MCPC_EC_CANCELLED)
	ctx->cancelled = true;
      return ec;
    }

  mcpc_conn_t *conn = ctx->conn;
  const unsigned long id = conn->next_req++;

  char *msg = mjson_aprintf ("{\"jsonrpc\":\"2.0\",\"id\":%lu,\"method\":%Q,\"params\":%s}\n", id,
			     (const char *) method_nt, params_json_nt != nullptr ? (const char *) params_json_nt : "{}");
  if (msg == nullptr)
    return MCPC_EC_BUG;
  mcpc_srv_send (ctx->sv, ctx->csock, msg, strlen (msg));
  free (msg);

  char idtxt[32];
  const int idlen = snprintf (idtxt, sizeof (idtxt), "%lu", id);

  // read until the answer to our request shows up; everything else is deferred or noted
  mcpc_framer_t fr;
  mcpc_framer_init_lines (&fr, MCPC_FRAME_DEFAULT_MAX);
  mcpc_errcode_t ec = MCPC_EC_CONN_CLOSED;

  for (;;)
    {
      const int ch = mcpc_srv_read_byte (ctx->sv, ctx->csock);
      if (ch < 0)
	break;
      const mcpc_frame_stat_t st = mcpc_framer_push (&fr, (unsigned char) ch);
      if (st == MCPC_FRAME_NOMEM)
	{
	  ec = MCPC_EC_BUG;
	  break;
	}
      if (st != MCPC_FRAME_READY)
	continue;

      const char *itok = nullptr;
      int ilen = 0;
      const char *res = nullptr;
      int res_len = 0;
      const bool has_id = mjson_find (fr.buf, (int) fr.len, "$.id", &itok, &ilen) != MJSON_TOK_INVALID;
      const bool is_ours = has_id && ilen == idlen && memcmp (itok, idtxt, (size_t) idlen) == 0;
      const bool is_method = mjson_find (fr.buf, (int) fr.len, "$.method", &res, &res_len) == MJSON_TOK_STRING;

      if (is_ours && !is_method)
	{
	  if (mjson_find (fr.buf, (int) fr.len, "$.error", &res, &res_len) != MJSON_TOK_INVALID)
	    ec = MCPC_EC_CLIENT_ERR;
	  else if (mjson_find (fr.buf, (int) fr.len, "$.result", &res, &res_len) != MJSON_TOK_INVALID)
	    {
	      if (ret_len != nullptr)
		*ret_len = (size_t) res_len;
	      if ((size_t) res_len >= ret_cap)
		ec = MCPC_EC_BUFCAP;
	      else
		{
		  memcpy (ret, res, (size_t) res_len);
		  ret[res_len] = 0;
		  ec = MCPC_EC_0;
		}
	    }
	  else
	    ec = MCPC_EC_CLIENT_ERR;
	  break;
	}

      if (_is_cancel_of (ctx, fr.buf, (int) fr.len))
	{
	  ctx->cancelled = true;
	  ec = MCPC_EC_CANCELLED;
	  break;
	}

      // an answer to something else we never asked is noise; a request or notification waits its turn
      if (is_method)
	mcpc_srv_defer (conn, fr.buf, fr.len);
      mcpc_framer_next (&fr);
    }

  mcpc_framer_free (&fr);
  return ec;
}

// Ask the client for something and copy its result to ret. Initialize-based revisions send a request
// and wait. 2026-07-28 looks for the answer among the inputResponses of this request and, when it is
// not there yet, records the question for the InputRequiredResult and returns MCPC_EC_NEEDS_INPUT.
static mcpc_errcode_t
_ctx_ask (mcpc_ctx_t *ctx, const char *method, const char8_t *params_json, char8_t *ret, size_t ret_cap, size_t *ret_len)
{
  if (!ctx->modern)
    return mcpc_ctx_request_client (ctx, (const char8_t *) method, params_json, ret, ret_cap, ret_len);

  char key[24];
  snprintf (key, sizeof (key), "mcpc-%u", ++ctx->seq);

  const char *ans = nullptr;
  size_t ans_len = 0;
  const int tt = ctx->in_resp != nullptr ? mcpc_json_member (ctx->in_resp, (size_t) ctx->in_resp_len, key, &ans, &ans_len) :
    MJSON_TOK_INVALID;
  if (tt == MJSON_TOK_OBJECT)
    {
      if (ret_len != nullptr)
	*ret_len = ans_len;
      if (ans_len >= ret_cap)
	return MCPC_EC_BUFCAP;
      memcpy (ret, ans, ans_len);
      ret[ans_len] = 0;
      return MCPC_EC_0;
    }
  if (tt != MJSON_TOK_INVALID)
    return MCPC_EC_CLIENT_ERR;	// answered with something that is not a result object

  struct mcpc_inreq *n = mcpc_realloc (ctx->inreqs, (ctx->n_inreqs + 1) * sizeof (struct mcpc_inreq));
  if (n == nullptr)
    return MCPC_EC_BUG;
  ctx->inreqs = n;
  const char *p = params_json != nullptr ? (const char *) params_json : "{}";
  struct mcpc_inreq *q = &ctx->inreqs[ctx->n_inreqs];
  q->key = mcpc_alloc (strlen (key) + 1);
  q->params = mcpc_alloc (strlen (p) + 1);
  if (q->key == nullptr || q->params == nullptr)
    {
      free (q->key);
      free (q->params);
      return MCPC_EC_BUG;
    }
  strcpy (q->key, key);
  strcpy (q->params, p);
  q->method = method;
  ctx->n_inreqs++;
  return MCPC_EC_NEEDS_INPUT;
}

void
mcpc_ctx_release (struct mcpc_ctx *ctx)
{
  for (size_t i = 0; i < ctx->n_inreqs; i++)
    {
      free (ctx->inreqs[i].key);
      free (ctx->inreqs[i].params);
    }
  free (ctx->inreqs);
  free (ctx->state_in);
  free (ctx->state_out);
  ctx->inreqs = nullptr;
  ctx->n_inreqs = 0;
  ctx->state_in = ctx->state_out = nullptr;
}

/* = = = = = = = = = = = = = = requestState = = = = = = = = = = = = = = */

char *
mcpc_state_seal (const mcpc_server_t *sv, const char *method, const char *target, const char *state)
{
  if (sv->state_key == nullptr)
    return nullptr;

  const unsigned long long exp = (unsigned long long) time (nullptr) + sv->state_ttl;
  char head[64];
  const int hl = snprintf (head, sizeof (head), "%llu\n", exp);
  const size_t pl = (size_t) hl + strlen (method) + 1 + strlen (target) + 1 + strlen (state);
  char *payload = mcpc_alloc (pl + 1);
  if (payload == nullptr)
    return nullptr;
  snprintf (payload, pl + 1, "%s%s\n%s\n%s", head, method, target, state);

  // "v1." + base64(payload) + "." + base64(mac over everything before the last dot)
  const size_t b64len = 4 * ((pl + 2) / 3);
  const size_t tl = 3 + b64len + 1 + 4 * ((MCPC_SHA256_LEN + 2) / 3);
  char *tok = mcpc_alloc (tl + 1);
  if (tok == nullptr)
    {
      free (payload);
      return nullptr;
    }
  memcpy (tok, "v1.", 3);
  mcpc_b64_encode (payload, pl, tok + 3);
  const size_t signed_len = 3 + b64len;
  tok[signed_len] = '.';
  uint8_t mac[MCPC_SHA256_LEN];
  mcpc_hmac_sha256 (sv->state_key, sv->state_key_len, tok, signed_len, mac);
  mcpc_b64_encode (mac, sizeof (mac), tok + signed_len + 1);
  free (payload);
  return tok;
}

char *
mcpc_state_open (const mcpc_server_t *sv, const char *method, const char *target, const char *token, size_t token_len)
{
  if (sv->state_key == nullptr || token_len < 5 || memcmp (token, "v1.", 3) != 0)
    return nullptr;

  const char *dot = memchr (token + 3, '.', token_len - 3);
  if (dot == nullptr)
    return nullptr;
  const size_t signed_len = (size_t) (dot - token);
  const char *mac_b64 = dot + 1;
  const size_t mac_b64_len = token_len - signed_len - 1;

  // the token comes from the client: only a MAC of exactly the right size may be decoded into the buffer
  if (mac_b64_len != 4 * ((MCPC_SHA256_LEN + 2) / 3))
    return nullptr;
  uint8_t got[MCPC_SHA256_LEN + 3], want[MCPC_SHA256_LEN];
  const long gl = mcpc_b64_decode (mac_b64, mac_b64_len, got);
  mcpc_hmac_sha256 (sv->state_key, sv->state_key_len, token, signed_len, want);
  if (gl != MCPC_SHA256_LEN || !mcpc_ct_equal (got, want, MCPC_SHA256_LEN))
    return nullptr;

  const size_t enc_len = signed_len - 3;
  uint8_t *payload = mcpc_alloc (enc_len / 4 * 3 + 4);
  if (payload == nullptr)
    return nullptr;
  const long pl = mcpc_b64_decode (token + 3, enc_len, payload);
  char *res = nullptr;
  if (pl > 0)
    {
      payload[pl] = 0;
      char *end = nullptr;
      const unsigned long long exp = strtoull ((const char *) payload, &end, 10);
      if (end != nullptr && *end == '\n' && (unsigned long long) time (nullptr) <= exp)
	{
	  const char *m = end + 1;
	  const char *m_end = strchr (m, '\n');
	  const char *t = m_end != nullptr ? m_end + 1 : nullptr;
	  const char *t_end = t != nullptr ? strchr (t, '\n') : nullptr;
	  if (t_end != nullptr && strlen (method) == (size_t) (m_end - m) && memcmp (m, method, (size_t) (m_end - m)) == 0 &&
	      strlen (target) == (size_t) (t_end - t) && memcmp (t, target, (size_t) (t_end - t)) == 0)
	    {
	      const char *st = t_end + 1;
	      res = mcpc_alloc (strlen (st) + 1);
	      if (res != nullptr)
		strcpy (res, st);
	    }
	}
    }
  free (payload);
  return res;
}

MCPC_API mcpc_errcode_t
mcpc_ctx_state_set (mcpc_ctx_t *ctx, const char8_t *state_nt)
{
  if (!ctx->modern)
    return MCPC_EC_UNSUPPORTED;
  if (ctx->sv->state_key == nullptr)
    return MCPC_EC_CAPA_OFF;
  char *tok = mcpc_state_seal (ctx->sv, ctx->method, ctx->target, state_nt != nullptr ? (const char *) state_nt : "");
  if (tok == nullptr)
    return MCPC_EC_BUG;
  free (ctx->state_out);
  ctx->state_out = tok;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_ctx_state_get (const mcpc_ctx_t *ctx, char8_t *ret, size_t ret_cap, size_t *ret_len)
{
  if (ctx->state_in == nullptr)
    return MCPC_EC_ANYDATA_NOTFOUND;
  const size_t n = strlen (ctx->state_in);
  if (ret_len != nullptr)
    *ret_len = n;
  if (n >= ret_cap)
    return MCPC_EC_BUFCAP;
  memcpy (ret, ctx->state_in, n + 1);
  return MCPC_EC_0;
}

// the client must have declared capability `cap` (a top-level key of its capabilities object)
static bool
_client_has (const mcpc_ctx_t *ctx, const char *cap)
{
  if (ctx->conn->ccaps == nullptr)
    return false;
  char path[40];
  snprintf (path, sizeof (path), "$.%s", cap);
  const char *tok = nullptr;
  int tl = 0;
  return mjson_find (ctx->conn->ccaps, (int) ctx->conn->ccaps_len, path, &tok, &tl) == MJSON_TOK_OBJECT;
}

static mcpc_elicit_action_t
_parse_action (const char *res, int res_len, bool *ok)
{
  const char *tok = nullptr;
  int tl = 0;
  *ok = true;
  if (mjson_find (res, res_len, "$.action", &tok, &tl) == MJSON_TOK_STRING)
    {
      if (tl == 8 && memcmp (tok, "\"accept\"", 8) == 0)
	return MCPC_ELICIT_ACCEPT;
      if (tl == 9 && memcmp (tok, "\"decline\"", 9) == 0)
	return MCPC_ELICIT_DECLINE;
      if (tl == 8 && memcmp (tok, "\"cancel\"", 8) == 0)
	return MCPC_ELICIT_CANCEL;
    }
  *ok = false;
  return MCPC_ELICIT_CANCEL;
}

MCPC_API mcpc_errcode_t
mcpc_ctx_elicit_form (mcpc_ctx_t *ctx, const char8_t *message_nt, const char8_t *requested_schema_json_nt,
		      mcpc_elicit_action_t *action, char8_t *content, size_t content_cap, size_t *content_len)
{
  if (!(ctx->feat & MCPC_FEAT_ELICIT))
    return MCPC_EC_UNSUPPORTED;
  if (!_client_has (ctx, "elicitation"))
    return MCPC_EC_CLIENT_NOCAPA;

  char *params = mjson_aprintf ("{\"mode\":\"form\",\"message\":%Q,\"requestedSchema\":%s}", (const char *) message_nt,
				(const char *) requested_schema_json_nt);
  if (params == nullptr)
    return MCPC_EC_BUG;
  // 2025-06-18 has no "mode" member, form was the only kind
  if (!(ctx->feat & MCPC_FEAT_ELICIT_URL))
    {
      free (params);
      params = mjson_aprintf ("{\"message\":%Q,\"requestedSchema\":%s}", (const char *) message_nt,
			      (const char *) requested_schema_json_nt);
      if (params == nullptr)
	return MCPC_EC_BUG;
    }

  size_t cap = 4096 + (content_cap > 0 ? content_cap : 0);
  char8_t *res = mcpc_alloc (cap);
  if (res == nullptr)
    {
      free (params);
      return MCPC_EC_BUG;
    }
  size_t res_len = 0;
  mcpc_errcode_t ec = _ctx_ask (ctx, "elicitation/create", (const char8_t *) params, res, cap, &res_len);
  free (params);

  if (ec == MCPC_EC_0)
    {
      bool ok = false;
      *action = _parse_action ((const char *) res, (int) res_len, &ok);
      if (!ok)
	ec = MCPC_EC_CLIENT_ERR;
      else if (content != nullptr && content_cap > 0)
	{
	  const char *ct = nullptr;
	  int cl = 0;
	  if (*action == MCPC_ELICIT_ACCEPT && mjson_find ((const char *) res, (int) res_len, "$.content", &ct, &cl) == MJSON_TOK_OBJECT)
	    {
	      if (content_len != nullptr)
		*content_len = (size_t) cl;
	      if ((size_t) cl >= content_cap)
		ec = MCPC_EC_BUFCAP;
	      else
		{
		  memcpy (content, ct, (size_t) cl);
		  content[cl] = 0;
		}
	    }
	  else
	    {
	      content[0] = 0;
	      if (content_len != nullptr)
		*content_len = 0;
	    }
	}
    }
  free (res);
  return ec;
}

MCPC_API mcpc_errcode_t
mcpc_ctx_elicit_url (mcpc_ctx_t *ctx, const char8_t *message_nt, const char8_t *url_nt, const char8_t *elicitation_id_nt,
		     mcpc_elicit_action_t *action)
{
  if (!(ctx->feat & MCPC_FEAT_ELICIT_URL))
    return MCPC_EC_UNSUPPORTED;
  if (!_client_has (ctx, "elicitation"))
    return MCPC_EC_CLIENT_NOCAPA;
  // the client lists the modes it supports; an empty object means form only
  const char *tok = nullptr;
  int tl = 0;
  if (mjson_find (ctx->conn->ccaps, (int) ctx->conn->ccaps_len, "$.elicitation.url", &tok, &tl) == MJSON_TOK_INVALID)
    return MCPC_EC_CLIENT_NOCAPA;

  char *params = mjson_aprintf ("{\"mode\":\"url\",\"message\":%Q,\"url\":%Q,\"elicitationId\":%Q}", (const char *) message_nt,
				(const char *) url_nt, elicitation_id_nt != nullptr ? (const char *) elicitation_id_nt : "");
  if (params == nullptr)
    return MCPC_EC_BUG;
  char8_t res[512];
  size_t res_len = 0;
  mcpc_errcode_t ec = _ctx_ask (ctx, "elicitation/create", (const char8_t *) params, res, sizeof (res), &res_len);
  free (params);
  if (ec == MCPC_EC_0)
    {
      bool ok = false;
      *action = _parse_action ((const char *) res, (int) res_len, &ok);
      if (!ok)
	ec = MCPC_EC_CLIENT_ERR;
    }
  return ec;
}

MCPC_API mcpc_errcode_t
mcpc_ctx_sample (mcpc_ctx_t *ctx, const char8_t *params_json_nt, char8_t *ret, size_t ret_cap, size_t *ret_len)
{
  if (!_client_has (ctx, "sampling"))
    return MCPC_EC_CLIENT_NOCAPA;
  return _ctx_ask (ctx, "sampling/createMessage", params_json_nt, ret, ret_cap, ret_len);
}

MCPC_API mcpc_errcode_t
mcpc_ctx_list_roots (mcpc_ctx_t *ctx, char8_t *ret, size_t ret_cap, size_t *ret_len)
{
  if (!_client_has (ctx, "roots"))
    return MCPC_EC_CLIENT_NOCAPA;

  size_t cap = ret_cap + 64;
  char8_t *res = mcpc_alloc (cap);
  if (res == nullptr)
    return MCPC_EC_BUG;
  size_t res_len = 0;
  mcpc_errcode_t ec = _ctx_ask (ctx, "roots/list", nullptr, res, cap, &res_len);
  if (ec == MCPC_EC_0)
    {
      const char *tok = nullptr;
      int tl = 0;
      if (mjson_find ((const char *) res, (int) res_len, "$.roots", &tok, &tl) != MJSON_TOK_ARRAY)
	ec = MCPC_EC_CLIENT_ERR;
      else
	{
	  if (ret_len != nullptr)
	    *ret_len = (size_t) tl;
	  if ((size_t) tl >= ret_cap)
	    ec = MCPC_EC_BUFCAP;
	  else
	    {
	      memcpy (ret, tok, (size_t) tl);
	      ret[tl] = 0;
	    }
	}
    }
  free (res);
  return ec;
}
