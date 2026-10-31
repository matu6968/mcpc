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

#include "server.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "anydata.h"
#include "ext_thread.h"
#include "ext_string.h"
#include "jsonutil.h"
#include "log.h"
#include "mjson.h"
#include "net.h"
#include "random.h"
#include <time.h>

#define DEFAULT_MAX_BODY (4u * 1024u * 1024u)
#define DEFAULT_MAX_CONNS 64u
#define MAX_CONNS_LIMIT 250u	// the connection pool counts in a uint8_t
#define DEFAULT_HEADER_TIMEOUT 15000u
#define DEFAULT_IDLE_TIMEOUT 60000u
#define SLICE_MS 250		// how often a waiting thread looks at the stop flag
#define KEEPALIVE_MS 15000

#define META_PV "io.modelcontextprotocol/protocolVersion"

// n bytes equal without regard to ASCII case
static bool
strncasecmp_ascii (const char *a, const char *b, size_t n)
{
  for (size_t i = 0; i < n; i++)
    if (tolower ((unsigned char) a[i]) != tolower ((unsigned char) b[i]))
      return false;
  return true;
}

static bool
ieq (const char *a, size_t al, const char *b)
{
  size_t i = 0;
  for (; i < al && b[i]; i++)
    if (tolower ((unsigned char) a[i]) != tolower ((unsigned char) b[i]))
      return false;
  return i == al && b[i] == 0;
}

static bool
in_list (const char *const *list, const char *s, size_t len)
{
  for (; list != nullptr && *list != nullptr; list++)
    if (strlen (*list) == len && strncasecmp_ascii (*list, s, len))
      return true;
  return false;
}

/* = = = = = = = = = = = = = = Origin and Host = = = = = = = = = = = = = = */

static bool
is_loopback_name (const char *h, size_t len)
{
  return ieq (h, len, "localhost") || ieq (h, len, "127.0.0.1") || ieq (h, len, "[::1]") || ieq (h, len, "::1");
}

// host part of "host", "host:port", "[v6]" or "[v6]:port"
static void
host_only (const char *v, size_t len, const char **h, size_t *hl)
{
  *h = v;
  *hl = len;
  if (len > 0 && v[0] == '[')
    {
      const char *close = memchr (v, ']', len);
      if (close != nullptr)
	*hl = (size_t) (close - v) + 1;
      return;
    }
  const char *colon = memchr (v, ':', len);
  if (colon != nullptr)
    *hl = (size_t) (colon - v);
}

static bool
host_ok (const mcpc_server_t *sv, const struct mcpc_http_req *rq, bool bound_loopback)
{
  size_t vl = 0;
  const char *v = mcpc_http_hdr (rq, "host", &vl);
  if (v == nullptr)
    return rq->minor == 0;
  const char *h;
  size_t hl;
  host_only (v, vl, &h, &hl);
  if (sv->http.allowed_hosts != nullptr)
    return in_list (sv->http.allowed_hosts, h, hl);
  // a loopback server must only answer to its own names, or a rebinding page could reach it
  return !bound_loopback || is_loopback_name (h, hl);
}

static bool
origin_ok (const mcpc_server_t *sv, const struct mcpc_http_req *rq)
{
  size_t ol = 0;
  const char *o = mcpc_http_hdr (rq, "origin", &ol);
  if (o == nullptr)
    return true;		// not a browser
  if (sv->http.allowed_origins != nullptr)
    {
      for (const char *const *p = sv->http.allowed_origins; *p != nullptr; p++)
	if (strcmp (*p, "*") == 0 || (strlen (*p) == ol && memcmp (*p, o, ol) == 0))
	  return true;
      return false;
    }
  // scheme://host[:port]
  const char *sep = memchr (o, ':', ol);
  if (sep == nullptr || (size_t) (sep - o) + 3 > ol || sep[1] != '/' || sep[2] != '/')
    return false;
  const char *h;
  size_t hl;
  host_only (sep + 3, ol - (size_t) (sep + 3 - o), &h, &hl);
  return is_loopback_name (h, hl);
}

/* = = = = = = = = = = = = = = plain error replies = = = = = = = = = = = = = = */

// A complete reply outside any JSON-RPC exchange (or carrying a JSON-RPC error body).
static void
reply (mcpc_fd_t fd, int status, const char *reason, const char *extra, const char *body, bool close_after)
{
  const size_t bl = body != nullptr ? strlen (body) : 0;
  char head[512];
  const int hl = snprintf (head, sizeof (head), "HTTP/1.1 %d %s\r\n%sContent-Length: %zu\r\nCache-Control: no-store\r\n%s%s\r\n",
			   status, reason, bl > 0 ? "Content-Type: application/json\r\n" : "", bl, extra != nullptr ? extra : "",
			   close_after ? "Connection: close\r\n" : "");
  mcpc_net_write (fd, head, (size_t) hl);
  if (bl > 0)
    mcpc_net_write (fd, body, bl);
}

static void
reply_rpc_error (mcpc_fd_t fd, int status, const char *reason, const char *id_raw, int id_len, int code, const char *msg,
		 const char *data_json, bool close_after)
{
  char *body = mjson_aprintf ("{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"error\":{\"code\":%d,\"message\":%Q%s%s}}", id_len > 0 ? id_len : 4,
			      id_len > 0 ? id_raw : "null", code, msg, data_json != nullptr ? ",\"data\":" : "",
			      data_json != nullptr ? data_json : "");
  reply (fd, status, reason, nullptr, body, close_after);
  free (body);
}

/* = = = = = = = = = = = = = = header validation = = = = = = = = = = = = = = */

#define MCPC_HEADER_MISMATCH (-32020)

struct vfail
{
  int status;
  int code;
  char msg[256];
  char data[160];		// raw JSON for error.data, or empty
};

static bool
fail (struct vfail *f, int status, int code, const char *fmt, ...)
{
  f->status = status;
  f->code = code;
  f->data[0] = 0;
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (f->msg, sizeof (f->msg), fmt, ap);
  va_end (ap);
  return false;
}

