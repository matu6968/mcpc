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

#ifndef h_server
#define h_server

#include <mcpc/visb.h>
#include <mcpc/server.h>
#include <mcpc/ctx.h>

#include "ext_stdint.h"
#include "ext_atomic.h"
#include "ext_mutex.h"
#include "proto.h"
#include "http.h"

#include "errcode.h"
#include "mjson.h"
#include "rsc.h"
#include "tool.h"
#include "prmpt.h"

#include <stdio.h>

#if is_win
#include <winsock2.h>
typedef i32_t socklen_t;
#endif

#define BUFFER_SIZE 1024
#define PORT 8080

// ------------------------------ conn ------------------------------

enum mcpc_conn_type
{
  MCPC_CONN_FD,
  MCPC_CONN_STRM,
};
typedef enum mcpc_conn_type mcpc_conn_type_t;

#if defined(is_unix)
typedef int mcpc_sock_t;
#elif is_win
typedef SOCKET mcpc_sock_t;
#endif

static inline bool
mcpc_sock_invalid (mcpc_sock_t sock)
{
#if defined(is_unix)
  return sock < 0;
#endif
#if is_win
  return sock == INVALID_SOCKET;
#endif
}

enum mcpc_init_stat
{
  MCPC_INIT_NONE = 0,
  MCPC_INIT_BEG,
  MCPC_INIT_FAIL,
  MCPC_INIT_SUCC,
};
typedef enum mcpc_init_stat mcpc_init_stat_t;

// one open subscriptions/listen stream of the 2026-07-28 revision
#define MCPC_LISTEN_TOOLS (1u << 0)
#define MCPC_LISTEN_PROMPTS (1u << 1)
#define MCPC_LISTEN_RSCS (1u << 2)
struct mcpc_listen
{
  char *id;			// the request id of subscriptions/listen as raw JSON, also the subscription id
  struct mcpc_http_chan *chan;	// HTTP: where the stream is written, NULL on the byte stream transports
  unsigned flags;
  char **uris;			// resource uris to report updates for
  size_t nuris;
};

// An HTTP session of an initialize-based client: what the handshake negotiated, kept for the requests
// that name it in Mcp-Session-Id.
struct mcpc_session
{
  char id[33];
  mcpc_pver_t pver;
  char *ccaps;
  size_t ccaps_len;
  long last;			// time of the last request, seconds
  unsigned long used;		// order of use, to pick the least recently used one
};

// A question sent to a legacy client on the stream of a request, waiting for its answer to arrive as a POST.
struct mcpc_pending
{
  char id[40];
  char session[33];
  bool done;
  bool failed;
  char *json;			// the result (or error) object of the answer
  struct mcpc_pending *next;
};

typedef struct mcpc_conn mcpc_conn_t;
struct mcpc_conn
{
  mcpc_conn_type_t typ;
  union
  {
    mcpc_sock_t fd;
    FILE *strm;
  };
  const ch8_t *cname;
  u8_t cname_len;
  mcpc_init_stat_t client_init;
  mcpc_pver_t pver;		// negotiated protocol revision
  char *ccaps;			// raw JSON of the client capabilities from initialize
  size_t ccaps_len;
  int log_level;		// logging/setLevel, MCPC_LOG_*
  bool overlay;			// a stateless request is being served with its own view of version and capabilities
  int base_init;		// client_init as the handshake left it, valid while overlay is set
  struct mcpc_http_chan *http;	// reply channel of the HTTP request being served
  char **subs;			// resource URIs subscribed to with resources/subscribe
  size_t nsubs;
  char **pend;			// frames received while a callback waited for the client
  size_t npend;
  struct mcpc_listen *listens;	// open subscriptions/listen streams
  size_t nlistens;
  unsigned long next_req;	// id of the next request we send to the client
  mcpc_conn_t *nex;
};

typedef uint8_t mcpc_connpool_size_t;

typedef struct mcpc_connpool mcpc_connpool_t;
struct mcpc_connpool
{
  const mcpc_connpool_size_t cap;
  mcpc_connpool_size_t len;
  mcpc_conn_t *head;
};

// ----------------------------- server -----------------------------

enum mcpc_server_type
{
  MCPC_SV_NONE = 0,
  MCPC_SV_IOSTRM,
  MCPC_SV_STDIO,
  MCPC_SV_TCP,
  MCPC_SV_HTTP,
};
typedef enum mcpc_server_type mcpc_server_type_t;

struct mcpc_server
{
  mcpc_server_type_t typ;
  const ch8_t *name;
  u8_t name_len;
  const ch8_t *ver;
  u8_t ver_len;
  u16_t capa;
  const mcpc_toolpool_t *toolpool;
  const mcpc_prmptpool_t *prmptpool;
  const mcpc_rscpool_t *rscpool;
  const mcpc_anypool_t *rsctplpool;
  struct mjson_fixedbuf rpcres;
  char *title;			// Implementation.title
  char *desc;			// Implementation.description
  char *website;		// Implementation.websiteUrl
  char *icons_json;		// raw JSON array of icons
  char *instructions;		// hints for the model, returned by initialize
  size_t page_size;		// max items per list page, 0 = no pagination
  unsigned pmask;		// MCPC_PROTO_* revisions served
  unsigned long ttl_ms;		// ttlMs of cacheable results (2026-07-28)
  bool cache_public;		// cacheScope of cacheable results
  char *extensions_json;	// raw JSON object advertised as capabilities.extensions
  uint8_t *state_key;		// HMAC key for requestState, NULL = not issued
  size_t state_key_len;
  unsigned state_ttl;		// seconds
  mcpc_http_opts_t http;	// HTTP transport settings, defaults filled in by mcpc_server_start_http
  mcpc_fd_t lfd;
  int stop;			// set by mcpc_server_stop, access through mcpc_flag_get/set
  size_t nconns;		// open HTTP connections
  mcpc_mutex_t cmu;		// protects nconns
  struct mcpc_session *sessions;	// guarded by mu
  size_t nsessions;
  unsigned long session_clock;
  struct mcpc_pending *pending;	// guarded by the lock of pcv
  mcpc_cond_t pcv;
  const mcpc_connpool_t *connpool;
  mcpc_mutex_t mu;		// serializes request handling across connection threads
};
// implication: for iostrm server, the first conn will be used as client init status storing
mcpc_errcode_t mcpc_connpool_remove_by_sock (mcpc_connpool_t * connpool, mcpc_sock_t sock);

