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

#define __TU__ "server"

#include "server.h"

#include "ext_string.h"

#include "alloc.h"
#include <mcpc/anydata.h>
#include "errcode.h"
#include "log.h"
#include "anydata.h"
#include "ctx.h"
#include "frame.h"
#include "jsonutil.h"
#include "retbuf.h"
#include "rsc.h"
#include "tool.h"
#include "ucbr.h"
#include "serlz.h"

#define BUFCAP_INIT_JSONREQ 1024
#define CH_SPC 0x20
#define CH_LF 0x0a
#define CH_LBRC 0x7b
#define CH_RBRC 0x7d
#define CH_QUO 0x22
#define CH_BSLASH 0x5c


#if is_unix
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#elif is_win
#include <winsock2.h>
typedef i32_t socklen_t;
#endif

struct mjson_fixedbuf
_new_mjson_fixedbuf ()
{
  struct mjson_fixedbuf fbuf = { nullptr, 0, 0 };
  char *buf = mcpc_alloc (1024);
  fbuf.ptr = buf;
  fbuf.size = 1024;		// TODO  cap
  fbuf.len = 0;
  return fbuf;
}

// Result buffer for a client speaking the revision negotiated on conn, with the caching hints set.
static retbuf_t
_rb_new (const mcpc_server_t *sv, const mcpc_conn_t *conn, size_t hint)
{
  retbuf_t rb = retbuf_new (hint);
  mcpc_assert (rb != nullptr, MCPC_EC_BUG);
  rb->feat = mcpc_pver_feats (conn->pver);
  rb->ttl_ms = sv->ttl_ms;
  rb->cache_public = sv->cache_public;
  return rb;
}

// Send a serialized result (takes ownership of rtbf). A buffer that could not be fully written
// is reported as an internal error rather than sent truncated. The 2026-07-28 revision wants
// a resultType on every result and says who answered in _meta.
static void
_reply_result (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock, retbuf_t rtbf, const char *result_type)
{
  if (!retbuf_is_ok (rtbf) || retbuf_len (rtbf) < 2)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "failed to serialize response", NULL);
      retbuf_free (rtbf);
      return;
    }

  if (!(rtbf->feat & MCPC_FEAT_MODERN))
    {
      jsonrpc_return_success2 (r, mcpc_sock_invalid (csock), "%.*s", (int) retbuf_len (rtbf), retbuf_data (rtbf));
      retbuf_free (rtbf);
      return;
    }

  const char *body = retbuf_data (rtbf);
  const int body_len = (int) retbuf_len (rtbf);	// "{...}"
  const bool empty = body_len == 2;
  const char *tok = nullptr;
  int tok_len = 0;
  const bool has_meta = mjson_find (body, body_len, "$._meta", &tok, &tok_len) != MJSON_TOK_INVALID;

  retbuf_t info = retbuf_new (128);
  retbuf_t out = retbuf_new (body_len + 256);
  mcpc_assert (info != nullptr && out != nullptr, MCPC_EC_BUG);
  info->feat = rtbf->feat;
  serlz_serverInfo (info, sv);

  rtbuf_printf_mjfmt (out, u8c1 ("{%Q:%Q"), "resultType", result_type);
  if (!empty)
    rtbuf_printf_mjfmt (out, u8c1 (",%.*s"), body_len - 2, body + 1);
  if (!has_meta)
    rtbuf_printf_mjfmt (out, u8c1 (",%Q:{%Q:%.*s}"), "_meta", "io.modelcontextprotocol/serverInfo",
			(int) retbuf_len (info), retbuf_data (info));
  rtbuf_printf_mjfmt (out, u8c1 ("}"));

  if (!retbuf_is_ok (out))
    jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "failed to serialize response", NULL);
  else
    jsonrpc_return_success2 (r, mcpc_sock_invalid (csock), "%.*s", (int) retbuf_len (out), retbuf_data (out));

  retbuf_free (info);
  retbuf_free (out);
  retbuf_free (rtbf);
}

static void
_reply_retbuf (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock, retbuf_t rtbf)
{
  _reply_result (sv, r, csock, rtbf, "complete");
}

void
_connpool_init (mcpc_connpool_t *ret)
{
  mcpc_connpool_t connpool = { 255, 0, nullptr };
  memcpy (ret, &connpool, sizeof (mcpc_connpool_t));
}

bool
_connpool_is_buggy (const mcpc_connpool_t *connpool)
{
  if (connpool == nullptr)
    {
      return true;
    }
  if (connpool->cap == 0)
    {
      return true;
    }
  if (connpool->len != 0)
    {
      u8_t nexist = 0;
      mcpc_conn_t *cur_conn = connpool->head;
      while (cur_conn != nullptr)
	{
	  cur_conn = cur_conn->nex;
	  nexist += 1;		// OR
	  // mcpc_assert(!ckd_add(&nexist, nexist, 1), MCPC_EC_BUG);
	}
      if (nexist != connpool->len)
	{
	  return true;
	}
    }
  return false;
}

// extract a field's unquoted string
mcpc_errcode_t
_tulc_jstr_get_field_u8str (struct jsonrpc_request *r, const char8_t *jstr, size_t jstr_len, const char8_t *jpathnt,
			    char8_t **ret, size_t *ret_len, size_t ret_cap, bool not_empty)
{
  const char *jstr2 = (const char *) jstr;
  int jstr_len2 = (int) jstr_len;
  const char *jpathnt2 = (const char *) jpathnt;
  const char8_t *ret_q = nullptr;
  int32_t ret_q_len = 0;
  int tmptok;

  tmptok = mjson_find (jstr2, jstr_len2, jpathnt2, (const char **) &ret_q, (int *) &ret_q_len);
  if (tmptok != MJSON_TOK_STRING || ret_q == nullptr || ret_q_len == 0)
    goto field_invalid;

  int getres = mjson_get_string ((const char *) ret_q, ret_q_len, "$", (char *) *ret, ret_cap);
  if (getres == -1)
    return MCPC_EC_BUFCAP;
  if (not_empty && getres == 0)
    goto field_invalid;
  *ret_len = (size_t) getres;

  return MCPC_EC_0;

field_invalid:
  jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "field invalid", "%Q", jpathnt2);
  return MCPC_EC_JSON_FIELD_INVALID;
}

// extract a field's unquoted string
mcpc_errcode_t
_tulc_jparams_get_field_u8str (struct jsonrpc_request *r,
			       const char8_t *jpathnt, char8_t **ret, size_t *ret_len, size_t ret_cap, bool not_empty)
{
  return _tulc_jstr_get_field_u8str (r, (const char8_t *) r->params, r->params_len, jpathnt, ret, ret_len,
				     ret_cap, not_empty);
}

// extract a field's value, that is, everything
mcpc_errcode_t
_tulc_jparams_getref_field_obj (struct jsonrpc_request *r, const char8_t *jpathnt, const char8_t **ret, size_t *ret_len)
{
  const char *jpathnt2 = (const char *) jpathnt;
  int tmptok;

  tmptok = mjson_find (r->params, r->params_len, jpathnt2, (const char **) ret, (int *) ret_len);
  if (tmptok != MJSON_TOK_OBJECT || ret == nullptr || ret_len == 0)
    goto field_invalid;

  return MCPC_EC_0;

field_invalid:
  jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "field invalid", "%Q", jpathnt2);
  return MCPC_EC_JSON_FIELD_INVALID;
}


// extract a field's value, that is, everything
mcpc_errcode_t
_tulc_jparams_getref_field_u8str_q (struct jsonrpc_request *r, const char8_t *jpathnt, const char8_t **ret,
				    size_t *ret_len)
{
  const char *jpathnt2 = (const char *) jpathnt;
  int tmptok;

  tmptok = mjson_find (r->params, r->params_len, jpathnt2, (const char **) ret, (int *) ret_len);
  if (tmptok != MJSON_TOK_STRING || ret == nullptr || ret_len == 0)
    goto field_invalid;

  return MCPC_EC_0;

field_invalid:
  jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "field invalid", "%Q", jpathnt2);
  return MCPC_EC_JSON_FIELD_INVALID;
}


// extract a field's value, that is, everything
mcpc_errcode_t
_tulc_jstr_get_field_number (struct jsonrpc_request *r, const char8_t *jstr, size_t jstr_len,
			     const char8_t *jpathnt, double *ret)
{
  const char *jpathnt2 = (const char *) jpathnt;
  double numval = 0;

  int ngot = mjson_get_number ((const char *) jstr, jstr_len, jpathnt2, &numval);

  if (ngot != 1)
    goto field_invalid;

  *ret = numval;

  return MCPC_EC_0;

field_invalid:
  jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "field invalid", "%Q", jpathnt2);
  return MCPC_EC_JSON_FIELD_INVALID;
}


mcpc_errcode_t
mcpc_conn_free (mcpc_conn_t *conn)
{
  bug_if_nullptr (conn);

  if (conn->cname != nullptr)
    {
      free ((void *) conn->cname);
      conn->cname = nullptr;
      conn->cname_len = 0;
    }

  free (conn->ccaps);
  for (size_t i = 0; i < conn->nsubs; i++)
    free (conn->subs[i]);
  free (conn->subs);
  for (size_t i = 0; i < conn->npend; i++)
    free (conn->pend[i]);
  free (conn->pend);
  for (size_t i = 0; i < conn->nlistens; i++)
    {
      free (conn->listens[i].id);
      for (size_t j = 0; j < conn->listens[i].nuris; j++)
	free (conn->listens[i].uris[j]);
      free (conn->listens[i].uris);
    }
  free (conn->listens);

  if (conn->nex != nullptr)
    {
      mcpc_conn_free (conn->nex);
    }

  free (conn);

  return MCPC_EC_0;
}

mcpc_errcode_t
mcpc_connpool_free (mcpc_connpool_t *connpool)
{
  bug_if_nullptr (connpool);
  // an HTTP server has no connections left once it has stopped serving
  if (connpool->head != nullptr)
    mcpc_conn_free (connpool->head);
  free (connpool);
  return MCPC_EC_0;
}

MCPC_API mcpc_server_t *
mcpc_server_new (mcpc_server_type_t typ, mcpc_toolpool_t *toolpool, u32_t vabeg, ...)
{
  mcpc_assert (typ != MCPC_SV_NONE, MCPC_EC_BUG);
  mcpc_assert (toolpool != nullptr, MCPC_EC_BUG);

  mcpc_server_t *sv = (mcpc_server_t *) mcpc_alloc (sizeof (mcpc_server_t));

  sv->typ = typ;
  sv->toolpool = toolpool;
  sv->prmptpool = mcpc_prmptpool_new ();
  sv->rscpool = mcpc_rscpool_new ();
  sv->rsctplpool = mcpc_anypool_new ();
  sv->rpcres = _new_mjson_fixedbuf ();
  mcpc_mutex_init (&sv->mu);
  sv->connpool = mcpc_alloc (sizeof (mcpc_connpool_t));
  _connpool_init ((mcpc_connpool_t *) sv->connpool);

  if (false)
    ;
  else if (typ == MCPC_SV_IOSTRM)
    {
      va_list ap;
      va_start (ap, vabeg);
      FILE *strm0 = va_arg (ap, FILE *);
      mcpc_assert (strm0 != nullptr, MCPC_EC_BUG);
      FILE *strm1 = va_arg (ap, FILE *);
      mcpc_assert (strm1 != nullptr, MCPC_EC_BUG);
      mcpc_conn_t *conn0 = mcpc_conn_new (MCPC_CONN_STRM, strm0);
      mcpc_assert (MCPC_EC_0 == mcpc_connpool_addref ((mcpc_connpool_t *) sv->connpool, conn0), MCPC_EC_BUG);
      mcpc_conn_t *conn1 = mcpc_conn_new (MCPC_CONN_STRM, strm1);
      mcpc_assert (MCPC_EC_0 == mcpc_connpool_addref ((mcpc_connpool_t *) (sv->connpool), conn1), MCPC_EC_BUG);
      va_end (ap);
    }
  else if (typ == MCPC_SV_TCP || typ == MCPC_SV_HTTP)
    {
    }
  else
    {
      mcpc_assert (false, MCPC_EC_BUG);
    }

  ch8_t *default_name = u8c1 ("untitled");
  u8_t default_name_len = (u8_t) u8strlen (default_name);
  sv->name = mcpc_alloc ((size_t) default_name_len);
  memcpy ((void *) sv->name, default_name, default_name_len);
  sv->name_len = default_name_len;

  ch8_t *default_ver = u8c1 ("0.1");
  u8_t default_ver_len = (u8_t) u8strlen (default_ver);
  sv->ver = mcpc_alloc ((size_t) default_ver_len);
  memcpy ((void *) sv->ver, default_ver, default_ver_len);
  sv->ver_len = default_ver_len;

  sv->capa = 0;
  sv->title = nullptr;
  sv->desc = nullptr;
  sv->website = nullptr;
  sv->icons_json = nullptr;
  sv->instructions = nullptr;
  sv->page_size = 0;
  sv->pmask = MCPC_PROTO_ALL;
  sv->ttl_ms = 0;
  sv->cache_public = false;
  sv->extensions_json = nullptr;
  sv->state_key = nullptr;
  sv->state_key_len = 0;
  sv->state_ttl = 300;
  memset (&sv->http, 0, sizeof (sv->http));
  sv->lfd = MCPC_FD_INVALID;
  mcpc_flag_set (&sv->stop, 0);
  sv->nconns = 0;
  mcpc_mutex_init (&sv->cmu);
  sv->sessions = nullptr;
  sv->nsessions = 0;
  sv->session_clock = 0;
  sv->pending = nullptr;
  mcpc_cond_init (&sv->pcv);

  return sv;
}

MCPC_API mcpc_server_t *
mcpc_server_new_iostrm (const FILE *const strm_in, const FILE *const strm_out)
{
  mcpc_assert (strm_in != nullptr, MCPC_EC_BUG);
  mcpc_assert (strm_out != nullptr, MCPC_EC_BUG);

  mcpc_toolpool_t *toolpool = mcpc_toolpool_new ();
  mcpc_tool_t *head = mcpc_tool_new2 (_FIRST_TOOL_NAME, _FIRST_TOOL_DESC);
  mcpc_toolpool_addfre (toolpool, head);

  mcpc_server_t *sv = mcpc_server_new (MCPC_SV_IOSTRM, toolpool, true, strm_in, strm_out);

  return sv;
}