// compare a header value (decoded when it uses the base64 sentinel) with the text it mirrors
static bool
hdr_matches (const struct mcpc_http_req *rq, const char *name, const char *want, size_t want_len, bool numeric)
{
  size_t vl = 0;
  const char *v = mcpc_http_hdr (rq, name, &vl);
  if (v == nullptr)
    return false;
  size_t dl = 0;
  char *dec = mcpc_http_decode_value (v, vl, &dl);
  if (dec == nullptr)
    return false;
  bool same = dl == want_len && memcmp (dec, want, dl) == 0;
  if (!same && numeric)
    same = strtod (dec, nullptr) == strtod (want, nullptr) && dl > 0 && want_len > 0;
  free (dec);
  return same;
}

// Mirror check for tool parameters marked "x-mcp-header": walk the schema properties and the call
// arguments in step. Returns the name of the first header that is missing or wrong, or NULL.
static const char *
check_param_headers (const struct mcpc_http_req *rq, const char *schema, size_t slen, const char *args, size_t alen, int depth,
		     char *namebuf, size_t namecap)
{
  const char *props;
  size_t pl;
  if (depth > 8 || mcpc_json_member (schema, slen, "properties", &props, &pl) != MJSON_TOK_OBJECT)
    return nullptr;

  int koff, klen, voff, vlen, vtype;
  for (int off = 0; (off = mjson_next (props, (int) pl, off, &koff, &klen, &voff, &vlen, &vtype)) != 0;)
    {
      if (vtype != MJSON_TOK_OBJECT || klen < 2)
	continue;
      char key[256];
      if ((size_t) klen - 2 >= sizeof (key))
	continue;
      memcpy (key, props + koff + 1, (size_t) klen - 2);
      key[klen - 2] = 0;

      const char *av = nullptr;
      size_t al = 0;
      const int at = args != nullptr ? mcpc_json_member (args, alen, key, &av, &al) : MJSON_TOK_INVALID;

      const char *hv;
      size_t hl;
      if (mcpc_json_member (props + voff, (size_t) vlen, "x-mcp-header", &hv, &hl) == MJSON_TOK_STRING && hl >= 2 && at != MJSON_TOK_INVALID &&
	  at != MJSON_TOK_NULL)
	{
	  char hname[200];
	  snprintf (hname, sizeof (hname), "Mcp-Param-%.*s", (int) hl - 2 > 150 ? 150 : (int) hl - 2, hv + 1);
	  bool ok;
	  if (at == MJSON_TOK_STRING)
	    {
	      char *plain = mcpc_alloc (al + 1);
	      const int got = plain != nullptr ? mjson_get_string (av, (int) al, "$", plain, (int) al + 1) : -1;
	      ok = got >= 0 && hdr_matches (rq, hname, plain, (size_t) got, false);
	      free (plain);
	    }
	  else
	    ok = hdr_matches (rq, hname, av, al, at == MJSON_TOK_NUMBER);
	  if (!ok)
	    {
	      snprintf (namebuf, namecap, "%s", hname);
	      return namebuf;
	    }
	}
      if (vtype == MJSON_TOK_OBJECT && at == MJSON_TOK_OBJECT)
	{
	  const char *r = check_param_headers (rq, props + voff, (size_t) vlen, av, al, depth + 1, namebuf, namecap);
	  if (r != nullptr)
	    return r;
	}
    }
  return nullptr;
}

struct reqinfo
{
  bool has_id;
  const char *id;
  int id_len;
  bool is_request;		// has a method
  bool is_init;
  bool respond_202;		// nothing to dispatch (a stray response from the client)
  bool modern;			// carries per-request metadata of the 2026-07-28 revision
};

