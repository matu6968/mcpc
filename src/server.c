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
#include "frame.h"
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

// Send a serialized result (takes ownership of rtbf). A buffer that could not
// be fully written is reported as an internal error rather than sent truncated.
static void
_reply_retbuf (struct jsonrpc_request *r, mcpc_sock_t csock, retbuf_t rtbf)
{
  if (!retbuf_is_ok (rtbf) || retbuf_len (rtbf) == 0)
    jsonrpc_return_error (r, JSONRPC_ERROR_INTERNAL, "failed to serialize response", NULL);
  else
    jsonrpc_return_success2 (r, mcpc_sock_invalid (csock), "%.*s", (int) retbuf_len (rtbf), retbuf_data (rtbf));
  retbuf_free (rtbf);
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
  else if (typ == MCPC_SV_TCP)
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

static inline bool
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


// Write the accumulated response in sv->rpcres to the peer and reset it.
// A peer that went away is not an error for the server, so failures are dropped.
static void
_flush_rpcres (mcpc_server_t *sv, mcpc_sock_t csock)
{
  if (sv->rpcres.len <= 0)
    return;

  if (sv->typ == MCPC_SV_IOSTRM)
    {
      tuflog_d ("sv->rpcres:%.*s", sv->rpcres.len, sv->rpcres.ptr);
      mcpc_conn_t *outconn = mcpc_connpool_getconn (sv->connpool, 1);
      FILE *out_strm = outconn != nullptr ? mcpc_conn_getstrm (outconn) : nullptr;
      if (out_strm != nullptr)
	{
	  fwrite (sv->rpcres.ptr, 1, (size_t) sv->rpcres.len, out_strm);
	  fflush (out_strm);
	}
    }
  else if (sv->typ == MCPC_SV_TCP)
    {
      size_t sent = 0;
      while (sent < (size_t) sv->rpcres.len)
	{
#if is_unix
	  const ssize_t n = send (csock, sv->rpcres.ptr + sent, (size_t) sv->rpcres.len - sent, MSG_NOSIGNAL);
#else
	  const int n = send (csock, sv->rpcres.ptr + sent, sv->rpcres.len - (int) sent, 0);
#endif
	  if (n <= 0)
	    break;
	  sent += (size_t) n;
	}
    }

  sv->rpcres.len = 0;
}

// An incoming object exceeded the size limit and was dropped; its id is unknown.
static void
_reply_toobig (mcpc_server_t *sv, mcpc_sock_t csock)
{
  struct jsonrpc_request r;
  memset (&r, 0, sizeof (r));
  r.fn = mcpc_print_grow;
  r.fn_data = (void *) &sv->rpcres;
  jsonrpc_return_error (&r, JSONRPC_ERROR_INVALID_REQUEST, "message too large", NULL);
  _flush_rpcres (sv, csock);
}

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
  mcpc_framer_init (&framer, MCPC_FRAME_DEFAULT_MAX);

  int ch;
  while ((ch = getc (in_strm)) != EOF)
    {
      const mcpc_frame_stat_t st = mcpc_framer_push (&framer, (unsigned char) ch);
      if (st == MCPC_FRAME_READY)
	{
	  handle_inone (sv, framer.buf, framer.len, (mcpc_sock_t) - 1);
	  mcpc_framer_next (&framer);
	}
      else if (st == MCPC_FRAME_TOOBIG)
	_reply_toobig (sv, (mcpc_sock_t) - 1);
      else if (st == MCPC_FRAME_NOMEM)
	break;
    }

  mcpc_framer_free (&framer);

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
	  mcpc_mutex_unlock (&sv->mu);
	  mcpc_framer_next (&framer);
	  tulog_d ("one msg handled");
	}
      else if (st == MCPC_FRAME_TOOBIG)
	{
	  mcpc_mutex_lock (&sv->mu);
	  _reply_toobig (sv, csock);
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

  mcpc_print_grow_free (&sv->rpcres);
  mcpc_mutex_destroy (&sv->mu);
  mcpc_toolpool_free ((mcpc_toolpool_t *) sv->toolpool);
  mcpc_prmptpool_free ((mcpc_prmptpool_t *) sv->prmptpool);
  mcpc_rscpool_free ((mcpc_rscpool_t *) sv->rscpool);
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

  // TODO P6 capabilities negotiation

  retbuf_t rtbf = retbuf_new (1024);
  mcpc_assert (rtbf != nullptr, MCPC_EC_BUG);
  mcpc_assert (!serlz_initres (rtbf, sv), MCPC_EC_BUG);
  _reply_retbuf (r, csock, rtbf);

  conn->client_init = MCPC_INIT_BEG;

output_res:
  return;
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

  retbuf_t rtbf = retbuf_new (1024);
  mcpc_assert (rtbf != nullptr, MCPC_EC_BUG);
  mcpc_assert (!serlz_toolpool (rtbf, sv->toolpool), MCPC_EC_BUG);
  _reply_retbuf (r, csock, rtbf);

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

  retbuf_t rtbf = retbuf_new (1024);
  mcpc_assert (rtbf != nullptr, MCPC_EC_BUG);
  mcpc_assert (!serlz_prmptpool (rtbf, sv->prmptpool), MCPC_EC_BUG);
  _reply_retbuf (r, csock, rtbf);

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

  retbuf_t rtbf = retbuf_new (1024);
  mcpc_assert (rtbf != nullptr, MCPC_EC_BUG);
  mcpc_assert (!serlz_rscpool (rtbf, sv->rscpool), MCPC_EC_BUG);
  _reply_retbuf (r, csock, rtbf);

output_res:
  return;
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

  char8_t *callname = nullptr;	// TODO  suff?
  size_t callname_len = 0;

  callname = alloca (1024);	// TODO insuffi?
  if (MCPC_EC_0 != _tulc_jparams_get_field_u8str (r, u8c1 ("$.name"), &callname, &callname_len, 1024, true))
    return;

  // const mcpc_tool_t *tool = mcpc_toolpool_find_tool (sv->toolpool, callname, callname_len);

  const mcpc_tool_t *tool = (const mcpc_tool_t *) mcpc_toolpool_getbyname (sv->toolpool, callname, callname_len);
  if (tool == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "tool not found", NULL);
      goto output_res;
    }

  const char8_t *callargs = nullptr;
  size_t callargs_len = 0;
  callargs = alloca (1024);	// TODO insuffi?
  if (MCPC_EC_0 != _tulc_jparams_getref_field_obj (r, u8c1 ("$.arguments"), &callargs, &callargs_len))
    return;

  const mcpc_toolprop_t *cur_tprop = nullptr;
  while ((cur_tprop = mcpc_tool_get_next_toolprop (tool)))
    {
      const char8_t *cur_tp_name = nullptr;
      size_t cur_tp_name_len = 0;
      bug_if_nz (mcpc_toolprop_getref_name (cur_tprop, &cur_tp_name, &cur_tp_name_len));
      mcpc_anytype_t cur_tp_type = MCPC_NONE;
      bug_if_nz (mcpc_toolprop_get_data_type (cur_tprop, &cur_tp_type));
      char8_t *cur_tp_jpath = alloca (128);
      sprintf ((char *) cur_tp_jpath, "$.%.*s", (int) cur_tp_name_len, cur_tp_name);
      if (false)
	;
      else if (cur_tp_type == MCPC_I32)
	{
	  double propval = 0;
	  if (MCPC_EC_0 != _tulc_jstr_get_field_number (r, callargs, callargs_len, cur_tp_jpath, &propval))
	    return;
	  i32_t tmp = (int) propval;
	  mcpc_toolprop_set_i32 ((mcpc_toolprop_t *) cur_tprop, tmp);
	}
      else if (cur_tp_type == MCPC_U8STR)
	{
	  // copy is essential here
	  size_t propval_cap = 1024;
	  char8_t *propval = mcpc_alloc (propval_cap);	// TODO  alloca maybe suffi
	  size_t propval_len = 0;

	  while (true)
	    {
	      mcpc_errcode_t ec =
		_tulc_jstr_get_field_u8str (r, callargs, callargs_len, cur_tp_jpath, &propval, &propval_len,
					    propval_cap, false);
	      if (ec == MCPC_EC_0)
		break;
	      else if (ec == MCPC_EC_BUFCAP)
		{
		  propval_cap += propval_cap;
		  propval = mcpc_realloc (propval, propval_cap);
		  propval_len = 0;
		  continue;
		}
	      else
		return;
	    }


	  mcpc_toolprop_set_u8str ((mcpc_toolprop_t *) cur_tprop, propval, (size_t) propval_len);
	  free (propval);
	}
    }

  mcpc_ucbr_t *ucbr = mcpc_ucbr_new ();

  mcpc_tcallcb_t callcb = nullptr;
  mcpc_tool_get_call_cb (tool, &callcb);
  callcb (tool, ucbr);

  retbuf_t rtbf = retbuf_new (mcpc_ucbr_get_serlz_lenhint (ucbr));
  mcpc_assert (rtbf != nullptr, MCPC_EC_BUG);

  serlz_tool_callres (rtbf, ucbr);

  _reply_retbuf (r, csock, rtbf);

  mcpc_ucbr_free (ucbr);