MCPC_API mcpc_server_t *
mcpc_server_new_http ()
{
  mcpc_toolpool_t *toolpool = mcpc_toolpool_new ();
  mcpc_tool_t *head = mcpc_tool_new2 (_FIRST_TOOL_NAME, _FIRST_TOOL_DESC);
  mcpc_toolpool_addfre (toolpool, head);

  return mcpc_server_new (MCPC_SV_HTTP, toolpool, false);
}

MCPC_API void
mcpc_server_stop (mcpc_server_t *sv)
{
  mcpc_flag_set (&sv->stop, 1);
}

MCPC_API mcpc_errcode_t
mcpc_server_start_http (mcpc_server_t *sv, const mcpc_http_opts_t *opts)
{
  if (_server_is_buggy (sv) || sv->typ != MCPC_SV_HTTP)
    return MCPC_EC_BUG;
  return mcpc_http_run (sv, opts);
}

MCPC_API mcpc_server_t *
mcpc_server_new_tcp ()
{
  mcpc_toolpool_t *toolpool = mcpc_toolpool_new ();
  mcpc_tool_t *head = mcpc_tool_new2 (_FIRST_TOOL_NAME, _FIRST_TOOL_DESC);
  mcpc_toolpool_addfre (toolpool, head);

  mcpc_server_t *sv = mcpc_server_new (MCPC_SV_TCP, toolpool, false);

  return sv;
}

MCPC_API mcpc_errcode_t
mcpc_server_set_name (mcpc_server_t *sv, const char8_t *name, size_t name_len)
{
  bug_if (name_len > (size_t) UINT8_MAX);
  sv->name = mcpc_realloc ((void *) sv->name, name_len);
  memcpy ((void *) sv->name, name, name_len);
  sv->name_len = (u8_t) name_len;
  return MCPC_EC_0;
}

MCPC_API  mcpc_errcode_t
mcpc_server_set_nament (mcpc_server_t *sv, const char8_t *nament)
{
  return mcpc_server_set_name (sv, nament, u8strlen (nament));
}