// Check the request body and the headers that mirror it before the core sees it. On failure fills f and
// returns false.
static bool
validate (const mcpc_server_t *sv, const struct mcpc_http_req *rq, const char *body, size_t n, struct reqinfo *ri, struct vfail *f,
	  mcpc_pver_t *hdr_pver)
{
  memset (ri, 0, sizeof (*ri));
  const char *top;
  int tl;
  const int tt = mjson_find (body, (int) n, "$", &top, &tl);
  if (tt == MJSON_TOK_INVALID)
    return fail (f, 400, -32700, "parse error");
  if (tt != MJSON_TOK_OBJECT)
    return fail (f, 400, -32600, tt == MJSON_TOK_ARRAY ? "batch requests are not supported" : "invalid request");

  const int idt = mjson_find (body, (int) n, "$.id", &ri->id, &ri->id_len);
  ri->has_id = (idt == MJSON_TOK_NUMBER || idt == MJSON_TOK_STRING) && mcpc_json_token_clean (idt, ri->id, (size_t) ri->id_len);
  if (!ri->has_id)
    {
      ri->id = nullptr;
      ri->id_len = 0;
      if (idt != MJSON_TOK_INVALID)
	return fail (f, 400, -32600, "invalid request id");
    }

  const char *m;
  int ml;
  if (mjson_find (body, (int) n, "$.method", &m, &ml) != MJSON_TOK_STRING)
    {
      const char *x;
      int xl;
      // the client may not send responses on this transport, but a stray one is not worth an error
      if (mjson_find (body, (int) n, "$.result", &x, &xl) != MJSON_TOK_INVALID || mjson_find (body, (int) n, "$.error", &x, &xl) != MJSON_TOK_INVALID)
	{
	  ri->respond_202 = true;
	  return true;
	}
      return fail (f, 400, -32600, "invalid request");
    }
  ri->is_request = true;
  char method[128];
  const int mlen = mjson_get_string (m, ml, "$", method, sizeof (method));
  if (mlen < 0)
    return fail (f, 400, -32600, "invalid request");
  ri->is_init = strcmp (method, "initialize") == 0;
  if (ri->is_init)
    return true;

  const char *pv = nullptr;
  size_t pvl = 0;
  const char *meta = nullptr;
  int metal = 0;
  int pvt = MJSON_TOK_INVALID;
  if (mjson_find (body, (int) n, "$.params._meta", &meta, &metal) == MJSON_TOK_OBJECT)
    pvt = mcpc_json_member (meta, (size_t) metal, META_PV, &pv, &pvl);

  size_t hl = 0;
  const char *hv = mcpc_http_hdr (rq, "mcp-protocol-version", &hl);
  char vers[160];
  vers[0] = 0;

  if (pvt != MJSON_TOK_INVALID)
    {
      ri->modern = true;
      // ---- 2026-07-28: the headers repeat what the body says, and both must agree
      const mcpc_pver_t v = (pvt == MJSON_TOK_STRING && pvl >= 2) ? mcpc_pver_parse (pv + 1, pvl - 2) : MCPC_PV_NONE;
      if (v != MCPC_PV_2026_07_28 || !(sv->pmask & mcpc_pver_bit (v)))
	return true;		// the core answers with the supported versions and 400

      if (hv == nullptr)
	return fail (f, 400, MCPC_HEADER_MISMATCH, "Header mismatch: missing MCP-Protocol-Version header");
      if (hl != pvl - 2 || memcmp (hv, pv + 1, hl) != 0)
	return fail (f, 400, MCPC_HEADER_MISMATCH, "Header mismatch: MCP-Protocol-Version header value '%.*s' does not match body value '%.*s'",
		     (int) (hl > 40 ? 40 : hl), hv, (int) (pvl - 2 > 40 ? 40 : pvl - 2), pv + 1);
      if (!hdr_matches (rq, "mcp-method", method, (size_t) mlen, false))
	return fail (f, 400, MCPC_HEADER_MISMATCH, "Header mismatch: Mcp-Method header is missing or does not match the method '%s'", method);

      const bool is_call = strcmp (method, "tools/call") == 0;
      if (is_call || strcmp (method, "prompts/get") == 0 || strcmp (method, "resources/read") == 0)
	{
	  const char *nm;
	  int nml;
	  const int nt = mjson_find (body, (int) n, is_call || method[0] == 'p' ? "$.params.name" : "$.params.uri", &nm, &nml);
	  if (nt == MJSON_TOK_STRING)
	    {
	      char *plain = mcpc_alloc ((size_t) nml + 1);
	      const int got = plain != nullptr ? mjson_get_string (nm, nml, "$", plain, nml + 1) : -1;
	      const bool ok = got >= 0 && hdr_matches (rq, "mcp-name", plain, (size_t) got, false);
	      free (plain);
	      if (!ok)
		return fail (f, 400, MCPC_HEADER_MISMATCH, "Header mismatch: Mcp-Name header is missing or does not match the request");
	    }
	}

      // headers mirroring annotated tool parameters
      if (is_call)
	{
	  const char *nm;
	  int nml;
	  if (mjson_find (body, (int) n, "$.params.name", &nm, &nml) == MJSON_TOK_STRING && nml >= 3 && nml <= 258)
	    {
	      char name[260];
	      const int got = mjson_get_string (nm, nml, "$", name, sizeof (name));
	      const mcpc_tool_t *tool = got > 0 ? (const mcpc_tool_t *) mcpc_toolpool_getbyname (sv->toolpool, (const char8_t *) name, (size_t) got) :
		nullptr;
	      const char *schema = tool != nullptr ? mcpc_anydata_xget (tool, MCPC_X_INSCHEMA) : nullptr;
	      if (schema != nullptr)
		{
		  const char *args = nullptr;
		  int al = 0;
		  mjson_find (body, (int) n, "$.params.arguments", &args, &al);
		  char bad[200];
		  const char *wrong = check_param_headers (rq, schema, strlen (schema), args, (size_t) al, 0, bad, sizeof (bad));
		  if (wrong != nullptr)
		    return fail (f, 400, MCPC_HEADER_MISMATCH, "Header mismatch: %s is missing or does not match the arguments", wrong);
		}
	    }
	}
      return true;
    }

  // ---- no per-request metadata: an initialize-based revision, named by the header
  mcpc_pver_t v = MCPC_PV_2025_03_26;	// the revision before the header existed
  if (hv != nullptr)
    v = mcpc_pver_parse (hv, hl);
  if (mcpc_pver_is_modern (v))
    return fail (f, 400, -32602, "missing required _meta field io.modelcontextprotocol/protocolVersion");
  if (v == MCPC_PV_NONE || !(sv->pmask & mcpc_pver_bit (v)))
    {
      int k = 0;
      vers[k++] = '[';
      for (int x = MCPC_PV_2026_07_28; x >= MCPC_PV_2024_11_05; x--)
	if (sv->pmask & mcpc_pver_bit ((mcpc_pver_t) x))
	  k += snprintf (vers + k, sizeof (vers) - (size_t) k, "%s\"%s\"", k > 1 ? "," : "", mcpc_pver_str ((mcpc_pver_t) x));
      vers[k++] = ']';
      vers[k] = 0;
      fail (f, 400, -32022, "Unsupported protocol version");
      mjson_snprintf (f->data, sizeof (f->data), "{\"supported\":%s,\"requested\":%.*Q}", vers, (int) (hl > 40 ? 40 : hl), hv != nullptr ? hv : "");
      return false;
    }
  *hdr_pver = v;
  return true;
}

/* = = = = = = = = = = = = = = sessions and questions to the client = = = = = = = = = = = = = = */

#define SESSION_MAX 256
#define SESSION_TTL 3600	// seconds without a request
#define ASK_TIMEOUT_MS 120000