output_res:
  return;
}

void
_handle_prmpt_call (mcpc_server_t *sv, struct jsonrpc_request *r, mcpc_sock_t csock)
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

  char8_t *callname = nullptr;
  size_t callname_len = 0;

  callname = alloca (128);	// TODO insuffi?
  if (MCPC_EC_0 != _tulc_jparams_get_field_u8str (r, u8c1 ("$.name"), &callname, &callname_len, 1024, true))
    return;

  bug_if (callname_len == 0);

  const mcpc_prmpt_t *prmpt = (const mcpc_prmpt_t *) mcpc_prmptpool_getbyname (sv->prmptpool, callname, callname_len);
  if (prmpt == nullptr)
    {
      jsonrpc_return_error (r, JSONRPC_ERROR_BAD_PARAMS, "prompt not found", NULL);
      goto output_res;
    }


  const char8_t *callargs = nullptr;
  size_t callargs_len = 0;
  callargs = alloca (1024);	// TODO insuffi?
  if (MCPC_EC_0 != _tulc_jparams_getref_field_obj (r, u8c1 ("$.arguments"), &callargs, &callargs_len))
    return;

  const mcpc_prmptarg_t *cur_parg = nullptr;
  while ((cur_parg = mcpc_prmpt_get_next_prmptarg (prmpt)))
    {
      const char8_t *cur_tp_name = nullptr;
      size_t cur_tp_name_len = 0;
      bug_if_nz (mcpc_prmptarg_getref_name (cur_parg, &cur_tp_name, &cur_tp_name_len));
      mcpc_anytype_t cur_tp_type = MCPC_NONE;
      bug_if_nz (mcpc_prmptarg_get_data_type (cur_parg, &cur_tp_type));
      char8_t *cur_tp_jpath = alloca (100);
      sprintf ((char *) cur_tp_jpath, "$.%.*s", (int) cur_tp_name_len, cur_tp_name);
      if (false)
	;
      else if (cur_tp_type == MCPC_U8STR)
	{
	  char8_t *propval = alloca (1024);	// TODO  might be insuff
	  size_t propval_len = 0;

	  // copy is essential here
	  if (MCPC_EC_0 !=
	      _tulc_jstr_get_field_u8str (r, callargs, callargs_len, cur_tp_jpath, &propval, &propval_len, 1024, true))
	    return;

	  mcpc_prmptarg_set_u8str ((mcpc_prmptarg_t *) cur_parg, propval, (size_t) propval_len);
	}
    }

  mcpc_ucbr_t *ucbr = mcpc_ucbr_new ();

  mcpc_prmpt_callcb_t callcb = nullptr;
  mcpc_prmpt_get_callcb (prmpt, &callcb);
  callcb (prmpt, ucbr);

  retbuf_t rtbf = retbuf_new (1024);
  mcpc_assert (rtbf != nullptr, MCPC_EC_BUG);

  serlz_prmpt_callres (rtbf, ucbr);

  _reply_retbuf (r, csock, rtbf);

  mcpc_ucbr_free (ucbr);