// replace a heap-owned string field with a copy of (s, len); NULL clears it
static mcpc_errcode_t
_set_str (char **field, const char8_t *s, size_t len)
{
  char *cp = nullptr;
  if (s != nullptr)
    {
      cp = mcpc_alloc (len + 1);
      if (cp == nullptr)
	return MCPC_EC_BUG;
      memcpy (cp, s, len);
      cp[len] = 0;
    }
  free (*field);
  *field = cp;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_set_version (mcpc_server_t *sv, const char8_t *vernt)
{
  const size_t len = u8strlen (vernt);
  if (len > (size_t) UINT8_MAX)
    return MCPC_EC_ANYDATA_LONGMETA1;
  char8_t *cp = mcpc_realloc ((void *) sv->ver, len);
  if (cp == nullptr)
    return MCPC_EC_BUG;
  memcpy (cp, vernt, len);
  sv->ver = cp;
  sv->ver_len = (u8_t) len;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_set_title (mcpc_server_t *sv, const char8_t *titlent)
{
  return _set_str (&sv->title, titlent, titlent ? u8strlen (titlent) : 0);
}

MCPC_API mcpc_errcode_t
mcpc_server_set_description (mcpc_server_t *sv, const char8_t *descnt)
{
  return _set_str (&sv->desc, descnt, descnt ? u8strlen (descnt) : 0);
}

MCPC_API mcpc_errcode_t
mcpc_server_set_website_url (mcpc_server_t *sv, const char8_t *urlnt)
{
  return _set_str (&sv->website, urlnt, urlnt ? u8strlen (urlnt) : 0);
}

MCPC_API mcpc_errcode_t
mcpc_server_set_instructions (mcpc_server_t *sv, const char8_t *textnt)
{
  return _set_str (&sv->instructions, textnt, textnt ? u8strlen (textnt) : 0);
}

MCPC_API mcpc_errcode_t
mcpc_server_set_icons_json (mcpc_server_t *sv, const char8_t *json_array_nt)
{
  if (json_array_nt != nullptr)
    {
      const char *tok = nullptr;
      int toklen = 0;
      if (mjson_find ((const char *) json_array_nt, (int) u8strlen (json_array_nt), "$", &tok, &toklen) !=
	  MJSON_TOK_ARRAY)
	return MCPC_EC_JSON_FIELD_INVALID;
    }
  return _set_str (&sv->icons_json, json_array_nt, json_array_nt ? u8strlen (json_array_nt) : 0);
}

MCPC_API mcpc_errcode_t
mcpc_server_set_protocol_mask (mcpc_server_t *sv, unsigned mask)
{
  if ((mask & MCPC_PROTO_ALL) == 0)
    return MCPC_EC_BUG;
  sv->pmask = mask & MCPC_PROTO_ALL;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_set_cache (mcpc_server_t *sv, unsigned long ttl_ms, bool is_public)
{
  sv->ttl_ms = ttl_ms;
  sv->cache_public = is_public;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_set_extensions_json (mcpc_server_t *sv, const char8_t *json_object_nt)
{
  if (json_object_nt != nullptr)
    {
      const char *tok = nullptr;
      int toklen = 0;
      if (mjson_find ((const char *) json_object_nt, (int) u8strlen (json_object_nt), "$", &tok, &toklen) !=
	  MJSON_TOK_OBJECT)
	return MCPC_EC_JSON_FIELD_INVALID;
    }
  return _set_str (&sv->extensions_json, json_object_nt, json_object_nt ? u8strlen (json_object_nt) : 0);
}

MCPC_API mcpc_errcode_t
mcpc_server_set_state_secret (mcpc_server_t *sv, const void *key, size_t key_len)
{
  uint8_t *cp = nullptr;
  if (key != nullptr && key_len > 0)
    {
      cp = mcpc_alloc (key_len);
      if (cp == nullptr)
	return MCPC_EC_BUG;
      memcpy (cp, key, key_len);
    }
  if (sv->state_key != nullptr)
    memset (sv->state_key, 0, sv->state_key_len);
  free (sv->state_key);
  sv->state_key = cp;
  sv->state_key_len = cp != nullptr ? key_len : 0;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_set_state_ttl (mcpc_server_t *sv, unsigned seconds)
{
  sv->state_ttl = seconds;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_set_page_size (mcpc_server_t *sv, size_t page_size)
{
  sv->page_size = page_size;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_log (mcpc_server_t *sv)
{
  sv->capa |= MCPC_CAPA_LOG;
  return MCPC_EC_0;
}

bool
mcpc_server_supp_log (const mcpc_server_t *sv)
{
  return (sv->capa & MCPC_CAPA_LOG) == MCPC_CAPA_LOG;
}

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_prmt (mcpc_server_t *sv)
{
  sv->capa |= MCPC_CAPA_PRMT;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_prmt_listchg (mcpc_server_t *sv)
{
  sv->capa |= MCPC_CAPA_PRMT_LISTCHG;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_tool (mcpc_server_t *sv)
{
  sv->capa |= MCPC_CAPA_TOOL;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_tool_listchg (mcpc_server_t *sv)
{
  sv->capa |= MCPC_CAPA_TOOL_LISTCHG;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_rsc (mcpc_server_t *sv)
{
  sv->capa |= MCPC_CAPA_RSC;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_rsc_listchg (mcpc_server_t *sv)
{
  sv->capa |= MCPC_CAPA_RSC_LISTCHG;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_rsc_subscr (mcpc_server_t *sv)
{
  sv->capa |= MCPC_CAPA_RSC_SUBSRC;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_complt (mcpc_server_t *sv)
{
  sv->capa |= MCPC_CAPA_COMPLT;
  return MCPC_EC_0;
}

bool
_server_is_buggy (const mcpc_server_t *sv)
{
  if (sv == nullptr)
    return true;
  if (sv->typ == MCPC_SV_NONE)
    return true;
  if (sv->toolpool == nullptr)
    return true;

  return false;
}

MCPC_API mcpc_errcode_t
mcpc_server_add_tool (mcpc_server_t *sv, mcpc_tool_t *tool)
{
  if (_server_is_buggy (sv))
    return MCPC_EC_BUG;
  if (tool == nullptr)
    return MCPC_EC_BUG;
  mcpc_toolpool_addfre ((mcpc_toolpool_t *) sv->toolpool, tool);
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_add_prmpt (mcpc_server_t *sv, mcpc_prmpt_t *prmpt)
{
  if (_server_is_buggy (sv))
    return MCPC_EC_BUG;
  if (prmpt == nullptr)
    return MCPC_EC_BUG;
  mcpc_prmptpool_addfre ((mcpc_prmptpool_t *) sv->prmptpool, prmpt);
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_add_rsctpl (mcpc_server_t *sv, mcpc_rsctpl_t *tpl)
{
  if (_server_is_buggy (sv))
    return MCPC_EC_BUG;
  if (tpl == nullptr)
    return MCPC_EC_BUG;
  mcpc_anypool_addfre ((mcpc_anypool_t *) sv->rsctplpool, tpl);
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_add_rsc (mcpc_server_t *sv, mcpc_rsc_t *rsc)
{
  if (_server_is_buggy (sv))
    return MCPC_EC_BUG;
  if (rsc == nullptr)
    return MCPC_EC_BUG;
  mcpc_rscpool_addfre ((mcpc_rscpool_t *) sv->rscpool, rsc);
  return MCPC_EC_0;
}

bool
mcpc_server_supp_prompts (const mcpc_server_t *sv)
{
  return (sv->capa & MCPC_CAPA_PRMT);
}

bool
mcpc_server_supp_prompts_lschg (const mcpc_server_t *sv)
{
  return (sv->capa & MCPC_CAPA_PRMT_LISTCHG) == MCPC_CAPA_PRMT_LISTCHG;
}

bool
mcpc_server_supp_tools (const mcpc_server_t *sv)
{
  return (sv->capa & MCPC_CAPA_TOOL) == MCPC_CAPA_TOOL;
}

bool
mcpc_server_supp_tools_lschg (const mcpc_server_t *sv)
{
  return (sv->capa & MCPC_CAPA_TOOL_LISTCHG) == MCPC_CAPA_TOOL_LISTCHG;
}

bool
mcpc_server_supp_rscs (const mcpc_server_t *sv)
{
  return (sv->capa & MCPC_CAPA_RSC) == MCPC_CAPA_RSC;
}

bool
mcpc_server_supp_rscs_lschg (const mcpc_server_t *sv)
{
  return (sv->capa & MCPC_CAPA_RSC_LISTCHG) == MCPC_CAPA_RSC_LISTCHG;
}

bool
mcpc_server_supp_rscs_subs (const mcpc_server_t *sv)
{
  return (sv->capa & MCPC_CAPA_RSC_SUBSRC) == MCPC_CAPA_RSC_SUBSRC;
}

bool
mcpc_server_supp_complt (const mcpc_server_t *sv)
{
  return (sv->capa & MCPC_CAPA_COMPLT) == MCPC_CAPA_COMPLT;
}


// Write bytes to one client. The stream transport has a single output stream, for TCP
// csock names the client. A peer that went away is not an error for the server.
static void
_send_bytes (mcpc_server_t *sv, mcpc_sock_t csock, const char *p, size_t len)
{
  if (len == 0)
    return;

  if (sv->typ == MCPC_SV_HTTP)
    {
      // a message that is not the final response of a request is an event on its stream
      mcpc_conn_t *c = mcpc_connpool_find_by_sock (sv->connpool, csock);
      if (c != nullptr && c->http != nullptr)
	mcpc_http_chan_event (c->http, p, len);
      return;
    }

  if (sv->typ == MCPC_SV_IOSTRM)
    {
      tuflog_d ("send:%.*s", (int) len, p);
      mcpc_conn_t *outconn = mcpc_connpool_getconn (sv->connpool, 1);
      FILE *out_strm = outconn != nullptr ? mcpc_conn_getstrm (outconn) : nullptr;
      if (out_strm != nullptr)
	{
	  fwrite (p, 1, len, out_strm);
	  fflush (out_strm);
	}
    }
  else if (sv->typ == MCPC_SV_TCP)
    {
      size_t sent = 0;
      while (sent < len)
	{
#if is_unix
	  const ssize_t n = send (csock, p + sent, len - sent, MSG_NOSIGNAL);
#else
	  const int n = send (csock, p + sent, (int) (len - sent), 0);
#endif
	  if (n <= 0)
	    break;
	  sent += (size_t) n;
	}
    }
}

// Write the accumulated response in sv->rpcres to the peer and reset it.
static void
_flush_rpcres (mcpc_server_t *sv, mcpc_sock_t csock)
{
  if (sv->rpcres.len > 0)
    {
      mcpc_conn_t *c = sv->typ == MCPC_SV_HTTP ? mcpc_connpool_find_by_sock (sv->connpool, csock) : nullptr;
      if (c != nullptr && c->http != nullptr)
	mcpc_http_chan_final (c->http, sv->rpcres.ptr, (size_t) sv->rpcres.len);	// the response itself
      else
	_send_bytes (sv, csock, sv->rpcres.ptr, (size_t) sv->rpcres.len);
    }
  sv->rpcres.len = 0;
}

// HTTP status for the final response of the request being served, when it is not the default 200
static void
_http_status (mcpc_server_t *sv, mcpc_sock_t csock, int status)
{
  if (sv->typ != MCPC_SV_HTTP)
    return;
  mcpc_conn_t *c = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (c != nullptr && c->http != nullptr)
    c->http->status = status;
}

void
mcpc_srv_send (mcpc_server_t *sv, mcpc_sock_t csock, const char *p, size_t len)
{
  _send_bytes (sv, csock, p, len);
}

int
mcpc_srv_read_byte (mcpc_server_t *sv, mcpc_sock_t csock)
{
  if (sv->typ == MCPC_SV_IOSTRM)
    {
      mcpc_conn_t *inconn = mcpc_connpool_getconn (sv->connpool, 0);
      FILE *in_strm = inconn != nullptr ? mcpc_conn_getstrm (inconn) : nullptr;
      return in_strm != nullptr ? getc (in_strm) : -1;
    }
  if (sv->typ == MCPC_SV_TCP)
    {
      char ch;
      return recv (csock, &ch, 1, 0) == 1 ? (unsigned char) ch : -1;
    }
  return -1;			// HTTP carries one request per POST, there is nothing to read in the middle of one
}

#define MCPC_MAX_PENDING 64

bool
mcpc_srv_defer (mcpc_conn_t *conn, const char *frame, size_t len)
{
  if (conn->npend >= MCPC_MAX_PENDING)
    return false;
  char *cp = mcpc_alloc (len + 1);
  char **np = cp != nullptr ? mcpc_realloc (conn->pend, (conn->npend + 1) * sizeof (char *)) : nullptr;
  if (np == nullptr)
    {
      free (cp);
      return false;
    }
  memcpy (cp, frame, len);
  cp[len] = 0;
  conn->pend = np;
  conn->pend[conn->npend++] = cp;
  return true;
}

// Serve the frames that arrived while a callback was waiting for the client, oldest first.
static void
_serve_pending (mcpc_server_t *sv, mcpc_sock_t csock)
{
  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  while (conn != nullptr && conn->npend > 0)
    {
      char *frame = conn->pend[0];
      memmove (conn->pend, conn->pend + 1, (conn->npend - 1) * sizeof (char *));
      conn->npend--;
      handle_inone (sv, frame, strlen (frame), csock);
      free (frame);
    }
}

// An incoming message could not be framed: it was larger than the limit (and dropped), or a line ended in
// the middle of an object. Its id is unknown.
static void
_reply_frame_error (mcpc_server_t *sv, mcpc_sock_t csock, bool too_big)
{
  struct jsonrpc_request r;
  memset (&r, 0, sizeof (r));
  r.fn = mcpc_print_grow;
  r.fn_data = (void *) &sv->rpcres;
  if (too_big)
    jsonrpc_return_error (&r, JSONRPC_ERROR_INVALID_REQUEST, "message too large", NULL);
  else
    jsonrpc_return_error (&r, JSONRPC_ERROR_INVALID, "parse error", NULL);
  _flush_rpcres (sv, csock);
}

static void _close_listens (mcpc_server_t * sv, mcpc_sock_t csock);
static void _reply_frame_error (mcpc_server_t * sv, mcpc_sock_t csock, bool too_big);

// ----------------------------- iostrm -----------------------------

mcpc_errcode_t
_wait_iostrm (mcpc_server_t *sv)
{
  mcpc_conn_t *conn0 = mcpc_connpool_getconn (sv->connpool, 0);
  mcpc_assert (conn0->typ == MCPC_CONN_STRM, MCPC_EC_BUG);
  mcpc_conn_t *conn1 = mcpc_connpool_getconn (sv->connpool, 1);
  mcpc_assert (conn1->typ == MCPC_CONN_STRM, MCPC_EC_BUG);

  FILE *in_strm = mcpc_conn_getstrm (conn0);
  mcpc_assert (in_strm != nullptr, MCPC_EC_BUG);

  mcpc_framer_t framer;
  mcpc_framer_init_lines (&framer, MCPC_FRAME_DEFAULT_MAX);

  int ch;
  while ((ch = getc (in_strm)) != EOF)
    {
      const mcpc_frame_stat_t st = mcpc_framer_push (&framer, (unsigned char) ch);
      if (st == MCPC_FRAME_READY)
	{
	  mcpc_mutex_lock (&sv->mu);
	  handle_inone (sv, framer.buf, framer.len, (mcpc_sock_t) - 1);
	  _serve_pending (sv, (mcpc_sock_t) - 1);
	  mcpc_mutex_unlock (&sv->mu);
	  mcpc_framer_next (&framer);
	}
      else if (st == MCPC_FRAME_TOOBIG || st == MCPC_FRAME_BROKEN)
	{
	  mcpc_mutex_lock (&sv->mu);
	  _reply_frame_error (sv, (mcpc_sock_t) - 1, st == MCPC_FRAME_TOOBIG);
	  mcpc_mutex_unlock (&sv->mu);
	}
      else if (st == MCPC_FRAME_NOMEM)
	break;
    }

  mcpc_framer_free (&framer);

  // input ended: tell clients of open subscriptions/listen streams that they ended gracefully
  mcpc_mutex_lock (&sv->mu);
  _close_listens (sv, (mcpc_sock_t) - 1);
  mcpc_mutex_unlock (&sv->mu);

  return MCPC_EC_0;
}

mcpc_errcode_t
_svimpl_iostrm (mcpc_server_t *sv)
{
  mcpc_assert (sv->connpool->len == 2, MCPC_EC_BUG);
  jsonrpc_init (nullptr, nullptr);	// no need to handle responses

  return _wait_iostrm (sv);
}

mcpc_errcode_t
_start_iostrm (mcpc_server_t *sv)
{
  return _svimpl_iostrm (sv);
}

// ------------------------------ tcp ------------------------------

typedef struct
{
  mcpc_sock_t csock;
  struct sockaddr_in caddr;
  mcpc_server_t *sv;
} accept_hdlr_args_t;

mcpc_errcode_t
_new_tcp_listen (mcpc_sock_t *ssock)
{
  mcpc_sock_t tmpsock = (mcpc_sock_t) 0;
  struct sockaddr_in saddr;

  tmpsock = socket (AF_INET, SOCK_STREAM, 0);
#if is_unix
  if (tmpsock < 0)
#endif
#if is_win
    if (tmpsock == INVALID_SOCKET)
#endif
      {
	return MCPC_EC_SVIMPL_SOCKET;
      }

  int level = SOL_SOCKET;
  int optname = SO_REUSEADDR;
  int optval = 1;
  mcpc_assert (0 == setsockopt (tmpsock, level, optname, (const char *) &optval, sizeof (optval)), MCPC_EC_BUG);

  saddr.sin_family = AF_INET;
  saddr.sin_port = htons (PORT);
#if defined(is_unix)
  inet_pton (AF_INET, "127.0.0.1", &saddr.sin_addr);
#elif defined(is_win)
  saddr.sin_addr.s_addr = inet_addr ("127.0.0.1");
#endif

  if (bind (tmpsock, (struct sockaddr *) &saddr, sizeof (saddr)) == -1)
    {
      return MCPC_EC_SVIMPL_BIND;
    }

  if (listen (tmpsock, 16) == -1)
    {
      return MCPC_EC_SVIMPL_LISTEN;
    }

  *ssock = tmpsock;

  return MCPC_EC_0;
}

#ifdef is_unix
static void *
#endif
#ifdef is_win
  DWORD WINAPI
#endif
_wait_conn (void *args)
{
  accept_hdlr_args_t *thread_args_ptr = (accept_hdlr_args_t *) args;
  mcpc_sock_t csock = thread_args_ptr->csock;
#ifdef MCPC_DBG
  struct sockaddr_in caddr = thread_args_ptr->caddr;
#endif
  mcpc_server_t *sv = thread_args_ptr->sv;

  tulog_d ("New connection from client IP address %s and port %d", inet_ntoa (caddr.sin_addr), ntohs (caddr.sin_port));

  mcpc_mutex_lock (&sv->mu);
  mcpc_conn_t *myconn = mcpc_conn_new (MCPC_CONN_FD, &csock);
  const mcpc_errcode_t regec = mcpc_connpool_addref ((mcpc_connpool_t *) sv->connpool, myconn);
  mcpc_mutex_unlock (&sv->mu);
  if (regec != MCPC_EC_0)
    {
      // too many clients: refuse this one but keep serving the others
      tulog_d ("connection refused, pool full");
      mcpc_conn_free (myconn);
      goto conn_done;
    }

  mcpc_framer_t framer;
  mcpc_framer_init (&framer, MCPC_FRAME_DEFAULT_MAX);

  while (true)
    {
      char ch;
      const i32_t last_nrecv = (i32_t) recv (csock, &ch, 1, 0);
      if (last_nrecv == 0)
	break;
      if (last_nrecv < 0)
	{
	  tulog_d ("recv err %s", strerror (errno));
	  break;
	}

      const mcpc_frame_stat_t st = mcpc_framer_push (&framer, (unsigned char) ch);
      if (st == MCPC_FRAME_READY)
	{
	  mcpc_mutex_lock (&sv->mu);
	  handle_inone (sv, framer.buf, framer.len, csock);
	  _serve_pending (sv, csock);
	  mcpc_mutex_unlock (&sv->mu);
	  mcpc_framer_next (&framer);
	  tulog_d ("one msg handled");
	}
      else if (st == MCPC_FRAME_TOOBIG)
	{
	  mcpc_mutex_lock (&sv->mu);
	  _reply_frame_error (sv, csock, true);
	  mcpc_mutex_unlock (&sv->mu);
	}
      else if (st == MCPC_FRAME_NOMEM)
	break;
    }

  mcpc_framer_free (&framer);

  mcpc_mutex_lock (&sv->mu);
  mcpc_connpool_remove_by_sock ((mcpc_connpool_t *) sv->connpool, csock);
  mcpc_mutex_unlock (&sv->mu);

conn_done:
#ifdef is_unix
  close (csock);
#endif
#ifdef is_win
  closesocket (csock);
#endif

  free (thread_args_ptr);

#ifdef is_unix
  tulog_d ("thread %lu exiting...", pthread_self ());
#endif
#ifdef is_win
  tulog_d ("thread %lu exiting...", GetCurrentThreadId ());
#endif

#ifdef is_unix
  pthread_exit (nullptr);
#endif
#ifdef is_win
  ExitThread (0);
#endif
}

mcpc_errcode_t
_svimpl_tcp (mcpc_server_t *sv)
{

#ifdef is_win
  WSADATA wsaData;
  if (WSAStartup (MAKEWORD (2, 2), &wsaData) != 0)
    {
      return MCPC_EC_SVIMPL_SOCKET;
    }
#endif

  mcpc_sock_t ssock;
  struct sockaddr_in caddr;
  socklen_t caddr_length = sizeof (caddr);

  mcpc_assert (MCPC_EC_0 == _new_tcp_listen (&ssock), MCPC_EC_BUG);

  tulog_d ("Server listening on port %d...", PORT);

  mcpc_sock_t csock;
  while (true)
    {
      csock = accept (ssock, (struct sockaddr *) &caddr, &caddr_length);
#ifdef is_unix
      if (csock < 0)
#endif
#ifdef is_win
	if (csock == INVALID_SOCKET)
#endif
	  {
	    tulog_d ("accept failed");
	    continue;
	  }

      accept_hdlr_args_t *args = (accept_hdlr_args_t *) mcpc_alloc (sizeof (accept_hdlr_args_t));
      args->csock = csock;
      args->caddr = caddr;
      args->sv = sv;

#ifdef is_unix
      pthread_t thread;
      if (pthread_create (&thread, nullptr, _wait_conn, args) != 0)
	{
	  tulog_d ("thread creation failed");
	  free (args);
	  close (csock);
	}
      else
	pthread_detach (thread);
#endif
#ifdef is_win
      HANDLE thread;
      DWORD threadId;
      thread = CreateThread (NULL, 0, _wait_conn, args, 0, &threadId);
      if (thread == NULL)
	{
	  tulog_d ("thread creation failed");
	  free (args);
	  closesocket (csock);
	}
#endif
    }

#ifdef is_win
  WSACleanup ();
#endif

  return 0;
}

mcpc_errcode_t
_start_tcp (mcpc_server_t *sv)
{
  return _svimpl_tcp (sv);
}

// ------------------------------- - -------------------------------

MCPC_API mcpc_errcode_t
mcpc_server_start (mcpc_server_t *sv)
{
  if (_server_is_buggy (sv))
    {
      return MCPC_EC_BUG;
    }
  if (false)
    ;
  else if (sv->typ == MCPC_SV_IOSTRM)
    {
      return _start_iostrm (sv);
    }
  else if (sv->typ == MCPC_SV_TCP)
    {
      return _start_tcp (sv);
    }
  else if (sv->typ == MCPC_SV_HTTP)
    {
      return mcpc_http_run (sv, nullptr);
    }
  else
    {
      return MCPC_EC_BUG;
    }
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_close (mcpc_server_t *sv)
{
  // return MCPC_EC_0; // NOTE: this does not trigger asan but valgrind
  if (_server_is_buggy (sv))
    {
      return MCPC_EC_BUG;
    }

  if (sv->name != nullptr)
    {
      free ((void *) sv->name);
      sv->name = nullptr;
      sv->name_len = 0;
    }

  if (sv->ver != nullptr)
    {
      free ((void *) sv->ver);
      sv->ver = nullptr;
      sv->ver_len = 0;
    }

  free (sv->title);
  free (sv->desc);
  free (sv->website);
  free (sv->icons_json);
  free (sv->instructions);
  free (sv->extensions_json);
  if (sv->state_key != nullptr)
    memset (sv->state_key, 0, sv->state_key_len);
  free (sv->state_key);
  mcpc_print_grow_free (&sv->rpcres);
  mcpc_mutex_destroy (&sv->mu);
  mcpc_mutex_destroy (&sv->cmu);
  for (size_t i = 0; i < sv->nsessions; i++)
    free (sv->sessions[i].ccaps);
  free (sv->sessions);
  while (sv->pending != nullptr)
    {
      struct mcpc_pending *p = sv->pending;
      sv->pending = p->next;
      free (p->json);
      free (p);
    }
  mcpc_cond_destroy (&sv->pcv);
  mcpc_toolpool_free ((mcpc_toolpool_t *) sv->toolpool);
  mcpc_prmptpool_free ((mcpc_prmptpool_t *) sv->prmptpool);
  mcpc_rscpool_free ((mcpc_rscpool_t *) sv->rscpool);
  mcpc_anypool_free ((mcpc_anypool_t *) sv->rsctplpool);
  mcpc_connpool_free ((mcpc_connpool_t *) sv->connpool);

  free (sv);

  return MCPC_EC_0;
}

mcpc_errcode_t
mcpc_server_cleanup_rpcres (mcpc_server_t *sv)
{
  sv->rpcres.len = 0;
  return MCPC_EC_0;
}

bool
_connpool_is_addable (mcpc_connpool_t *connpool)
{
  if (connpool->len < connpool->cap)
    {
      return true;
    }
  return false;
}

mcpc_conn_t *
mcpc_conn_new (mcpc_conn_type_t typ, void *back)
{
  mcpc_conn_t *conn = mcpc_alloc (sizeof (mcpc_conn_t));
  conn->typ = typ;

  if (false)
    ;
  else if (typ == MCPC_CONN_STRM)
    {
      FILE *strm = (FILE *) back;
      conn->strm = strm;
    }
  else if (typ == MCPC_CONN_FD)
    {
      int fd = *((int *) back);
      conn->fd = fd;
    }
  else
    {
      conn->strm = nullptr;
    }
  conn->cname = nullptr;
  conn->cname_len = 0;
  conn->client_init = MCPC_INIT_NONE;
  conn->pver = MCPC_PV_NONE;
  conn->ccaps = nullptr;
  conn->ccaps_len = 0;
  conn->log_level = MCPC_LOG_INFO;
  conn->overlay = false;
  conn->base_init = MCPC_INIT_NONE;
  conn->subs = nullptr;
  conn->nsubs = 0;
  conn->pend = nullptr;
  conn->npend = 0;
  conn->listens = nullptr;
  conn->nlistens = 0;
  conn->next_req = 1;
  conn->nex = nullptr;

  return conn;
}

FILE *
mcpc_conn_getstrm (mcpc_conn_t *conn)
{
  if (conn->typ != MCPC_CONN_STRM)
    {
      return nullptr;
    }
  return conn->strm;
}

int
mcpc_conn_getfd (mcpc_conn_t *conn)
{
  if (conn->typ != MCPC_CONN_FD)
    {
      return 0;
    }
  return conn->fd & ((1ULL << (sizeof (int) * 8)) - 1);
}

mcpc_errcode_t
mcpc_connpool_addref (mcpc_connpool_t *connpool, mcpc_conn_t *conn)
{
  if (_connpool_is_buggy ((const mcpc_connpool_t *) connpool))
    {
      return MCPC_EC_BUG;
    }

  if (!_connpool_is_addable (connpool))
    {
      return MCPC_EC_CONNPOOL_NOTADD;
    }

  if (connpool->head == nullptr)
    {
      connpool->head = conn;
      connpool->len = 1;
    }
  else
    {
      mcpc_conn_t *cur_conn = connpool->head;
      while (cur_conn->nex != nullptr)
	{
	  cur_conn = cur_conn->nex;
	}
      cur_conn->nex = conn;
      connpool->len += 1;
      // mcpc_assert(!ckd_add(&connpool->len, connpool->len, 1), MCPC_EC_BUG);
    }

  return MCPC_EC_0;
}

mcpc_conn_t *
mcpc_connpool_getconn (const mcpc_connpool_t *connpool, const mcpc_connpool_size_t idx)
{
  mcpc_assert (!_connpool_is_buggy (connpool), MCPC_EC_BUG);
  mcpc_connpool_size_t cur_idx = 0;
  mcpc_conn_t *cur_conn = connpool->head;
  while (cur_conn != nullptr && cur_idx < idx)
    {
      cur_conn = cur_conn->nex;
      cur_idx += 1;
      // mcpc_assert(!ckd_add(&cur_idx, cur_idx, 1), MCPC_EC_BUG);
    }
  return cur_conn;
}

mcpc_conn_t *
mcpc_connpool_find_by_sock (const mcpc_connpool_t *connpool, mcpc_sock_t sock)
{
  mcpc_assert (!_connpool_is_buggy (connpool), MCPC_EC_BUG);

  // iostrm
  if (mcpc_sock_invalid (sock))
    {			
      return connpool->head;
    }

  // tcp
  mcpc_conn_t *cur_conn = connpool->head;
  while (cur_conn != nullptr)
    {
      mcpc_sock_t cur_sock = cur_conn->fd;
      if (cur_sock == sock)
	{
	  return cur_conn;
	}
      cur_conn = cur_conn->nex;
    }

  return nullptr;
}

// Unlink and free the connection owning sock. Other nodes stay intact.
mcpc_errcode_t
mcpc_connpool_remove_by_sock (mcpc_connpool_t *connpool, mcpc_sock_t sock)
{
  mcpc_conn_t **link = &connpool->head;
  while (*link != nullptr)
    {
      mcpc_conn_t *cur = *link;
      if (cur->typ == MCPC_CONN_FD && cur->fd == sock)
	{
	  *link = cur->nex;
	  cur->nex = nullptr;	// mcpc_conn_free frees the whole chain
	  mcpc_conn_free (cur);
	  connpool->len -= 1;
	  return MCPC_EC_0;
	}
      link = &cur->nex;
    }
  return MCPC_EC_BUG;
}

mcpc_errcode_t
mcpc_conn_set_cname (mcpc_conn_t *conn, const ch8_t *const cname, const u8_t cname_len)
{
  void *hp = mcpc_alloc (cname_len);
  memcpy (hp, cname, cname_len);
  conn->cname = hp;
  conn->cname_len = cname_len;
  return MCPC_EC_0;
}

void
_handle_initbeg (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{

  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      goto output_res;
    }
  if (conn->client_init == MCPC_INIT_SUCC)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST, "conn already initialized", NULL);
      goto output_res;
    }

  // - - -

  const char8_t *protover = nullptr;
  size_t protover_len = 0;
  if (MCPC_EC_0 != _tulc_jparams_getref_field_u8str_q (r, u8c1 ("$.protocolVersion"), &protover, &protover_len))
    return;

  // - - - client info

  char8_t *cinfo_name = nullptr;
  size_t cinfo_name_len = 0;
  char8_t *cinfo_ver = nullptr;
  size_t cinfo_ver_len = 0;

  cinfo_name = alloca (128);
  if (MCPC_EC_0 !=
      _tulc_jparams_get_field_u8str (r, u8c1 ("$.clientInfo.name"), &cinfo_name, &cinfo_name_len, 128, true))
    return;			// TODO P5 add test

  cinfo_ver = alloca (128);
  if (MCPC_EC_0 !=
      _tulc_jparams_get_field_u8str (r, u8c1 ("$.clientInfo.version"), &cinfo_ver, &cinfo_ver_len, 128, true))
    return;			// TODO P5 add test

  mcpc_conn_set_cname (conn, cinfo_name, (u8_t) cinfo_name_len);

  // - - - capabilities

  const char8_t *ccapas = nullptr;
  size_t ccapas_len = 0;
  if (MCPC_EC_0 != _tulc_jparams_getref_field_obj (r, u8c1 ("$.capabilities"), &ccapas, &ccapas_len))
    return;			// TODO P5 add test

  // keep a private copy, the request buffer is reused for the next message
  free (conn->ccaps);
  conn->ccaps = mcpc_alloc (ccapas_len + 1);
  if (conn->ccaps != nullptr)
    {
      memcpy (conn->ccaps, ccapas, ccapas_len);
      conn->ccaps[ccapas_len] = 0;
      conn->ccaps_len = ccapas_len;
    }

  // answer with the client's revision when we speak it, else with our newest
  // protover is the quoted JSON token
  mcpc_pver_t pver =
    protover_len >= 2 ? mcpc_pver_negotiate ((const char *) protover + 1, protover_len - 2) : MCPC_PV_LATEST_LEGACY;
  // the client's revision if the server speaks it, else the newest legacy revision it does speak
  for (int v = (int) pver; v >= (int) MCPC_PV_2024_11_05 && !(sv->pmask & mcpc_pver_bit ((mcpc_pver_t) v)); v--)
    pver = (mcpc_pver_t) (v - 1);
  if (pver == MCPC_PV_NONE)
    for (int v = MCPC_PV_LATEST_LEGACY; v >= MCPC_PV_2024_11_05; v--)
      if (sv->pmask & mcpc_pver_bit ((mcpc_pver_t) v))
	{
	  pver = (mcpc_pver_t) v;
	  break;
	}
  conn->pver = pver;

  retbuf_t rtbf = _rb_new (sv, conn, 1024);
  serlz_initres (rtbf, sv, pver);
  _reply_retbuf (sv, r, csock, rtbf);

  conn->client_init = MCPC_INIT_BEG;

output_res:
  return;
}

// Reads params.cursor of a list request. Cursors are opaque to clients; ours is the
// decimal index of the first item of the page. Replies -32602 and returns false on a bad one.
static bool
_page_params (const mcpc_server_t *sv, struct jsonrpc_request *r, size_t total, size_t *offset, size_t *limit)
{
  *offset = 0;
  *limit = sv->page_size;

  const char *cur = nullptr;
  int cur_len = 0;
  const int tok = (r->params != nullptr && r->params_len > 0) ? mjson_find (r->params, r->params_len, "$.cursor", &cur, &cur_len) :
    MJSON_TOK_INVALID;
  if (tok == MJSON_TOK_INVALID || tok == MJSON_TOK_NULL)
    return true;

  size_t val = 0;
  bool ok = (tok == MJSON_TOK_STRING && cur_len >= 3 && cur_len <= 22);	// quotes + 1..20 digits
  for (int i = 1; ok && i < cur_len - 1; i++)
    {
      if (cur[i] < '0' || cur[i] > '9')
	ok = false;
      else
	val = val * 10 + (size_t) (cur[i] - '0');
    }
  if (!ok || val > total)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "invalid cursor", NULL);
      return false;
    }

  *offset = val;
  return true;
}

// ping may arrive at any time, even before initialization
void
_handle_ping (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{
  jsonrpc_return_success2 (r, mcpc_sock_invalid (csock), "{}");
}

static int
_loglevel_parse (const char *s, int len)
{
  static const char *const names[] = { "debug", "info", "notice", "warning", "error", "critical", "alert", "emergency" };
  for (int i = 0; i < 8; i++)
    if ((int) strlen (names[i]) == len && memcmp (names[i], s, (size_t) len) == 0)
      return i;
  return -1;
}

void
_handle_log_setlevel (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{
  if (!mcpc_server_supp_log (sv))
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_NOT_FOUND, "logging is not supported", NULL);
      return;
    }

  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      return;
    }
  if (conn->client_init != MCPC_INIT_SUCC)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST, "client not initialized", NULL);
      return;
    }

  const char8_t *lvl_q = nullptr;
  size_t lvl_q_len = 0;
  if (MCPC_EC_0 != _tulc_jparams_getref_field_u8str_q (r, u8c1 ("$.level"), &lvl_q, &lvl_q_len))
    return;

  const int lvl = lvl_q_len >= 2 ? _loglevel_parse ((const char *) lvl_q + 1, (int) lvl_q_len - 2) : -1;
  if (lvl < 0)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "unknown log level", NULL);
      return;
    }

  conn->log_level = lvl;
  jsonrpc_return_success2 (r, mcpc_sock_invalid (csock), "{}");
}

void
_handle_initdone (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock, bool *noreply)
{

  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      goto output_res;
    }
  if (conn->client_init != MCPC_INIT_BEG)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST, "client not begin init", NULL);
      goto output_res;
    }

  conn->client_init = MCPC_INIT_SUCC;
  *noreply = true;

output_res:
  return;
}

void
_handle_tools_list (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{

  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      goto output_res;
    }
  if (conn->client_init != MCPC_INIT_SUCC)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST, "client not initialized", NULL);
      goto output_res;
    }

  retbuf_t rtbf = _rb_new (sv, conn, 1024);
  size_t off = 0, lim = 0;
  if (!_page_params (sv, r, mcpc_toolpool_get_len (sv->toolpool), &off, &lim))
    {
      retbuf_free (rtbf);
      return;
    }
  serlz_toolpool (rtbf, sv->toolpool, off, lim);
  _reply_retbuf (sv, r, csock, rtbf);

output_res:
  return;
}

void
_handle_prmpt_list (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{
  if (!mcpc_server_supp_prompts (sv))
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_NOT_FOUND, "prompts is not supported", NULL);
      goto output_res;
    }

  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      goto output_res;
    }
  if (conn->client_init != MCPC_INIT_SUCC)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST, "client not initialized", NULL);
      goto output_res;
    }

  retbuf_t rtbf = _rb_new (sv, conn, 1024);
  size_t off = 0, lim = 0;
  if (!_page_params (sv, r, mcpc_prmptpool_get_len (sv->prmptpool), &off, &lim))
    {
      retbuf_free (rtbf);
      return;
    }
  serlz_prmptpool (rtbf, sv->prmptpool, off, lim);
  _reply_retbuf (sv, r, csock, rtbf);

output_res:
  return;
}