static void
hex_id (char *out, size_t bytes)
{
  unsigned char rnd[16];
  static const char *hx = "0123456789abcdef";
  if (bytes > sizeof (rnd) || !mcpc_random_bytes (rnd, bytes))
    {
      // without a source of randomness an id would be guessable, refuse to make one
      out[0] = 0;
      return;
    }
  for (size_t i = 0; i < bytes; i++)
    {
      out[2 * i] = hx[rnd[i] >> 4];
      out[2 * i + 1] = hx[rnd[i] & 15];
    }
  out[2 * bytes] = 0;
}

static void
session_free_at (mcpc_server_t *sv, size_t i)
{
  free (sv->sessions[i].ccaps);
  sv->sessions[i] = sv->sessions[--sv->nsessions];
}

// Called with sv->mu held (from the handler of initialize).
bool
mcpc_http_session_create (mcpc_server_t *sv, struct mcpc_http_chan *ch, mcpc_pver_t pver, const char *caps, size_t caps_len)
{
  const long now = (long) time (nullptr);
  for (size_t i = 0; i < sv->nsessions;)
    if (now - sv->sessions[i].last > SESSION_TTL)
      session_free_at (sv, i);
    else
      i++;
  if (sv->nsessions >= SESSION_MAX)
    {
      size_t oldest = 0;
      for (size_t i = 1; i < sv->nsessions; i++)
	if (sv->sessions[i].used < sv->sessions[oldest].used)
	  oldest = i;
      session_free_at (sv, oldest);
    }

  struct mcpc_session *ns = mcpc_realloc (sv->sessions, (sv->nsessions + 1) * sizeof (struct mcpc_session));
  char *cp = mcpc_alloc (caps_len + 1);
  if (ns == nullptr || cp == nullptr)
    {
      free (cp);
      if (ns != nullptr)
	sv->sessions = ns;
      return false;
    }
  sv->sessions = ns;
  struct mcpc_session *s = &sv->sessions[sv->nsessions];
  hex_id (s->id, 16);
  if (s->id[0] == 0)
    {
      free (cp);
      return false;
    }
  memcpy (cp, caps, caps_len);
  cp[caps_len] = 0;
  s->pver = pver;
  s->ccaps = cp;
  s->ccaps_len = caps_len;
  s->last = now;
  s->used = ++sv->session_clock;
  sv->nsessions++;
  snprintf (ch->session_out, sizeof (ch->session_out), "%s", s->id);
  return true;
}

// Look a session up and hand out a private copy of what it stores. Returns false if there is none.
static bool
session_get (mcpc_server_t *sv, const char *id, size_t idl, mcpc_pver_t *pver, char **caps, size_t *caps_len)
{
  bool found = false;
  mcpc_mutex_lock (&sv->mu);
  for (size_t i = 0; i < sv->nsessions && !found; i++)
    {
      struct mcpc_session *s = &sv->sessions[i];
      if (strlen (s->id) == idl && memcmp (s->id, id, idl) == 0)
	{
	  s->last = (long) time (nullptr);
	  s->used = ++sv->session_clock;
	  *pver = s->pver;
	  *caps = mcpc_alloc (s->ccaps_len + 1);
	  if (*caps != nullptr)
	    {
	      memcpy (*caps, s->ccaps, s->ccaps_len);
	      (*caps)[s->ccaps_len] = 0;
	      *caps_len = s->ccaps_len;
	      found = true;
	    }
	}
    }
  mcpc_mutex_unlock (&sv->mu);
  return found;
}

static bool
session_delete (mcpc_server_t *sv, const char *id, size_t idl)
{
  bool found = false;
  mcpc_mutex_lock (&sv->mu);
  for (size_t i = 0; i < sv->nsessions && !found; i++)
    if (strlen (sv->sessions[i].id) == idl && memcmp (sv->sessions[i].id, id, idl) == 0)
      {
	session_free_at (sv, i);
	found = true;
      }
  mcpc_mutex_unlock (&sv->mu);
  return found;
}

// An answer to one of our questions arrived in a POST of its own.
static void
deliver_answer (mcpc_server_t *sv, const char *body, size_t n, const char *session)
{
  const char *idt;
  int idl;
  if (mjson_find (body, (int) n, "$.id", &idt, &idl) != MJSON_TOK_STRING || idl < 3 || idl > 40)
    return;
  char id[40];
  if (mjson_get_string (idt, idl, "$", id, sizeof (id)) < 0)
    return;

  const char *res;
  int rl;
  bool failed = false;
  if (mjson_find (body, (int) n, "$.result", &res, &rl) == MJSON_TOK_INVALID)
    {
      if (mjson_find (body, (int) n, "$.error", &res, &rl) == MJSON_TOK_INVALID)
	return;
      failed = true;
    }
  char *copy = mcpc_alloc ((size_t) rl + 1);
  if (copy == nullptr)
    return;
  memcpy (copy, res, (size_t) rl);
  copy[rl] = 0;

  mcpc_cond_lock (&sv->pcv);
  for (struct mcpc_pending *p = sv->pending; p != nullptr; p = p->next)
    {
      // the answer must come from the session the question was asked in, or anyone could answer anyone's question
      if (!p->done && strcmp (p->id, id) == 0 && strcmp (p->session, session) == 0)
	{
	  p->json = copy;
	  p->failed = failed;
	  p->done = true;
	  copy = nullptr;
	  mcpc_cond_broadcast (&sv->pcv);
	  break;
	}
    }
  mcpc_cond_unlock (&sv->pcv);
  free (copy);
}