output_res:
  return;
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

  retbuf_t rtbf = retbuf_new (1024);
  mcpc_assert (rtbf != nullptr, MCPC_EC_BUG);

  serlz_complt (rtbf, complt);
  mcpc_complt_free (complt);


  _reply_retbuf (r, csock, rtbf);


output_res:
  return;
}

bool
is_metho_need_params (mcpc_metho_t metho)
{
  mcpc_assert (metho != MCPC_METHO_UNKNOWN, MCPC_EC_BUG);
  if (metho == MCPC_METHO_INITBEG)
    return true;
  if (metho == MCPC_METHO_CALLTOOL)
    return true;
  if (metho == MCPC_METHO_PRMPT_CALL)
    return true;
  if (metho == MCPC_METHO_COMPLT_COMPLT)
    return true;
  return false;
}

void
handle_inone (mcpc_server_t *sv, const char *const rbuf, size_t nread, mcpc_sock_t csock)
{
  bool noreply = false;
  bool is_notif = false;
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

  // check id, only numbers and strings are valid ids
  {
    const int id_tok = mjson_find (rbuf, (int) nread, "$.id", &r.id, &r.id_len);
    if (id_tok != MJSON_TOK_NUMBER && id_tok != MJSON_TOK_STRING)
      {
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

  mcpc_metho_t handle_what = MCPC_METHO_UNKNOWN;	// nothing

  if (false)
    ;
  else if (u8streq (methodname, u8c1 ("initialize")))
    handle_what = MCPC_METHO_INITBEG;
  else if (u8streq (methodname, u8c1 ("notifications/initialized")))
    handle_what = MCPC_METHO_INITDONE;
  else if (u8streq (methodname, u8c1 ("tools/list")))
    handle_what = MCPC_METHO_LISTTOOL;
  else if (u8streq (methodname, u8c1 ("tools/call")))
    handle_what = MCPC_METHO_CALLTOOL;
  else if (u8streq (methodname, u8c1 ("resources/list")))
    handle_what = MCPC_METHO_LSRSC;
  else if (u8streq (methodname, u8c1 ("prompts/list")))
    handle_what = MCPC_METHO_PRMPT_LS;
  else if (u8streq (methodname, u8c1 ("prompts/get")))
    handle_what = MCPC_METHO_PRMPT_CALL;
  else if (u8streq (methodname, u8c1 ("completion/complete")))
    handle_what = MCPC_METHO_COMPLT_COMPLT;

  if (handle_what == MCPC_METHO_UNKNOWN)
    {
      jsonrpc_return_error (&r, JSONRPC_ERROR_NOT_FOUND, "method not support", NULL);
      goto output_res;
    }

  // check params
  if (is_metho_need_params (handle_what))
    {
      mjson_find (rbuf, (int) nread, "$.params", &r.params, &r.params_len);
      if (r.params_len < 1)
	{
	  jsonrpc_return_error (&r, JSONRPC_ERROR_BAD_PARAMS, "no params", NULL);
	  goto output_res;
	}
    }

  if (false)
    ;
  else if (handle_what == MCPC_METHO_INITBEG)
    _handle_initbeg (sv, &r, csock);
  else if (handle_what == MCPC_METHO_INITDONE)
    _handle_initdone (sv, &r, csock, &noreply);
  else if (handle_what == MCPC_METHO_LISTTOOL)
    _handle_tools_list (sv, &r, csock);
  else if (handle_what == MCPC_METHO_LSRSC)
    _handle_lsrsc (sv, &r, csock);
  else if (handle_what == MCPC_METHO_CALLTOOL)
    _handle_tools_call (sv, &r, csock);
  else if (handle_what == MCPC_METHO_PRMPT_LS)
    _handle_prmpt_list (sv, &r, csock);
  else if (handle_what == MCPC_METHO_PRMPT_CALL)
    _handle_prmpt_call (sv, &r, csock);
  else if (handle_what == MCPC_METHO_COMPLT_COMPLT)
    _handle_complt_complt (sv, &r, csock);

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