void
_handle_lsrsc (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{

  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      goto output_res;
    }
  if (conn->client_init != MCPC_INIT_SUCC)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST, "client not initialized", NULL);
      goto output_res;
    }

  retbuf_t rtbf = _rb_new (sv, conn, 1024);
  size_t off = 0, lim = 0;
  if (!_page_params (sv, r, mcpc_rscpool_get_len (sv->rscpool), &off, &lim))
    {
      retbuf_free (rtbf);
      return;
    }
  serlz_rscpool (rtbf, sv->rscpool, off, lim);
  _reply_retbuf (sv, r, csock, rtbf);

output_res:
  return;
}

// Pick up what a retried multi round-trip request carries: the answers (params.inputResponses) and the
// state (params.requestState, verified, it comes from the client). Replies -32602 and returns false for a
// state that does not verify.
static bool
_load_retry (mcpc_server_t *sv, struct jsonrpc_request *r, struct mcpc_ctx *ctx)
{
  const char *tok = nullptr;
  int tl = 0;
  if (mjson_find (r->params, r->params_len, "$.inputResponses", &tok, &tl) == MJSON_TOK_OBJECT)
    {
      ctx->in_resp = tok;
      ctx->in_resp_len = tl;
    }

  if (mjson_find (r->params, r->params_len, "$.requestState", &tok, &tl) == MJSON_TOK_STRING && tl >= 2)
    {
      char *plain = mcpc_alloc ((size_t) tl + 1);
      const int got = plain != nullptr ? mjson_get_string (tok, tl, "$", plain, tl + 1) : -1;
      ctx->state_in = got >= 0 ? mcpc_state_open (sv, ctx->method, ctx->target, plain, (size_t) got) : nullptr;
      free (plain);
      if (ctx->state_in == nullptr)
	{
	  jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "invalid requestState", NULL);
	  return false;
	}
    }
  return true;
}