// Ask a client of an initialize-based revision something on the stream of the request being served and
// wait for the answer. The server lock is given up meanwhile, or no other request could run to deliver it.
mcpc_errcode_t
mcpc_http_request_client (mcpc_server_t *sv, mcpc_conn_t *conn, const char *method, const char *params_json, char *ret, size_t ret_cap,
			  size_t *ret_len)
{
  struct mcpc_http_chan *ch = conn->http;
  // without a stream there is nowhere to put the question, without a session nobody could answer it safely
  if (ch == nullptr || !ch->sse_ok || ch->session[0] == 0)
    return MCPC_EC_UNSUPPORTED;

  struct mcpc_pending *p = mcpc_calloc (sizeof (*p));
  if (p == nullptr)
    return MCPC_EC_BUG;
  strcpy (p->id, "mcpc-");
  hex_id (p->id + 5, 8);
  if (p->id[5] == 0)
    {
      free (p);
      return MCPC_EC_BUG;
    }
  snprintf (p->session, sizeof (p->session), "%s", ch->session);

  mcpc_cond_lock (&sv->pcv);
  p->next = sv->pending;
  sv->pending = p;
  mcpc_cond_unlock (&sv->pcv);

  char *msg = mjson_aprintf ("{\"jsonrpc\":\"2.0\",\"id\":%Q,\"method\":%Q,\"params\":%s}", p->id, method, params_json);
  if (msg != nullptr)
    mcpc_http_chan_event (ch, msg, strlen (msg));
  free (msg);

  mcpc_errcode_t ec = MCPC_EC_CONN_CLOSED;
  const unsigned depth = mcpc_mutex_release_all (&sv->mu);
  for (unsigned waited = 0; waited < ASK_TIMEOUT_MS; waited += SLICE_MS)
    {
      mcpc_cond_lock (&sv->pcv);
      if (!p->done)
	mcpc_cond_wait_ms (&sv->pcv, SLICE_MS);
      const bool done = p->done;
      mcpc_cond_unlock (&sv->pcv);
      if (done)
	{
	  ec = p->failed ? MCPC_EC_CLIENT_ERR : MCPC_EC_0;
	  break;
	}
      if (mcpc_flag_get (&sv->stop))
	break;
      if (mcpc_net_peer_gone (ch->fd))
	{
	  ec = MCPC_EC_CANCELLED;	// the client closed the request's stream
	  break;
	}
    }
  mcpc_mutex_restore (&sv->mu, depth);

  // take the question off the list and copy the answer out
  mcpc_cond_lock (&sv->pcv);
  for (struct mcpc_pending **pp = &sv->pending; *pp != nullptr; pp = &(*pp)->next)
    if (*pp == p)
      {
	*pp = p->next;
	break;
      }
  mcpc_cond_unlock (&sv->pcv);

  if (ec == MCPC_EC_0 && p->json != nullptr)
    {
      const size_t jl = strlen (p->json);
      if (ret_len != nullptr)
	*ret_len = jl;
      if (jl >= ret_cap)
	ec = MCPC_EC_BUFCAP;
      else
	memcpy (ret, p->json, jl + 1);
    }
  free (p->json);
  free (p);
  return ec;
}

/* = = = = = = = = = = = = = = a connection = = = = = = = = = = = = = = */

struct conn_arg
{
  mcpc_server_t *sv;
  mcpc_fd_t fd;
  bool loopback;
};

// Read until at least one byte arrives, the deadline passes or the server is asked to stop.
// Returns count, 0 on end of stream, -1 on error, -2 on timeout, -3 when stopping.
static long
read_wait (mcpc_server_t *sv, mcpc_fd_t fd, void *buf, size_t n, unsigned timeout_ms)
{
  unsigned waited = 0;
  for (;;)
    {
      if (mcpc_flag_get (&sv->stop))
	return -3;
      const unsigned slice = timeout_ms - waited < SLICE_MS ? timeout_ms - waited : SLICE_MS;
      const long got = mcpc_net_read (fd, buf, n, (int) slice);
      if (got != -2)
	return got;
      waited += slice;
      if (waited >= timeout_ms)
	return -2;
    }
}

// An open subscriptions/listen stream: stay on the connection until the client leaves or the
// server closes the stream, sending keep-alive comments in between.
static void
listen_loop (mcpc_server_t *sv, mcpc_conn_t *conn, struct mcpc_http_chan *ch)
{
  unsigned quiet = 0;
  for (;;)
    {
      mcpc_mutex_lock (&sv->mu);
      const bool alive = ch->listen_open && ch->state == MCPC_CHAN_SSE && !ch->broken;
      mcpc_mutex_unlock (&sv->mu);
      if (!alive)
	break;

      const int w = mcpc_net_wait_readable (ch->fd, SLICE_MS);
      if (w < 0 || (w == 1 && mcpc_net_peer_gone (ch->fd)))
	break;			// the client closed the stream: that cancels it
      if (w == 1)
	{
	  char junk[256];
	  mcpc_net_read (ch->fd, junk, sizeof (junk), 0);	// nothing is expected, drop it
	  continue;
	}
      quiet += SLICE_MS;
      if (quiet >= KEEPALIVE_MS)
	{
	  quiet = 0;
	  mcpc_mutex_lock (&sv->mu);
	  mcpc_http_chan_comment (ch);
	  mcpc_mutex_unlock (&sv->mu);
	}
    }

  // forget the stream: the connection is going away
  mcpc_mutex_lock (&sv->mu);
  for (size_t i = 0; i < conn->nlistens; i++)
    if (conn->listens[i].chan == ch)
      {
	struct mcpc_listen *l = &conn->listens[i];
	free (l->id);
	for (size_t j = 0; j < l->nuris; j++)
	  free (l->uris[j]);
	free (l->uris);
	conn->listens[i] = conn->listens[--conn->nlistens];
	break;
      }
  ch->listen_open = false;
  mcpc_http_chan_end (ch);
  mcpc_mutex_unlock (&sv->mu);
}

static const char *
find_ci (const char *hay, size_t hl, const char *needle)
{
  const size_t nl = strlen (needle);
  for (size_t i = 0; i + nl <= hl; i++)
    if (strncasecmp_ascii (hay + i, needle, nl))
      return hay + i;
  return nullptr;
}