mcpc_conn_t *mcpc_connpool_find_by_sock (const mcpc_connpool_t * connpool, mcpc_sock_t sock);

// check mcpc_connpool_find_by_sock

#define MCPC_CAPA_PRMT         0b0000000000000001
#define MCPC_CAPA_PRMT_LISTCHG 0b0000000000000011
#define MCPC_CAPA_TOOL         0b0000000000010000
#define MCPC_CAPA_TOOL_LISTCHG 0b0000000000110000
#define MCPC_CAPA_RSC          0b0000000100000000
#define MCPC_CAPA_RSC_LISTCHG  0b0000001100000000
#define MCPC_CAPA_RSC_SUBSRC   0b0000010100000000
#define MCPC_CAPA_COMPLT       0b0001000000000000
#define MCPC_CAPA_LOG          0b0010000000000000

#define _FIRST_TOOL_NAME u8c1("mcpc-info")
#define _FIRST_TOOL_DESC u8c1("description: MCP protocol in C implementation; version: 0.1")

mcpc_server_t *mcpc_server_new (mcpc_server_type_t typ, mcpc_toolpool_t * toolpool, u32_t vargs_beg, ...);

mcpc_server_t *mcpc_server_new_stdio ();

/* void _handle_tools_list(mcpc_server_t *sv, struct jsonrpc_request *r); */

/* void _handle_tools_call(mcpc_server_t *sv, struct jsonrpc_request *r); */

mcpc_errcode_t mcpc_server_cleanup_rpcres (mcpc_server_t * sv);

bool _server_is_buggy (const mcpc_server_t * sv);

// HTTP sessions and the questions waiting for an answer, see httpd.c
bool mcpc_http_session_create (mcpc_server_t * sv, struct mcpc_http_chan *ch, mcpc_pver_t pver, const char *caps, size_t caps_len);

mcpc_errcode_t mcpc_http_request_client (mcpc_server_t * sv, mcpc_conn_t * conn, const char *method, const char *params_json, char *ret,
					 size_t ret_cap, size_t *ret_len);

mcpc_errcode_t mcpc_conn_free (mcpc_conn_t * conn);

mcpc_errcode_t mcpc_http_run (mcpc_server_t * sv, const mcpc_http_opts_t * opts);

// end every subscriptions/listen stream of one connection with the graceful closing response
void _close_listens_public (mcpc_server_t * sv, mcpc_conn_t * conn);

void handle_inone (mcpc_server_t * sv, const char *const rbuf, size_t nread, mcpc_sock_t csock);

bool mcpc_server_supp_prompts (const mcpc_server_t * sv);

bool mcpc_server_supp_prompts_lschg (const mcpc_server_t * sv);

bool mcpc_server_supp_tools (const mcpc_server_t * sv);

bool mcpc_server_supp_tools_lschg (const mcpc_server_t * sv);

bool mcpc_server_supp_rscs (const mcpc_server_t * sv);

bool mcpc_server_supp_rscs_lschg (const mcpc_server_t * sv);

bool mcpc_server_supp_rscs_subs (const mcpc_server_t * sv);

bool mcpc_server_supp_complt (const mcpc_server_t * sv);

bool mcpc_server_supp_log (const mcpc_server_t * sv);

enum mcpc_metho
{
  MCPC_METHO_UNKNOWN = 0,
  MCPC_METHO_INITBEG,
  MCPC_METHO_INITDONE,
  MCPC_METHO_PING,
  MCPC_METHO_LISTTOOL,
  MCPC_METHO_CALLTOOL,
  MCPC_METHO_LSRSC,
  MCPC_METHO_RSC_READ,
  MCPC_METHO_RSC_TMPL_LS,
  MCPC_METHO_RSC_SUBS,
  MCPC_METHO_RSC_UNSUBS,
  MCPC_METHO_PRMPT_LS,
  MCPC_METHO_PRMPT_CALL,
  MCPC_METHO_COMPLT_COMPLT,
  MCPC_METHO_LOG_SETLVL,
  MCPC_METHO_DISCOVER,		// server/discover
  MCPC_METHO_SUB_LISTEN,	// subscriptions/listen
  MCPC_METHO_CANCELLED,		// notifications/cancelled
  MCPC_METHO_IGNORED,		// known notification that needs no action
};
typedef enum mcpc_metho mcpc_metho_t;

// ---------------------------- connpool ----------------------------

mcpc_conn_t *mcpc_conn_new (mcpc_conn_type_t typ, void *back);

FILE *mcpc_conn_getstrm (mcpc_conn_t * conn);

int mcpc_conn_getfd (mcpc_conn_t * conn);

// TODO private
mcpc_errcode_t mcpc_connpool_addref (mcpc_connpool_t * connpool, mcpc_conn_t * conn);

mcpc_conn_t *mcpc_connpool_getconn (const mcpc_connpool_t * connpool, const mcpc_connpool_size_t idx);

#endif