// Fill in the context of a request that may call back into the client. Returns false after replying
// with an error when params.requestState does not verify.
static bool
_ctx_begin (mcpc_server_t *sv, mcpc_conn_t *conn, struct jsonrpc_request *r, mcpc_sock_t csock, struct mcpc_ctx *ctx,
	    const char *method, const char *target, const char *args, int args_len)
{
  memset (ctx, 0, sizeof (*ctx));
  ctx->sv = sv;
  ctx->conn = conn;
  ctx->csock = csock;
  ctx->feat = mcpc_pver_feats (conn->pver);
  ctx->args = args;
  ctx->args_len = args_len;
  ctx->reqid = r->id;
  ctx->reqid_len = r->id_len;
  ctx->modern = mcpc_pver_is_modern (conn->pver);
  ctx->method = method;
  snprintf (ctx->target, sizeof (ctx->target), "%s", target);

  // the client asks for progress by putting a token in params._meta
  const char *pt = nullptr;
  int pt_len = 0;
  const int ptt = mjson_find (r->params, r->params_len, "$._meta.progressToken", &pt, &pt_len);
  if ((ptt == MJSON_TOK_NUMBER || ptt == MJSON_TOK_STRING) && mcpc_json_token_clean (ptt, pt, (size_t) pt_len))
    {
      ctx->token = pt;
      ctx->token_len = pt_len;
    }

  return !ctx->modern || _load_retry (sv, r, ctx);
}

// If the callback asked for input, answer with an InputRequiredResult instead and say so.
static bool
_ctx_input_required (mcpc_server_t *sv, mcpc_conn_t *conn, struct jsonrpc_request *r, mcpc_sock_t csock, struct mcpc_ctx *ctx)
{
  if (!ctx->modern || (ctx->n_inreqs == 0 && ctx->state_out == nullptr))
    return false;
  retbuf_t rtbf = _rb_new (sv, conn, 512);
  serlz_input_required (rtbf, ctx);
  _reply_result (sv, r, csock, rtbf, "input_required");
  return true;
}

// Fill the tool's property values from the JSON arguments object of one call.
// Returns NULL on success, otherwise a message (in err) describing what the caller got wrong.
static const char *
_bind_tool_args (const mcpc_tool_t *tool, const char *args, int args_len, char *err, size_t err_cap)
{
  const mcpc_toolproppool_t *pool = mcpc_tool_get_toolproppool (tool);
  const size_t n = pool != nullptr ? mcpc_toolproppool_get_len (pool) : 0;

  for (size_t i = 0; i < n; i++)
    {
      mcpc_toolprop_t *tp = (mcpc_toolprop_t *) mcpc_toolproppool_get (pool, i);
      tp->flags &= ~ANYDATA_FL_PROVIDED;
    }

  for (size_t i = 0; i < n; i++)
    {
      mcpc_toolprop_t *tp = (mcpc_toolprop_t *) mcpc_toolproppool_get (pool, i);
      const mcpc_anytype_t typ = tp->data_typ;
      const int name_len = tp->meta1_len;

      // $.name addresses the member of the arguments object
      char path[2 + 255 + 1];
      snprintf (path, sizeof (path), "$.%.*s", name_len, (const char *) tp->meta1);

      const char *tok = nullptr;
      int tok_len = 0;
      const int tt = mjson_find (args, args_len, path, &tok, &tok_len);

      if (tt == MJSON_TOK_INVALID || tt == MJSON_TOK_NULL)
	{
	  if (tp->flags & ANYDATA_FL_REQUIRED)
	    {
	      snprintf (err, err_cap, "missing required argument \"%.*s\"", name_len, (const char *) tp->meta1);
	      return err;
	    }
	  continue;
	}

      const char *want = nullptr;
      mcpc_errcode_t ec = MCPC_EC_0;

      if (typ == MCPC_BOOL)
	{
	  if (tt == MJSON_TOK_TRUE || tt == MJSON_TOK_FALSE)
	    ec = mcpc_anydata_set_bool (tp, tt == MJSON_TOK_TRUE);
	  else
	    want = "a boolean";
	}
      else if (typ == MCPC_F64 || (mcpc_anytype_is_intlike (typ) && typ != MCPC_FUNC))
	{
	  if (tt != MJSON_TOK_NUMBER)
	    want = "a number";
	  else
	    {
	      char num[64];
	      if (tok_len >= (int) sizeof (num))
		want = "a number";
	      else
		{
		  memcpy (num, tok, (size_t) tok_len);
		  num[tok_len] = 0;
		  const double d = strtod (num, nullptr);
		  if (typ == MCPC_F64)
		    ec = mcpc_anydata_set_f64 (tp, d);
		  else if (typ == MCPC_I8)
		    ec = mcpc_toolprop_set_i8 (tp, (i8_t) d);
		  else if (typ == MCPC_U8)
		    ec = mcpc_toolprop_set_u8 (tp, (u8_t) d);
		  else if (typ == MCPC_I16)
		    ec = mcpc_toolprop_set_i16 (tp, (i16_t) d);
		  else if (typ == MCPC_U16)
		    ec = mcpc_toolprop_set_u16 (tp, (u16_t) d);
		  else if (typ == MCPC_I32)
		    ec = mcpc_toolprop_set_i32 (tp, (i32_t) d);
		  else if (typ == MCPC_U32)
		    ec = mcpc_toolprop_set_u32 (tp, (u32_t) d);
		  else if (typ == MCPC_I64)
		    ec = mcpc_toolprop_set_i64 (tp, (i64_t) d);
		  else if (typ == MCPC_U64)
		    ec = mcpc_toolprop_set_u64 (tp, (u64_t) d);
		}
	    }
	}
      else if (typ == MCPC_U8STR)
	{
	  if (tt != MJSON_TOK_STRING)
	    want = "a string";
	  else
	    {
	      // the unescaped text is never longer than its quoted token
	      char *buf = mcpc_alloc ((size_t) tok_len + 1);
	      if (buf == nullptr)
		return "out of memory";
	      const int got = mjson_get_string (tok, tok_len, "$", buf, tok_len + 1);
	      if (got < 0)
		want = "a string";
	      else
		ec = mcpc_toolprop_set_u8str (tp, (const char8_t *) buf, (size_t) got);
	      free (buf);
	    }
	}
      else if (typ == MCPC_JSON)
	ec = mcpc_anydata_set_json (tp, (const char8_t *) tok, (size_t) tok_len);
      else
	continue;			// no argument binding for this type

      if (want != nullptr)
	{
	  snprintf (err, err_cap, "argument \"%.*s\" must be %s", name_len, (const char *) tp->meta1, want);
	  return err;
	}
      if (ec != MCPC_EC_0)
	{
	  snprintf (err, err_cap, "argument \"%.*s\" is invalid", name_len, (const char *) tp->meta1);
	  return err;
	}
      tp->flags |= ANYDATA_FL_PROVIDED;
    }

  return nullptr;
}

void
_handle_tools_call (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{
  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      goto output_res;
    }
  if (conn->client_init != MCPC_INIT_SUCC)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST, "client not initialized", NULL);
      goto output_res;
    }

  // tool names are at most 255 bytes, anything longer cannot name a tool
  char8_t callname[256 + 1];
  size_t callname_len = 0;
  {
    const char *nm = nullptr;
    int nm_len = 0;
    if (mjson_find (r->params, r->params_len, "$.name", &nm, &nm_len) != MJSON_TOK_STRING || nm_len < 3 ||
	nm_len > 2 + 4 * 255)
      {
	jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "tool not found", NULL);
	goto output_res;
      }
    char8_t *tmp = mcpc_alloc ((size_t) nm_len + 1);
    const int got = tmp != nullptr ? mjson_get_string (nm, nm_len, "$", (char *) tmp, nm_len + 1) : -1;
    if (got < 0 || got > 255)
      {
	free (tmp);
	jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "tool not found", NULL);
	goto output_res;
      }
    memcpy (callname, tmp, (size_t) got);
    callname[got] = 0;
    callname_len = (size_t) got;
    free (tmp);
  }

  const mcpc_tool_t *tool = (const mcpc_tool_t *) mcpc_toolpool_getbyname (sv->toolpool, callname, callname_len);
  if (tool == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "tool not found", NULL);
      goto output_res;
    }

  // arguments may be left out when the tool takes none
  const char *callargs = "{}";
  int callargs_len = 2;
  {
    const char *a = nullptr;
    int a_len = 0;
    const int at = mjson_find (r->params, r->params_len, "$.arguments", &a, &a_len);
    if (at == MJSON_TOK_OBJECT)
      {
	callargs = a;
	callargs_len = a_len;
      }
    else if (at != MJSON_TOK_INVALID && at != MJSON_TOK_NULL)
      {
	jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "arguments must be an object", NULL);
	goto output_res;
      }
  }

  // the request's own context. On 2026-07-28 the callback may be run again with answers and a state.
  struct mcpc_ctx ctx;
  if (!_ctx_begin (sv, conn, r, csock, &ctx, "tools/call", (const char *) callname, callargs, callargs_len))
    return;

  mcpc_ucbr_t *ucbr = mcpc_ucbr_new ();

  // wrong arguments are the caller's mistake to learn from, so they come back as a tool
  // error result the model can read, not as a protocol error
  char bind_err[300];
  if (_bind_tool_args (tool, callargs, callargs_len, bind_err, sizeof (bind_err)) != nullptr)
    {
      mcpc_ucbr_toolcall_add_errmsg_printf8 (ucbr, u8c1 ("%s"), bind_err);
    }
  else
    {
      mcpc_tcallcb2_t callcb2 = tool->extra != nullptr ? (mcpc_tcallcb2_t) tool->extra->cb : nullptr;
      mcpc_tcallcb_t callcb = nullptr;
      mcpc_tool_get_call_cb (tool, &callcb);
      if (callcb2 != nullptr)
	callcb2 (tool, &ctx, ucbr);
      else if (callcb != nullptr)
	callcb (tool, ucbr);
      else
	mcpc_ucbr_toolcall_add_errmsg_printf8 (ucbr, u8c1 ("tool has no implementation"));
    }

  if (!_ctx_input_required (sv, conn, r, csock, &ctx))
    {
      retbuf_t rtbf = _rb_new (sv, conn, mcpc_ucbr_get_serlz_lenhint (ucbr));
      serlz_tool_callres (rtbf, ucbr);
      _reply_retbuf (sv, r, csock, rtbf);
    }

  mcpc_ctx_release (&ctx);
  mcpc_ucbr_free (ucbr);

output_res:
  return;
}

// Common gate of the resources/* methods: returns the initialized connection or replies with the reason why not.
static mcpc_conn_t *
_rsc_gate (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock, bool need_subscribe)
{
  if (!mcpc_server_supp_rscs (sv) || (need_subscribe && !mcpc_server_supp_rscs_subs (sv)))
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_NOT_FOUND, need_subscribe ? "resource subscriptions are not supported"
			    : "resources are not supported", NULL);
      return nullptr;
    }

  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      return nullptr;
    }
  if (conn->client_init != MCPC_INIT_SUCC)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST, "client not initialized", NULL);
      return nullptr;
    }
  return conn;
}

// params.uri as an unescaped, NUL-terminated heap string, or NULL after replying with -32602
static char *
_param_uri (struct jsonrpc_request *r, size_t *len)
{
  const char *tok = nullptr;
  int tok_len = 0;
  if (r->params == nullptr || mjson_find (r->params, r->params_len, "$.uri", &tok, &tok_len) != MJSON_TOK_STRING ||
      tok_len < 3)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "uri is required", NULL);
      return nullptr;
    }
  char *uri = mcpc_alloc ((size_t) tok_len + 1);
  const int got = uri != nullptr ? mjson_get_string (tok, tok_len, "$", uri, tok_len + 1) : -1;
  if (got <= 0)
    {
      free (uri);
      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "uri is invalid", NULL);
      return nullptr;
    }
  uri[got] = 0;
  *len = (size_t) got;
  return uri;
}