static void
conn_main (void *p)
{
  struct conn_arg a = *(struct conn_arg *) p;
  free (p);
  mcpc_server_t *sv = a.sv;
  const mcpc_fd_t fd = a.fd;
  const mcpc_http_opts_t *o = &sv->http;

  mcpc_net_set_send_timeout (fd, 15000);

  // join the connection pool so the core can find this connection by its socket
  mcpc_mutex_lock (&sv->mu);
  mcpc_conn_t *conn = mcpc_conn_new (MCPC_CONN_FD, (void *) &fd);
  const mcpc_errcode_t regec = mcpc_connpool_addref ((mcpc_connpool_t *) sv->connpool, conn);
  mcpc_mutex_unlock (&sv->mu);
  if (regec != MCPC_EC_0)
    {
      mcpc_conn_free (conn);
      reply (fd, 503, "Service Unavailable", "Retry-After: 1\r\n", nullptr, true);
      mcpc_net_close (fd);
      mcpc_mutex_lock (&sv->cmu);
      sv->nconns--;
      mcpc_mutex_unlock (&sv->cmu);
      return;
    }

  struct mcpc_http_chan chan;
  mcpc_http_chan_init (&chan, fd);
  conn->http = &chan;

  char *rb = mcpc_alloc (MCPC_HTTP_MAX_HEAD + 1);
  size_t rb_len = 0;
  char *body = nullptr;
  bool first = true;

  while (rb != nullptr && !mcpc_flag_get (&sv->stop))
    {
      // ---- the request head
      struct mcpc_http_req rq;
      const char *perr = "";
      int pr = MCPC_HTTP_INCOMPLETE;
      unsigned budget = first ? o->header_timeout_ms : o->idle_timeout_ms;
      bool got_any = rb_len > 0;
      for (;;)
	{
	  if (rb_len > 0)
	    {
	      pr = mcpc_http_parse_head (rb, rb_len, &rq, &perr);
	      if (pr != MCPC_HTTP_INCOMPLETE)
		break;
	    }
	  if (rb_len >= MCPC_HTTP_MAX_HEAD)
	    {
	      pr = MCPC_HTTP_TOOBIG;
	      break;
	    }
	  const long got = read_wait (sv, fd, rb + rb_len, MCPC_HTTP_MAX_HEAD - rb_len, budget);
	  if (got <= 0)
	    {
	      if (got == -2 && got_any)
		reply (fd, 408, "Request Timeout", nullptr, nullptr, true);
	      pr = 99;		// closed, timed out or stopping
	      break;
	    }
	  if (!got_any)
	    {
	      got_any = true;
	      budget = o->header_timeout_ms;	// the clock for a complete request starts with its first byte
	    }
	  rb_len += (size_t) got;
	}
      first = false;
      if (pr == 99)
	break;
      if (pr == MCPC_HTTP_BAD || pr == MCPC_HTTP_TOOBIG || pr == MCPC_HTTP_UNSUPPORTED)
	{
	  const int st = pr == MCPC_HTTP_TOOBIG ? 431 : pr == MCPC_HTTP_UNSUPPORTED ? 501 : 400;
	  reply (fd, st, st == 431 ? "Request Header Fields Too Large" : st == 501 ? "Not Implemented" : "Bad Request", nullptr, nullptr, true);
	  break;
	}

      // ---- routing and the checks that come before the body
      bool keep = rq.keep_alive;
      size_t tl = rq.target_len;
      const char *q = memchr (rq.target, '?', tl);
      if (q != nullptr)
	tl = (size_t) (q - rq.target);

      if (!host_ok (sv, &rq, a.loopback) || !origin_ok (sv, &rq))
	{
	  reply_rpc_error (fd, 403, "Forbidden", nullptr, 0, -32600, "Forbidden: invalid Origin or Host", nullptr, true);
	  break;
	}
      if (tl != strlen (o->path) || memcmp (rq.target, o->path, tl) != 0)
	{
	  reply (fd, 404, "Not Found", nullptr, nullptr, true);
	  break;
	}
      if (strcmp (rq.method, "DELETE") == 0)
	{
	  // ends a session of an initialize-based client
	  size_t sl = 0;
	  const char *sid = mcpc_http_hdr (&rq, "mcp-session-id", &sl);
	  if (sid == nullptr)
	    reply (fd, 405, "Method Not Allowed", "Allow: POST, DELETE\r\n", nullptr, true);
	  else if (session_delete (sv, sid, sl))
	    reply (fd, 200, "OK", nullptr, nullptr, false);
	  else
	    reply (fd, 404, "Not Found", nullptr, nullptr, false);
	  if (!rq.keep_alive)
	    break;
	  // a DELETE has no body we care about, but make sure none is left to be mistaken for a request
	  if (rq.has_cl && rq.content_length > 0)
	    break;
	  memmove (rb, rb + rq.head_len, rb_len - rq.head_len);
	  rb_len -= rq.head_len;
	  continue;
	}
      if (strcmp (rq.method, "POST") != 0)
	{
	  // no standalone GET stream: only POST is served
	  reply (fd, 405, "Method Not Allowed", "Allow: POST, DELETE\r\n", nullptr, true);
	  break;
	}
      size_t ctl = 0;
      const char *ct = mcpc_http_hdr (&rq, "content-type", &ctl);
      if (ct == nullptr || ctl < 16 || !strncasecmp_ascii (ct, "application/json", 16) || (ctl > 16 && ct[16] != ';' && ct[16] != ' '))
	{
	  reply (fd, 415, "Unsupported Media Type", nullptr, nullptr, true);
	  break;
	}
      size_t acl = 0;
      const char *acc = mcpc_http_hdr (&rq, "accept", &acl);
      chan.sse_ok = acc != nullptr && (find_ci (acc, acl, "text/event-stream") != nullptr || find_ci (acc, acl, "*/*") != nullptr);
      const bool json_ok = acc == nullptr || find_ci (acc, acl, "application/json") != nullptr || find_ci (acc, acl, "*/*") != nullptr ||
	find_ci (acc, acl, "application/*") != nullptr;
      if (!json_ok && !chan.sse_ok)
	{
	  reply (fd, 406, "Not Acceptable", nullptr, nullptr, true);
	  break;
	}
      if (!rq.has_cl && !rq.chunked)
	{
	  reply (fd, 411, "Length Required", nullptr, nullptr, true);
	  break;
	}
      if (rq.has_cl && rq.content_length > o->max_body)
	{
	  reply (fd, 413, "Content Too Large", nullptr, nullptr, true);
	  break;
	}

      // ---- the body: what followed the head in the buffer, then the rest from the socket
      if (rq.expect_continue && rq.head_len == rb_len)
	mcpc_net_write (fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
      const size_t after = rb_len - rq.head_len;
      size_t blen = 0, consumed = 0;
      bool body_ok = true;
      if (rq.has_cl)
	{
	  body = mcpc_alloc (rq.content_length + 1);
	  if (body == nullptr)
	    break;
	  const size_t have = after < rq.content_length ? after : rq.content_length;
	  memcpy (body, rb + rq.head_len, have);
	  blen = have;
	  consumed = have;
	  while (blen < rq.content_length)
	    {
	      const long got = read_wait (sv, fd, body + blen, rq.content_length - blen, o->header_timeout_ms);
	      if (got <= 0)
		{
		  body_ok = false;
		  break;
		}
	      blen += (size_t) got;
	    }
	}
      else
	{
	  size_t cap = after + 4096 < o->max_body + 1 ? after + 4096 : o->max_body + 1;
	  body = mcpc_alloc (cap + 1);
	  struct mcpc_chunk_dec dec;
	  mcpc_chunk_init (&dec, o->max_body);
	  int cr = MCPC_HTTP_INCOMPLETE;
	  const char *in = rb + rq.head_len;
	  size_t inl = after;
	  char *rd = nullptr;
	  for (;;)
	    {
	      if (inl > 0)
		{
		  size_t used = 0, outn = 0;
		  if (blen + inl > cap)
		    {
		      size_t ncap = cap * 2 > blen + inl ? cap * 2 : blen + inl;
		      if (ncap > o->max_body + 1)
			ncap = o->max_body + 1;
		      char *nb = ncap >= blen + inl ? mcpc_realloc (body, ncap + 1) : nullptr;
		      if (nb == nullptr)
			{
			  cr = MCPC_HTTP_TOOBIG;
			  break;
			}
		      body = nb;
		      cap = ncap;
		    }
		  cr = mcpc_chunk_feed (&dec, in, inl, &used, body + blen, &outn);
		  blen += outn;
		  if (in == rb + rq.head_len)
		    consumed += used;
		  in += used;
		  inl -= used;
		}
	      if (cr != MCPC_HTTP_INCOMPLETE)
		break;
	      if (rd == nullptr)
		rd = mcpc_alloc (4096);
	      const long got = rd != nullptr ? read_wait (sv, fd, rd, 4096, o->header_timeout_ms) : 0;
	      if (got <= 0)
		{
		  cr = MCPC_HTTP_BAD;
		  break;
		}
	      in = rd;
	      inl = (size_t) got;
	      consumed = after;	// the head buffer held none of this
	    }
	  free (rd);
	  if (cr == MCPC_HTTP_TOOBIG)
	    {
	      reply (fd, 413, "Content Too Large", nullptr, nullptr, true);
	      body_ok = false;
	      keep = false;
	    }
	  else if (cr != MCPC_HTTP_OK)
	    body_ok = false;
	}
      if (!body_ok)
	{
	  if (keep)
	    reply (fd, 400, "Bad Request", nullptr, nullptr, true);
	  break;
	}
      body[blen] = 0;

      // pipelined bytes after this request stay for the next one
      if (rq.has_cl && after > consumed)
	{
	  memmove (rb, rb + rq.head_len + consumed, after - consumed);
	  rb_len = after - consumed;
	}
      else if (!rq.has_cl && after > consumed && consumed < after)
	{
	  memmove (rb, rb + rq.head_len + consumed, after - consumed);
	  rb_len = after - consumed;
	}
      else
	rb_len = 0;

      // ---- the JSON-RPC part
      struct reqinfo ri;
      struct vfail vf;
      mcpc_pver_t hdr_pver = MCPC_PV_2025_03_26;
      if (!validate (sv, &rq, body, blen, &ri, &vf, &hdr_pver))
	{
	  reply_rpc_error (fd, vf.status, vf.status == 400 ? "Bad Request" : "Error", ri.id, ri.id_len, vf.code, vf.msg,
			   vf.data[0] ? vf.data : nullptr, !keep);
	  free (body);
	  body = nullptr;
	  if (!keep)
	    break;
	  continue;
	}
      size_t sidl = 0;
      const char *sid = mcpc_http_hdr (&rq, "mcp-session-id", &sidl);
      char sidbuf[40];
      snprintf (sidbuf, sizeof (sidbuf), "%.*s", (int) (sidl < 39 ? sidl : 39), sid != nullptr ? sid : "");

      if (ri.respond_202)
	{
	  // an answer to a question one of our requests asked
	  deliver_answer (sv, body, blen, sidbuf);
	  reply (fd, 202, "Accepted", nullptr, nullptr, !keep);
	  free (body);
	  body = nullptr;
	  if (!keep)
	    break;
	  continue;
	}

      // an initialize-based request may name a session, which carries what the handshake settled
      free (chan.caps);
      chan.caps = nullptr;
      chan.caps_len = 0;
      chan.session[0] = 0;
      chan.session_out[0] = 0;
      if (!ri.modern && !ri.is_init && sid != nullptr)
	{
	  mcpc_pver_t spv = MCPC_PV_NONE;
	  if (!session_get (sv, sidbuf, strlen (sidbuf), &spv, &chan.caps, &chan.caps_len))
	    {
	      reply_rpc_error (fd, 404, "Not Found", ri.id, ri.id_len, -32001, "Session not found", nullptr, !keep);
	      free (body);
	      body = nullptr;
	      if (!keep)
		break;
	      continue;
	    }
	  hdr_pver = spv;
	  snprintf (chan.session, sizeof (chan.session), "%.32s", sidbuf);
	}

      mcpc_mutex_lock (&sv->mu);
      chan.state = MCPC_CHAN_IDLE;
      chan.status = 200;
      chan.responded = false;
      chan.listen_open = false;
      chan.chunked = rq.minor >= 1;
      chan.keep_alive = keep && !mcpc_flag_get (&sv->stop);
      chan.hdr_pver = hdr_pver;
      handle_inone (sv, body, blen, fd);
      mcpc_mutex_unlock (&sv->mu);
      // the body stays until the request is finished with: ri.id points into it

      // ---- make sure the request ended properly
      mcpc_mutex_lock (&sv->mu);
      if (chan.state == MCPC_CHAN_IDLE && !chan.broken)
	{
	  if (ri.has_id)
	    {
	      // a request must never go unanswered
	      char *e = mjson_aprintf ("{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"error\":{\"code\":-32603,\"message\":\"no response produced\"}}",
				       ri.id_len, ri.id);
	      chan.status = 500;
	      if (e != nullptr)
		mcpc_http_chan_final (&chan, e, strlen (e));
	      free (e);
	    }
	  else
	    reply (fd, 202, "Accepted", nullptr, nullptr, !chan.keep_alive);
	}
      else if (chan.state == MCPC_CHAN_SSE && !chan.listen_open)
	mcpc_http_chan_end (&chan);
      const bool listening = chan.listen_open && !chan.broken;
      mcpc_mutex_unlock (&sv->mu);
      free (body);
      body = nullptr;

      if (listening)
	{
	  listen_loop (sv, conn, &chan);
	  break;		// a stream consumes the connection
	}
      if (chan.broken || !chan.keep_alive)
	break;
    }

  // leave the pool, closing whatever streams the connection still held
  mcpc_mutex_lock (&sv->mu);
  conn->http = nullptr;
  mcpc_connpool_remove_by_sock ((mcpc_connpool_t *) sv->connpool, fd);
  mcpc_mutex_unlock (&sv->mu);

  free (body);
  free (rb);
  free (chan.caps);
  mcpc_net_close (fd);
  mcpc_mutex_lock (&sv->cmu);
  sv->nconns--;
  mcpc_mutex_unlock (&sv->cmu);
}

/* = = = = = = = = = = = = = = the server = = = = = = = = = = = = = = */

mcpc_errcode_t
mcpc_http_run (mcpc_server_t *sv, const mcpc_http_opts_t *opts)
{
  if (opts != nullptr)
    sv->http = *opts;
  mcpc_http_opts_t *o = &sv->http;
  if (o->host == nullptr)
    o->host = "127.0.0.1";
  if (o->port == 0 && opts == nullptr)
    o->port = 8080;
  if (o->path == nullptr || o->path[0] != '/')
    o->path = "/mcp";
  if (o->max_body == 0)
    o->max_body = DEFAULT_MAX_BODY;
  if (o->max_conns == 0)
    o->max_conns = DEFAULT_MAX_CONNS;
  if (o->max_conns > MAX_CONNS_LIMIT)
    o->max_conns = MAX_CONNS_LIMIT;
  if (o->header_timeout_ms == 0)
    o->header_timeout_ms = DEFAULT_HEADER_TIMEOUT;
  if (o->idle_timeout_ms == 0)
    o->idle_timeout_ms = DEFAULT_IDLE_TIMEOUT;

  bool loopback = false;
  unsigned short port = o->port;
  mcpc_fd_t lfd = mcpc_net_listen (o->host, o->port, &port, &loopback);
  if (lfd == MCPC_FD_INVALID)
    return MCPC_EC_SVIMPL_BIND;
  sv->lfd = lfd;
  mcpc_flag_set (&sv->stop, 0);
  if (o->on_listening != nullptr)
    o->on_listening (port, o->ud);

  while (!mcpc_flag_get (&sv->stop))
    {
      mcpc_fd_t c = mcpc_net_accept (lfd, SLICE_MS);
      if (c == MCPC_FD_INVALID)
	continue;

      mcpc_mutex_lock (&sv->cmu);
      const bool full = sv->nconns >= o->max_conns;
      if (!full)
	sv->nconns++;
      mcpc_mutex_unlock (&sv->cmu);
      if (full)
	{
	  reply (c, 503, "Service Unavailable", "Retry-After: 1\r\n", nullptr, true);
	  mcpc_net_close (c);
	  continue;
	}

      struct conn_arg *ca = mcpc_alloc (sizeof (*ca));
      if (ca == nullptr)
	{
	  mcpc_net_close (c);
	  mcpc_mutex_lock (&sv->cmu);
	  sv->nconns--;
	  mcpc_mutex_unlock (&sv->cmu);
	  continue;
	}
      ca->sv = sv;
      ca->fd = c;
      ca->loopback = loopback;
      if (!mcpc_thread_start (conn_main, ca))
	{
	  free (ca);
	  mcpc_net_close (c);
	  mcpc_mutex_lock (&sv->cmu);
	  sv->nconns--;
	  mcpc_mutex_unlock (&sv->cmu);
	}
    }

  // shutting down: end every open stream the proper way, then wait for the connections to leave
  mcpc_net_close (lfd);
  sv->lfd = MCPC_FD_INVALID;
  mcpc_mutex_lock (&sv->mu);
  for (mcpc_conn_t *c = sv->connpool->head; c != nullptr; c = c->nex)
    if (c->typ == MCPC_CONN_FD && c->nlistens > 0)
      _close_listens_public (sv, c);
  mcpc_mutex_unlock (&sv->mu);

  for (int i = 0; i < 100; i++)
    {
      mcpc_mutex_lock (&sv->cmu);
      const size_t n = sv->nconns;
      mcpc_mutex_unlock (&sv->cmu);
      if (n == 0)
	break;
      mcpc_net_sleep_ms (50);
    }
  return MCPC_EC_0;
}