void
_handle_rsc_tmpl_list (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{
  mcpc_conn_t *conn = _rsc_gate (sv, r, csock, false);
  if (conn == nullptr)
    return;

  size_t off = 0, lim = 0;
  if (!_page_params (sv, r, sv->rsctplpool->len, &off, &lim))
    return;

  retbuf_t rtbf = _rb_new (sv, conn, 1024);
  serlz_rsctplpool (rtbf, sv->rsctplpool, off, lim);
  _reply_retbuf (sv, r, csock, rtbf);
}

void
_handle_rsc_read (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{
  mcpc_conn_t *conn = _rsc_gate (sv, r, csock, false);
  if (conn == nullptr)
    return;

  size_t uri_len = 0;
  char *uri = _param_uri (r, &uri_len);
  if (uri == nullptr)
    return;

  // a concrete resource wins over a template that would also match
  const mcpc_anydata_t *src = mcpc_rscpool_getbyuri (sv->rscpool, (const char8_t *) uri, uri_len);
  if (src == nullptr)
    src = mcpc_rsctplpool_find (sv->rsctplpool, (const char8_t *) uri, uri_len);
  if (src == nullptr)
    {
      // -32002 is the "resource not found" code of the 2024-2025 revisions, 2026-07-28 uses invalid params
      jsonrpc_return_error (r, mcpc_pver_is_modern (conn->pver) ? JSONRPC_ERROR_BAD_PARAMS : -32002, "Resource not found",
			    "{%Q:%.*Q}", "uri", (int) uri_len, uri);
      free (uri);
      return;
    }

  mcpc_rcallcb2_t cb2 = src->extra != nullptr ? (mcpc_rcallcb2_t) src->extra->cb2 : nullptr;
  mcpc_rcallcb_t cb = src->extra != nullptr ? (mcpc_rcallcb_t) src->extra->cb : nullptr;
  if (cb == nullptr && cb2 == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "resource has no reader", NULL);
      free (uri);
      return;
    }

  // the state of a retry is bound to the uri; longer ones are cut, which only makes the binding coarser
  struct mcpc_ctx ctx;
  if (!_ctx_begin (sv, conn, r, csock, &ctx, "resources/read", uri, "{}", 2))
    {
      free (uri);
      return;
    }

  mcpc_ucbr_t *ucbr = mcpc_ucbr_new ();
  if (cb2 != nullptr)
    cb2 (src, &ctx, (const char8_t *) uri, uri_len, ucbr);
  else
    cb (src, (const char8_t *) uri, uri_len, ucbr);

  if (!_ctx_input_required (sv, conn, r, csock, &ctx))
    {
      retbuf_t rtbf = _rb_new (sv, conn, mcpc_ucbr_get_serlz_lenhint (ucbr));
      serlz_rscread_res (rtbf, ucbr, uri, (src->flags & ANYDATA_FL_RSCTPL) ? mcpc_anydata_xget (src, MCPC_X_MIME) :
			 mcpc_rsc_mime_cstr (src));
      _reply_retbuf (sv, r, csock, rtbf);
    }

  mcpc_ctx_release (&ctx);
  mcpc_ucbr_free (ucbr);
  free (uri);
}

#define MCPC_MAX_SUBS 1024

void
_handle_rsc_subscribe (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock, bool subscribe)
{
  mcpc_conn_t *conn = _rsc_gate (sv, r, csock, true);
  if (conn == nullptr)
    return;

  size_t uri_len = 0;
  char *uri = _param_uri (r, &uri_len);
  if (uri == nullptr)
    return;

  size_t at = conn->nsubs;
  for (size_t i = 0; i < conn->nsubs; i++)
    if (strcmp (conn->subs[i], uri) == 0)
      at = i;

  if (subscribe && at == conn->nsubs)
    {
      if (conn->nsubs >= MCPC_MAX_SUBS)
	{
	  free (uri);
	  jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "too many subscriptions", NULL);
	  return;
	}
      char **ns = mcpc_realloc (conn->subs, (conn->nsubs + 1) * sizeof (char *));
      if (ns == nullptr)
	{
	  free (uri);
	  jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "out of memory", NULL);
	  return;
	}
      conn->subs = ns;
      conn->subs[conn->nsubs++] = uri;
    }
  else
    {
      if (!subscribe && at < conn->nsubs)
	{
	  free (conn->subs[at]);
	  conn->subs[at] = conn->subs[--conn->nsubs];
	}
      free (uri);
    }

  jsonrpc_return_success2 (r, mcpc_sock_invalid (csock), "{}");
}

// ----------------------------- notifications -----------------------------

// does an open listen stream want this notification?
static bool
_listen_wants (const struct mcpc_listen *l, const char *method, const char *uri)
{
  if (uri != nullptr)
    {
      for (size_t i = 0; i < l->nuris; i++)
	if (strcmp (l->uris[i], uri) == 0)
	  return true;
      return false;
    }
  if (strcmp (method, "notifications/tools/list_changed") == 0)
    return l->flags & MCPC_LISTEN_TOOLS;
  if (strcmp (method, "notifications/prompts/list_changed") == 0)
    return l->flags & MCPC_LISTEN_PROMPTS;
  if (strcmp (method, "notifications/resources/list_changed") == 0)
    return l->flags & MCPC_LISTEN_RSCS;
  return false;
}

// Send one notification to every initialized client (to those subscribed to uri, when uri is given),
// and on the 2026-07-28 revision to every subscriptions/listen stream that asked for it.
static void
_notify_clients (mcpc_server_t *sv, const char *method, const char *uri)
{
  char *msg = nullptr;
  if (uri != nullptr)
    msg = mjson_aprintf ("{\"jsonrpc\":\"2.0\",\"method\":%Q,\"params\":{\"uri\":%Q}}\n", method, uri);
  else
    msg = mjson_aprintf ("{\"jsonrpc\":\"2.0\",\"method\":%Q}\n", method);
  if (msg == nullptr)
    return;
  const size_t len = strlen (msg);

  mcpc_mutex_lock (&sv->mu);
  for (mcpc_conn_t *c = sv->connpool->head; c != nullptr; c = c->nex)
    {
      // the stream transport keeps the client state in its first connection, the second is only the output
      if (sv->typ == MCPC_SV_IOSTRM ? c != sv->connpool->head : c->typ != MCPC_CONN_FD)
	continue;
      const mcpc_sock_t sock = c->typ == MCPC_CONN_FD ? c->fd : (mcpc_sock_t) - 1;

      // the handshake state, not the view a stateless request in flight may have laid over it
      if ((c->overlay ? c->base_init : (int) c->client_init) == MCPC_INIT_SUCC)
	{
	  bool subscribed = uri == nullptr;
	  for (size_t i = 0; i < c->nsubs && !subscribed; i++)
	    subscribed = strcmp (c->subs[i], uri) == 0;
	  if (subscribed)
	    _send_bytes (sv, sock, msg, len);
	}

      for (size_t i = 0; i < c->nlistens; i++)
	{
	  const struct mcpc_listen *l = &c->listens[i];
	  if (!_listen_wants (l, method, uri))
	    continue;
	  char *lm;
	  if (uri != nullptr)
	    lm = mjson_aprintf ("{\"jsonrpc\":\"2.0\",\"method\":%Q,\"params\":{\"_meta\":{"
				"\"io.modelcontextprotocol/subscriptionId\":%s},\"uri\":%Q}}\n", method, l->id, uri);
	  else
	    lm = mjson_aprintf ("{\"jsonrpc\":\"2.0\",\"method\":%Q,\"params\":{\"_meta\":{"
				"\"io.modelcontextprotocol/subscriptionId\":%s}}}\n", method, l->id);
	  if (lm != nullptr)
	    {
	      if (l->chan != nullptr)
		mcpc_http_chan_event (l->chan, lm, strlen (lm));
	      else
		_send_bytes (sv, sock, lm, strlen (lm));
	      free (lm);
	    }
	}
    }
  mcpc_mutex_unlock (&sv->mu);
  free (msg);
}

MCPC_API mcpc_errcode_t
mcpc_server_notify_tools_list_changed (mcpc_server_t *sv)
{
  if (!mcpc_server_supp_tools_lschg (sv))
    return MCPC_EC_CAPA_OFF;
  _notify_clients (sv, "notifications/tools/list_changed", nullptr);
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_notify_prmpts_list_changed (mcpc_server_t *sv)
{
  if (!mcpc_server_supp_prompts_lschg (sv))
    return MCPC_EC_CAPA_OFF;
  _notify_clients (sv, "notifications/prompts/list_changed", nullptr);
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_notify_rscs_list_changed (mcpc_server_t *sv)
{
  if (!mcpc_server_supp_rscs_lschg (sv))
    return MCPC_EC_CAPA_OFF;
  _notify_clients (sv, "notifications/resources/list_changed", nullptr);
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_server_notify_rsc_updated (mcpc_server_t *sv, const char8_t *urint)
{
  if (!mcpc_server_supp_rscs_subs (sv))
    return MCPC_EC_CAPA_OFF;
  _notify_clients (sv, "notifications/resources/updated", (const char *) urint);
  return MCPC_EC_0;
}

void
_handle_prmpt_call (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{
  if (!mcpc_server_supp_prompts (sv))
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_NOT_FOUND, "prompts is not supported", NULL);
      return;
    }

  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      return;
    }
  if (conn->client_init != MCPC_INIT_SUCC)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST, "client not initialized", NULL);
      return;
    }

  // prompt names are at most 255 bytes
  char8_t callname[256 + 1];
  size_t callname_len = 0;
  {
    const char *nm = nullptr;
    int nm_len = 0;
    if (mjson_find (r->params, r->params_len, "$.name", &nm, &nm_len) != MJSON_TOK_STRING || nm_len < 3 ||
	nm_len > 2 + 4 * 255)
      {
	jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "prompt not found", NULL);
	return;
      }
    char8_t *tmp = mcpc_alloc ((size_t) nm_len + 1);
    const int got = tmp != nullptr ? mjson_get_string (nm, nm_len, "$", (char *) tmp, nm_len + 1) : -1;
    if (got <= 0 || got > 255)
      {
	free (tmp);
	jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "prompt not found", NULL);
	return;
      }
    memcpy (callname, tmp, (size_t) got);
    callname[got] = 0;
    callname_len = (size_t) got;
    free (tmp);
  }

  const mcpc_prmpt_t *prmpt = (const mcpc_prmpt_t *) mcpc_prmptpool_getbyname (sv->prmptpool, callname, callname_len);
  if (prmpt == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "prompt not found", NULL);
      return;
    }

  // arguments may be left out when none are required
  const char *callargs = "{}";
  int callargs_len = 2;
  {
    const char *a = nullptr;
    int a_len = 0;
    const int at = mjson_find (r->params, r->params_len, "$.arguments", &a, &a_len);
    if (at == MJSON_TOK_OBJECT)
      {
	callargs = a;
	callargs_len = a_len;
      }
    else if (at != MJSON_TOK_INVALID && at != MJSON_TOK_NULL)
      {
	jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "arguments must be an object", NULL);
	return;
      }
  }

  // prompt arguments are always strings; a missing required one is the caller's mistake (-32602)
  const size_t n_args = prmpt->chd_pool != nullptr ? prmpt->chd_pool->len : 0;
  for (size_t i = 0; i < n_args; i++)
    {
      mcpc_prmptarg_t *parg = (mcpc_prmptarg_t *) mcpc_anydata_get_child (prmpt, i);
      parg->flags &= ~ANYDATA_FL_PROVIDED;
    }
  for (size_t i = 0; i < n_args; i++)
    {
      mcpc_prmptarg_t *parg = (mcpc_prmptarg_t *) mcpc_anydata_get_child (prmpt, i);
      char path[2 + 255 + 1];
      snprintf (path, sizeof (path), "$.%.*s", (int) parg->meta1_len, (const char *) parg->meta1);

      const char *tok = nullptr;
      int tok_len = 0;
      const int tt = mjson_find (callargs, callargs_len, path, &tok, &tok_len);
      if (tt == MJSON_TOK_INVALID || tt == MJSON_TOK_NULL)
	{
	  if (parg->flags & ANYDATA_FL_REQUIRED)
	    {
	      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "missing required argument", "%.*Q",
				    (int) parg->meta1_len, parg->meta1);
	      return;
	    }
	  continue;
	}
      char *buf = tt == MJSON_TOK_STRING ? mcpc_alloc ((size_t) tok_len + 1) : nullptr;
      const int got = buf != nullptr ? mjson_get_string (tok, tok_len, "$", buf, tok_len + 1) : -1;
      if (got < 0)
	{
	  free (buf);
	  jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "argument must be a string", "%.*Q",
				(int) parg->meta1_len, parg->meta1);
	  return;
	}
      mcpc_prmptarg_set_u8str (parg, (const char8_t *) buf, (size_t) got);
      parg->flags |= ANYDATA_FL_PROVIDED;
      free (buf);
    }

  struct mcpc_ctx ctx;
  if (!_ctx_begin (sv, conn, r, csock, &ctx, "prompts/get", (const char *) callname, callargs, callargs_len))
    return;

  mcpc_ucbr_t *ucbr = mcpc_ucbr_new ();

  mcpc_prmpt_callcb2_t callcb2 = prmpt->extra != nullptr ? (mcpc_prmpt_callcb2_t) prmpt->extra->cb2 : nullptr;
  mcpc_prmpt_callcb_t callcb = nullptr;
  mcpc_prmpt_get_callcb (prmpt, &callcb);
  if (callcb2 != nullptr)
    callcb2 (prmpt, &ctx, ucbr);
  else if (callcb != nullptr)
    callcb (prmpt, ucbr);

  if (!_ctx_input_required (sv, conn, r, csock, &ctx))
    {
      retbuf_t rtbf = _rb_new (sv, conn, 1024);
      serlz_prmpt_callres (rtbf, ucbr, (const char *) prmpt->meta2, prmpt->meta2_len);
      _reply_retbuf (sv, r, csock, rtbf);
    }

  mcpc_ctx_release (&ctx);
  mcpc_ucbr_free (ucbr);
}


void
_handle_complt_complt (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{
  if (!mcpc_server_supp_complt (sv))
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_NOT_FOUND, "completion is not supported", NULL);
      goto output_res;
    }

  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      goto output_res;
    }
  if (conn->client_init != MCPC_INIT_SUCC)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST, "client not initialized", NULL);
      goto output_res;
    }


  // ref.type
  char8_t *compref_type = alloca (1024);	// TODO  suff?
  size_t compref_type_len = 0;
  if (MCPC_EC_0 != _tulc_jparams_get_field_u8str (r, u8c1 ("$.ref.type"), &compref_type, &compref_type_len, 1024, true))
    return;

  int askfor_what = -1;

  if (false);
  else if (0 == u8strncmp (compref_type, u8c1 ("ref/prompt"), compref_type_len))
    askfor_what = 1;
  else if (0 == u8strncmp (compref_type, u8c1 ("ref/resource"), compref_type_len))
    askfor_what = 2;

  if (askfor_what < 0)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "<ref.type> invalid", NULL);
      return;
    }

  char8_t *compref_name = nullptr;	// TODO  suff?
  size_t compref_name_len = 0;
  char8_t *compref_uri = nullptr;	// TODO  suff?
  size_t compref_uri_len = 0;
  char8_t *comparg_name = nullptr;	// TODO  suff?
  size_t comparg_name_len = 0;
  char8_t *comparg_val = nullptr;	// TODO  suff?
  size_t comparg_val_len = 0;

  bool askfor_prmpt = askfor_what == 1;
  bool askfor_resc = askfor_what == 2;

  mcpc_complt_t *complt = nullptr;

  if (false);
  else if (askfor_prmpt)
    {
      //   ref.name
      compref_name = alloca (1024);	// TODO insuffi?
      if (MCPC_EC_0 !=
	  _tulc_jparams_get_field_u8str (r, u8c1 ("$.ref.name"), &compref_name, &compref_name_len, 1024, true))
	return;

      // argument.name
      comparg_name = alloca (1024);	// TODO insuffi?
      if (MCPC_EC_0 !=
	  _tulc_jparams_get_field_u8str (r, u8c1 ("$.argument.name"), &comparg_name, &comparg_name_len, 1024, true))
	return;

      // argument.value
      comparg_val = alloca (1024);	// TODO insuffi?
      if (MCPC_EC_0 !=
	  _tulc_jparams_get_field_u8str (r, u8c1 ("$.argument.value"), &comparg_val, &comparg_val_len, 1024, false))
	return;

      const mcpc_prmpt_t *tgt_prmpt = mcpc_prmptpool_getbyname (sv->prmptpool, compref_name, compref_name_len);
      if (tgt_prmpt == nullptr)
	{
	  jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "prompt not found", "%.*Q", compref_name_len, compref_name);
	  return;
	}

      const mcpc_prmptarg_t *tgt_prmptarg = mcpc_prmpt_getbyname_prmptarg (tgt_prmpt, comparg_name, comparg_name_len);
      if (tgt_prmptarg == nullptr)
	{
	  jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "promptarg not found", "%.*Q", comparg_name_len,
				comparg_name);
	  return;
	}

      complt = mcpc_complt_new_prmptarg (tgt_prmpt, comparg_name, comparg_name_len, comparg_val, comparg_val_len);
    }
  else if (askfor_resc)
    {
      //   ref.uri mycomm: rubbish design
      compref_uri = alloca (1024);
      if (MCPC_EC_0 !=
	  _tulc_jparams_get_field_u8str (r, u8c1 ("$.ref.uri"), &compref_uri, &compref_uri_len, 1024, false))
	return;
      complt = mcpc_complt_new_rsc (sv->rscpool, compref_uri, compref_uri_len);
    }

  bug_if_nullptr (complt);

  retbuf_t rtbf = _rb_new (sv, conn, 1024);

  serlz_complt (rtbf, complt);
  mcpc_complt_free (complt);


  _reply_retbuf (sv, r, csock, rtbf);


output_res:
  return;
}

#define ERA_LEGACY 1u		// initialize based revisions
#define ERA_MODERN 2u		// 2026-07-28 and later
#define ERA_BOTH (ERA_LEGACY | ERA_MODERN)

struct metho_ent
{
  const char *name;
  mcpc_metho_t metho;
  bool need_params;
  unsigned eras;		// the eras that have this method
};

// looked up linearly, the table is short and requests are not hot enough to matter
static const struct metho_ent metho_tab[] = {
  { "initialize", MCPC_METHO_INITBEG, true, ERA_LEGACY },
  { "notifications/initialized", MCPC_METHO_INITDONE, false, ERA_LEGACY },
  { "ping", MCPC_METHO_PING, false, ERA_LEGACY },
  { "server/discover", MCPC_METHO_DISCOVER, false, ERA_MODERN },
  { "subscriptions/listen", MCPC_METHO_SUB_LISTEN, true, ERA_MODERN },
  { "tools/list", MCPC_METHO_LISTTOOL, false, ERA_BOTH },
  { "tools/call", MCPC_METHO_CALLTOOL, true, ERA_BOTH },
  { "resources/list", MCPC_METHO_LSRSC, false, ERA_BOTH },
  { "resources/read", MCPC_METHO_RSC_READ, true, ERA_BOTH },
  { "resources/templates/list", MCPC_METHO_RSC_TMPL_LS, false, ERA_BOTH },
  { "resources/subscribe", MCPC_METHO_RSC_SUBS, true, ERA_LEGACY },
  { "resources/unsubscribe", MCPC_METHO_RSC_UNSUBS, true, ERA_LEGACY },
  { "prompts/list", MCPC_METHO_PRMPT_LS, false, ERA_BOTH },
  { "prompts/get", MCPC_METHO_PRMPT_CALL, true, ERA_BOTH },
  { "completion/complete", MCPC_METHO_COMPLT_COMPLT, true, ERA_BOTH },
  { "logging/setLevel", MCPC_METHO_LOG_SETLVL, true, ERA_LEGACY },
  { "notifications/cancelled", MCPC_METHO_CANCELLED, false, ERA_BOTH },
  { "notifications/roots/list_changed", MCPC_METHO_IGNORED, false, ERA_LEGACY },
};

static const struct metho_ent *
_metho_lookup (const char8_t *name)
{
  for (size_t i = 0; i < sizeof (metho_tab) / sizeof (metho_tab[0]); i++)
    if (strcmp (metho_tab[i].name, (const char *) name) == 0)
      return &metho_tab[i];
  return nullptr;
}

// ------------------------------ 2026-07-28 ------------------------------

#define META_PV "io.modelcontextprotocol/protocolVersion"
#define META_CAPS "io.modelcontextprotocol/clientCapabilities"
#define META_LOG "io.modelcontextprotocol/logLevel"

// JSON array of the revisions the server speaks, newest first
static void
_supported_versions (const mcpc_server_t *sv, char *out, size_t cap)
{
  size_t n = 0;
  out[n++] = '[';
  for (int v = MCPC_PV_2026_07_28; v >= MCPC_PV_2024_11_05; v--)
    {
      if (!(sv->pmask & mcpc_pver_bit ((mcpc_pver_t) v)))
	continue;
      n += (size_t) snprintf (out + n, cap - n, "%s\"%s\"", n > 1 ? "," : "", mcpc_pver_str ((mcpc_pver_t) v));
    }
  out[n++] = ']';
  out[n] = 0;
}

void
_handle_discover (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{
  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  char vers[128];
  _supported_versions (sv, vers, sizeof (vers));

  retbuf_t rtbf = _rb_new (sv, conn, 1024);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:%s,%Q:"), "supportedVersions", vers, "capabilities");
  serlz_capas (rtbf, sv);
  if (sv->instructions != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "instructions", sv->instructions);
  _serlz_cache_public (rtbf);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  _reply_retbuf (sv, r, csock, rtbf);
}

#define MCPC_MAX_LISTENS 64

// subscriptions/listen opens a stream that stays open: it is answered with an acknowledgment now,
// notifications later, and a result only when the stream is closed from our side
void
_handle_sub_listen (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{
  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      return;
    }
  if (r->id_len == 0)
    return;			// a notification cannot open a stream

  const char *filter = nullptr;
  int filter_len = 0;
  if (mjson_find (r->params, r->params_len, "$.notifications", &filter, &filter_len) != MJSON_TOK_OBJECT)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "notifications must be an object", NULL);
      return;
    }
  if (conn->nlistens >= MCPC_MAX_LISTENS)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "too many open subscriptions", NULL);
      return;
    }
  for (size_t i = 0; i < conn->nlistens; i++)
    if (strlen (conn->listens[i].id) == (size_t) r->id_len && memcmp (conn->listens[i].id, r->id, (size_t) r->id_len) == 0)
      {
	jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "subscription id already in use", NULL);
	return;
      }

  if (sv->typ == MCPC_SV_HTTP && (conn->http == nullptr || !conn->http->sse_ok))
    {
      _http_status (sv, csock, 406);
      jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST, "subscriptions/listen needs an Accept header with text/event-stream", NULL);
      return;
    }

  struct mcpc_listen l;
  memset (&l, 0, sizeof (l));
  l.chan = sv->typ == MCPC_SV_HTTP ? conn->http : nullptr;
  l.id = mcpc_alloc ((size_t) r->id_len + 1);
  if (l.id == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "out of memory", NULL);
      return;
    }
  memcpy (l.id, r->id, (size_t) r->id_len);
  l.id[r->id_len] = 0;

  // honor only what the server can produce
  const char *v;
  size_t vl;
  if (mcpc_json_member (filter, (size_t) filter_len, "toolsListChanged", &v, &vl) == MJSON_TOK_TRUE &&
      mcpc_server_supp_tools_lschg (sv))
    l.flags |= MCPC_LISTEN_TOOLS;
  if (mcpc_json_member (filter, (size_t) filter_len, "promptsListChanged", &v, &vl) == MJSON_TOK_TRUE &&
      mcpc_server_supp_prompts_lschg (sv))
    l.flags |= MCPC_LISTEN_PROMPTS;
  if (mcpc_json_member (filter, (size_t) filter_len, "resourcesListChanged", &v, &vl) == MJSON_TOK_TRUE &&
      mcpc_server_supp_rscs_lschg (sv))
    l.flags |= MCPC_LISTEN_RSCS;

  if (mcpc_json_member (filter, (size_t) filter_len, "resourceSubscriptions", &v, &vl) == MJSON_TOK_ARRAY &&
      mcpc_server_supp_rscs_subs (sv))
    {
      int koff, klen, voff, vlen, vtype;
      for (int off = 0; (off = mjson_next (v, (int) vl, off, &koff, &klen, &voff, &vlen, &vtype)) != 0;)
	{
	  if (vtype != MJSON_TOK_STRING || l.nuris >= MCPC_MAX_SUBS)
	    continue;
	  char *u = mcpc_alloc ((size_t) vlen + 1);
	  const int got = u != nullptr ? mjson_get_string (v + voff, vlen, "$", u, vlen + 1) : -1;
	  char **nu = got > 0 ? mcpc_realloc (l.uris, (l.nuris + 1) * sizeof (char *)) : nullptr;
	  if (nu == nullptr)
	    {
	      free (u);
	      continue;
	    }
	  u[got] = 0;
	  l.uris = nu;
	  l.uris[l.nuris++] = u;
	}
    }

  struct mcpc_listen *nl = mcpc_realloc (conn->listens, (conn->nlistens + 1) * sizeof (struct mcpc_listen));
  if (nl == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "out of memory", NULL);
      free (l.id);
      for (size_t j = 0; j < l.nuris; j++)
	free (l.uris[j]);
      free (l.uris);
      return;
    }
  conn->listens = nl;
  conn->listens[conn->nlistens++] = l;

  // the acknowledgment states the subset the server agreed to, and comes before anything else on the stream
  retbuf_t ack = retbuf_new (256);
  rtbuf_printf_mjfmt (ack, u8c1 ("{%Q:%Q,%Q:%Q,%Q:{%Q:{%Q:%s},%Q:{"), "jsonrpc", "2.0", "method",
		      "notifications/subscriptions/acknowledged", "params", "_meta",
		      "io.modelcontextprotocol/subscriptionId", l.id, "notifications");
  const char *sep = "";
  if (l.flags & MCPC_LISTEN_TOOLS)
    {
      rtbuf_printf_mjfmt (ack, u8c1 ("%Q:true"), "toolsListChanged");
      sep = ",";
    }
  if (l.flags & MCPC_LISTEN_PROMPTS)
    {
      rtbuf_printf_mjfmt (ack, u8c1 ("%s%Q:true"), sep, "promptsListChanged");
      sep = ",";
    }
  if (l.flags & MCPC_LISTEN_RSCS)
    {
      rtbuf_printf_mjfmt (ack, u8c1 ("%s%Q:true"), sep, "resourcesListChanged");
      sep = ",";
    }
  if (l.nuris > 0)
    {
      rtbuf_printf_mjfmt (ack, u8c1 ("%s%Q:["), sep, "resourceSubscriptions");
      for (size_t i = 0; i < l.nuris; i++)
	rtbuf_printf_mjfmt (ack, u8c1 ("%s%Q"), i ? "," : "", l.uris[i]);
      rtbuf_printf_mjfmt (ack, u8c1 ("]"));
    }
  rtbuf_printf_mjfmt (ack, u8c1 ("}}}\n"));
  if (retbuf_is_ok (ack))
    _send_bytes (sv, csock, retbuf_data (ack), retbuf_len (ack));
  retbuf_free (ack);
  if (l.chan != nullptr)
    l.chan->listen_open = true;	// the HTTP layer keeps the connection and the stream open
}

// notifications/cancelled naming an open listen stream closes it; no response is due for either request
static void
_cancel_listen (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
{
  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  const char *tok = nullptr;
  int tl = 0;
  if (conn == nullptr || r->params == nullptr ||
      mjson_find (r->params, r->params_len, "$.requestId", &tok, &tl) == MJSON_TOK_INVALID)
    return;
  for (size_t i = 0; i < conn->nlistens; i++)
    {
      struct mcpc_listen *l = &conn->listens[i];
      if (strlen (l->id) != (size_t) tl || memcmp (l->id, tok, (size_t) tl) != 0)
	continue;
      free (l->id);
      for (size_t j = 0; j < l->nuris; j++)
	free (l->uris[j]);
      free (l->uris);
      conn->listens[i] = conn->listens[--conn->nlistens];
      return;
    }
}

// Server side end of every open stream: the successful response tells the client it ended gracefully.
static void
_close_listens_conn (mcpc_server_t *sv, mcpc_conn_t *conn, mcpc_sock_t csock)
{
  while (conn != nullptr && conn->nlistens > 0)

    {
      struct mcpc_listen *l = &conn->listens[--conn->nlistens];
      char *m = mjson_aprintf ("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"resultType\":\"complete\",\"_meta\":{"
			       "\"io.modelcontextprotocol/subscriptionId\":%s}}}\n", l->id, l->id);
      if (m != nullptr)
	{
	  if (l->chan != nullptr)
	    {
	      mcpc_http_chan_final (l->chan, m, strlen (m));
	      l->chan->listen_open = false;
	    }
	  else
	    _send_bytes (sv, csock, m, strlen (m));
	  free (m);
	}
      free (l->id);
      for (size_t j = 0; j < l->nuris; j++)
	free (l->uris[j]);
      free (l->uris);
    }
}

static void
_close_listens (mcpc_server_t *sv, mcpc_sock_t csock)
{
  _close_listens_conn (sv, mcpc_connpool_find_by_sock (sv->connpool, csock), csock);
}

void
_close_listens_public (mcpc_server_t *sv, mcpc_conn_t *conn)
{
  _close_listens_conn (sv, conn, conn->typ == MCPC_CONN_FD ? conn->fd : (mcpc_sock_t) - 1);
}

// Run the handler of one method.
static void
_dispatch_method (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock, mcpc_metho_t m, bool *noreply)
{
  switch (m)
    {
    case MCPC_METHO_INITBEG:
      _handle_initbeg (sv, r, csock);
      break;
    case MCPC_METHO_INITDONE:
      _handle_initdone (sv, r, csock, noreply);
      break;
    case MCPC_METHO_PING:
      _handle_ping (sv, r, csock);
      break;
    case MCPC_METHO_DISCOVER:
      _handle_discover (sv, r, csock);
      break;
    case MCPC_METHO_SUB_LISTEN:
      _handle_sub_listen (sv, r, csock);
      break;
    case MCPC_METHO_LISTTOOL:
      _handle_tools_list (sv, r, csock);
      break;
    case MCPC_METHO_LSRSC:
      _handle_lsrsc (sv, r, csock);
      break;
    case MCPC_METHO_CALLTOOL:
      _handle_tools_call (sv, r, csock);
      break;
    case MCPC_METHO_RSC_READ:
      _handle_rsc_read (sv, r, csock);
      break;
    case MCPC_METHO_RSC_TMPL_LS:
      _handle_rsc_tmpl_list (sv, r, csock);
      break;
    case MCPC_METHO_RSC_SUBS:
      _handle_rsc_subscribe (sv, r, csock, true);
      break;
    case MCPC_METHO_RSC_UNSUBS:
      _handle_rsc_subscribe (sv, r, csock, false);
      break;
    case MCPC_METHO_PRMPT_LS:
      _handle_prmpt_list (sv, r, csock);
      break;
    case MCPC_METHO_PRMPT_CALL:
      _handle_prmpt_call (sv, r, csock);
      break;
    case MCPC_METHO_COMPLT_COMPLT:
      _handle_complt_complt (sv, r, csock);
      break;
    case MCPC_METHO_LOG_SETLVL:
      _handle_log_setlevel (sv, r, csock);
      break;
    case MCPC_METHO_CANCELLED:
      _cancel_listen (sv, r, csock);
      *noreply = true;
      break;
    case MCPC_METHO_IGNORED:
      *noreply = true;
      break;
    default:
      jsonrpc_return_error (r, JSONRPC_ERROR_NOT_FOUND, "method not support", NULL);
      break;
    }
}

// Run a handler with the request's own view of version, capabilities and log level laid over the
// connection: the handlers read those from the connection, a stateless request has them inline.
static void
_run_overlaid (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock, mcpc_conn_t *conn, const struct metho_ent *me,
	       bool *noreply, mcpc_pver_t pver, const char *caps, size_t caps_len, int log_level)
{
  const int saved_init = conn->client_init;
  const mcpc_pver_t saved_pver = conn->pver;
  char *const saved_caps = conn->ccaps;
  const size_t saved_caps_len = conn->ccaps_len;
  const int saved_log = conn->log_level;

  conn->overlay = true;
  conn->base_init = saved_init;
  conn->client_init = MCPC_INIT_SUCC;
  conn->pver = pver;
  conn->ccaps = (char *) caps;
  conn->ccaps_len = caps_len;
  conn->log_level = log_level;

  _dispatch_method (sv, r, csock, me->metho, noreply);

  conn->overlay = false;
  conn->client_init = (mcpc_init_stat_t) saved_init;
  conn->pver = saved_pver;
  conn->ccaps = saved_caps;
  conn->ccaps_len = saved_caps_len;
  conn->log_level = saved_log;
}

// Decide which era a request belongs to and run it accordingly:
//  - initialize selects the legacy handshake for the connection,
//  - a request carrying io.modelcontextprotocol/protocolVersion in _meta is served statelessly,
//  - anything else belongs to an initialized legacy connection.
static void
_route_request (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock, const struct metho_ent *me, bool *noreply)
{
  mcpc_conn_t *conn = mcpc_connpool_find_by_sock (sv->connpool, csock);
  if (conn == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "connection not found", NULL);
      return;
    }

  if (me->metho == MCPC_METHO_INITBEG)
    {
      if (!(sv->pmask & MCPC_PROTO_LEGACY))
	{
	  char vers[128];
	  _supported_versions (sv, vers, sizeof (vers));
	  jsonrpc_return_error (r, JSONRPC_ERROR_INVALID_REQUEST,
				"initialize is not supported, this server speaks only the protocol versions in data",
				"{%Q:%s}", "supported", vers);
	  return;
	}
      _dispatch_method (sv, r, csock, me->metho, noreply);
      if (sv->typ == MCPC_SV_HTTP)
	{
	  // what the handshake settled is kept in a session when it succeeded, the connection forgets it either way
	  if (conn->http != nullptr && conn->client_init == MCPC_INIT_BEG && conn->ccaps != nullptr)
	    mcpc_http_session_create (sv, conn->http, conn->pver, conn->ccaps, conn->ccaps_len);
	  free (conn->ccaps);
	  conn->ccaps = nullptr;
	  conn->ccaps_len = 0;
	  conn->client_init = MCPC_INIT_NONE;
	  conn->pver = MCPC_PV_NONE;
	}
      return;
    }

  const char *meta = nullptr;
  int meta_len = 0;
  const char *pv = nullptr;
  size_t pv_len = 0;
  int pv_tok = MJSON_TOK_INVALID;
  if (r->params != nullptr && mjson_find (r->params, r->params_len, "$._meta", &meta, &meta_len) == MJSON_TOK_OBJECT)
    pv_tok = mcpc_json_member (meta, (size_t) meta_len, META_PV, &pv, &pv_len);

  // ---- no per-request metadata: a legacy connection, or a client that skipped the handshake
  if (pv_tok == MJSON_TOK_INVALID)
    {
      if (!(me->eras & ERA_LEGACY))
	{
	  if (conn->pver == MCPC_PV_NONE)
	    {
	      _http_status (sv, csock, 400);
	      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS,
				    "missing required _meta field io.modelcontextprotocol/protocolVersion", NULL);
	    }
	  else
	    jsonrpc_return_error (r, JSONRPC_ERROR_NOT_FOUND, "method not support", NULL);
	  return;
	}
      if (sv->typ == MCPC_SV_HTTP)
	{
	  // no session over HTTP: a legacy request names its revision in the MCP-Protocol-Version header
	  // and is served as if its client had finished the handshake with no capabilities
	  const bool has_caps = conn->http != nullptr && conn->http->caps != nullptr;
	  _run_overlaid (sv, r, csock, conn, me, noreply, conn->http != nullptr ? conn->http->hdr_pver : MCPC_PV_2025_03_26,
			 has_caps ? conn->http->caps : "{}", has_caps ? conn->http->caps_len : 2, MCPC_LOG_INFO);
	  return;
	}
      if (conn->pver == MCPC_PV_NONE && me->metho != MCPC_METHO_PING && me->metho != MCPC_METHO_INITDONE &&
	  me->metho != MCPC_METHO_CANCELLED && me->metho != MCPC_METHO_IGNORED)
	{
	  jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS,
				"send initialize first, or io.modelcontextprotocol/protocolVersion in _meta", NULL);
	  return;
	}
      _dispatch_method (sv, r, csock, me->metho, noreply);
      return;
    }

  // ---- stateless request
  mcpc_pver_t pver = MCPC_PV_NONE;
  if (pv_tok == MJSON_TOK_STRING && pv_len >= 2)
    pver = mcpc_pver_parse (pv + 1, pv_len - 2);
  if (pv_tok != MJSON_TOK_STRING || pver != MCPC_PV_2026_07_28 || !(sv->pmask & mcpc_pver_bit (pver)))
    {
      char vers[128];
      _supported_versions (sv, vers, sizeof (vers));
      _http_status (sv, csock, 400);
      if (pv_tok == MJSON_TOK_STRING && mcpc_json_token_clean (pv_tok, pv, pv_len))
	jsonrpc_return_error (r, -32022, "Unsupported protocol version", "{%Q:%s,%Q:%.*s}", "supported", vers, "requested",
			      (int) pv_len, pv);
      else
	jsonrpc_return_error (r, -32022, "Unsupported protocol version", "{%Q:%s}", "supported", vers);
      return;
    }

  const char *caps = nullptr;
  size_t caps_len = 0;
  if (mcpc_json_member (meta, (size_t) meta_len, META_CAPS, &caps, &caps_len) != MJSON_TOK_OBJECT)
    {
      _http_status (sv, csock, 400);
      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "missing required _meta field "
			    "io.modelcontextprotocol/clientCapabilities", NULL);
      return;
    }

  int log_level = 99;		// no logLevel in _meta: the server must not log for this request
  const char *lv = nullptr;
  size_t lv_len = 0;
  const int lv_tok = mcpc_json_member (meta, (size_t) meta_len, META_LOG, &lv, &lv_len);
  if (lv_tok != MJSON_TOK_INVALID)
    {
      log_level = (lv_tok == MJSON_TOK_STRING && lv_len >= 2) ? _loglevel_parse (lv + 1, (int) lv_len - 2) : -1;
      if (log_level < 0)
	{
	  _http_status (sv, csock, 400);
	  jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "unknown log level", NULL);
	  return;
	}
    }

  if (!(me->eras & ERA_MODERN))
    {
      if (me->metho == MCPC_METHO_IGNORED || me->metho == MCPC_METHO_INITDONE)
	*noreply = true;
      else
	{
	  _http_status (sv, csock, 404);
	  jsonrpc_return_error (r, JSONRPC_ERROR_NOT_FOUND, "method not support", NULL);
	}
      return;
    }

  _run_overlaid (sv, r, csock, conn, me, noreply, pver, caps, caps_len, log_level);
}

void
handle_inone (mcpc_server_t *sv, const char *const rbuf, size_t nread, mcpc_sock_t csock)
{
  bool noreply = false;
  bool is_notif = false;
  bool id_bad = false;
  tuflog_d ("rbuf:%.*s", nread, rbuf);
  struct jsonrpc_request r;
  memset (&r, 0, sizeof (r));
  r.ctx = &jsonrpc_default_context;
  r.frame = rbuf;
  r.frame_len = (int) nread;
  r.fn = mcpc_print_grow;
  r.fn_data = (void *) &sv->rpcres;

  // the frame must be one JSON object, anything else cannot carry an id to answer to
  const char *top = nullptr;
  int top_len = 0;
  const int top_tok = mjson_find (rbuf, (int) nread, "$", &top, &top_len);
  if (top_tok == MJSON_TOK_INVALID)
    {
      jsonrpc_return_error (&r, JSONRPC_ERROR_INVALID, "parse error", NULL);
      goto output_res;
    }
  if (top_tok != MJSON_TOK_OBJECT)
    {
      jsonrpc_return_error (&r, JSONRPC_ERROR_INVALID_REQUEST, "invalid request", NULL);
      goto output_res;
    }

  // check id, only numbers and strings are valid ids, and one we cannot echo back as it is is not usable
  {
    const int id_tok = mjson_find (rbuf, (int) nread, "$.id", &r.id, &r.id_len);
    if ((id_tok != MJSON_TOK_NUMBER && id_tok != MJSON_TOK_STRING) || !mcpc_json_token_clean (id_tok, r.id, (size_t) r.id_len))
      {
	// an id that is there but cannot be used is an invalid request, not a notification
	id_bad = id_tok != MJSON_TOK_INVALID;
	r.id = nullptr;
	r.id_len = 0;
      }
  }

  // check method
  // Method must exist and must be a string
  const char8_t *methodname_q = nullptr;
  int32_t methodname_q_len = 0;
  if (mjson_find (rbuf, (int) nread, "$.method", (const char **) &methodname_q, &methodname_q_len) != MJSON_TOK_STRING)
    {
      const char *dummy = nullptr;
      int dummy_len = 0;
      // a response (e.g. to a request the server sent) has no method and needs no answer
      if (mjson_find (rbuf, (int) nread, "$.result", &dummy, &dummy_len) != MJSON_TOK_INVALID ||
	  mjson_find (rbuf, (int) nread, "$.error", &dummy, &dummy_len) != MJSON_TOK_INVALID)
	noreply = true;
      else
	jsonrpc_return_error (&r, JSONRPC_ERROR_INVALID_REQUEST, "invalid request", NULL);
      goto output_res;
    }

  if (id_bad)
    {
      jsonrpc_return_error (&r, JSONRPC_ERROR_INVALID_REQUEST, "invalid request id", NULL);
      goto output_res;
    }

  // a request without id is a notification, which must never be answered
  is_notif = (r.id_len == 0);

  // methodname_q includes the quotes; real method names are short
  const int32_t methodname_max = 128;
  if (methodname_q_len < 3 || methodname_q_len > methodname_max)
    {
      jsonrpc_return_error (&r, JSONRPC_ERROR_NOT_FOUND, "method not found", NULL);
      goto output_res;
    }

  char8_t methodname[128 + 1];
  int32_t methodname_len = mjson_get_string ((const char *) methodname_q, methodname_q_len, "$",
					     (char *) methodname, methodname_max);
  if (methodname_len <= 0)
    {
      jsonrpc_return_error (&r, JSONRPC_ERROR_NOT_FOUND, "method not found", NULL);
      goto output_res;
    }
  methodname[methodname_len] = 0;

  const struct metho_ent *me = _metho_lookup (methodname);
  if (me == nullptr)
    {
      _http_status (sv, csock, 404);
      jsonrpc_return_error (&r, JSONRPC_ERROR_NOT_FOUND, "method not support", NULL);
      goto output_res;
    }
  // params are optional for most methods but always parsed, list methods take a cursor
  mjson_find (rbuf, (int) nread, "$.params", &r.params, &r.params_len);
  if (me->need_params && r.params_len < 1)
    {
      jsonrpc_return_error (&r, JSONRPC_ERROR_BAD_PARAMS, "no params", NULL);
      goto output_res;
    }

  _route_request (sv, &r, csock, me, &noreply);

output_res:
  if (noreply || (is_notif && top_tok == MJSON_TOK_OBJECT))
    {
      sv->rpcres.len = 0;
      return;
    }
  if (sv->rpcres.len == 0)
    return;
  _flush_rpcres (sv, csock);
}
