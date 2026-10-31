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

#ifndef h_pub_server
#define h_pub_server

#include <stdio.h>

#include "visb.h"
#include "_c23_keywords.h"

#include "errcode.h"
#include "tool.h"
#include "prmpt.h"
#include "rsc.h"


typedef struct mcpc_server mcpc_server_t;

MCPC_API mcpc_server_t *
mcpc_server_new_iostrm (const FILE *const strm_in, const FILE *const strm_out);

MCPC_API mcpc_server_t *
mcpc_server_new_tcp ();

// ---- Streamable HTTP ----
//
// One endpoint accepting POST, served over plain HTTP/1.1 (put a TLS proxy in front for HTTPS).
// Every field left zero takes its default.

typedef struct mcpc_http_opts
{
  const char *host;		// address to listen on, default "127.0.0.1"
  unsigned short port;		// default 8080; 0 picks a free port, see on_listening
  const char *path;		// the MCP endpoint, default "/mcp"
  const char *const *allowed_origins;	// NULL terminated; "*" allows any. Default: only loopback origins (requests without Origin are always fine)
  const char *const *allowed_hosts;	// NULL terminated host names (no port) accepted in Host. Default: the loopback names while bound to a loopback address, any host otherwise
  size_t max_body;		// largest request body, default 4 MiB
  size_t max_conns;		// simultaneous connections, default 64, at most 250
  unsigned header_timeout_ms;	// time a client has to send a complete request, default 15000
  unsigned idle_timeout_ms;	// how long a connection may sit idle between requests, default 60000
  void (*on_listening) (unsigned short port, void *ud);	// called once the socket is bound
  void *ud;
} mcpc_http_opts_t;

MCPC_API mcpc_server_t *
mcpc_server_new_http ();

// Serve until mcpc_server_stop. opts may be NULL for all defaults. Requests are handled one at a
// time, a slow tool delays the others. Authentication is not part of the library.
MCPC_API mcpc_errcode_t
mcpc_server_start_http (mcpc_server_t *sv, const mcpc_http_opts_t *opts);

// Ask a running server to finish: open subscriptions/listen streams are closed gracefully and
// mcpc_server_start returns. Safe to call from a signal handler or another thread.
MCPC_API void
mcpc_server_stop (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_set_name (mcpc_server_t *sv, const char8_t *name, size_t name_len);

MCPC_API mcpc_errcode_t
mcpc_server_set_nament (mcpc_server_t *sv, const char8_t *nament);

// Implementation info reported in the initialize result. All strings are copied.
MCPC_API mcpc_errcode_t
mcpc_server_set_version (mcpc_server_t *sv, const char8_t *vernt);

MCPC_API mcpc_errcode_t
mcpc_server_set_title (mcpc_server_t *sv, const char8_t *titlent);

MCPC_API mcpc_errcode_t
mcpc_server_set_description (mcpc_server_t *sv, const char8_t *descnt);

MCPC_API mcpc_errcode_t
mcpc_server_set_website_url (mcpc_server_t *sv, const char8_t *urlnt);

// JSON array of icon objects, e.g. [{"src":"https://...","mimeType":"image/png"}]
MCPC_API mcpc_errcode_t
mcpc_server_set_icons_json (mcpc_server_t *sv, const char8_t *json_array_nt);

// Usage hints for the model, returned once by initialize.
// Protocol revisions the server speaks, a mask of MCPC_PROTO_*. The default is all of them. Without
// the legacy ones, initialize is refused with an error that names the supported versions.
#define MCPC_PROTO_2024_11_05 (1u << 0)
#define MCPC_PROTO_2025_03_26 (1u << 1)
#define MCPC_PROTO_2025_06_18 (1u << 2)
#define MCPC_PROTO_2025_11_25 (1u << 3)
#define MCPC_PROTO_2026_07_28 (1u << 4)
#define MCPC_PROTO_LEGACY (MCPC_PROTO_2024_11_05 | MCPC_PROTO_2025_03_26 | MCPC_PROTO_2025_06_18 | MCPC_PROTO_2025_11_25)
#define MCPC_PROTO_ALL (MCPC_PROTO_LEGACY | MCPC_PROTO_2026_07_28)

MCPC_API mcpc_errcode_t
mcpc_server_set_protocol_mask (mcpc_server_t *sv, unsigned mask);

// Caching hints attached to list and read results of the 2026-07-28 revision (default: 0 ms, private).
// is_public promises that no result contains caller specific data.
MCPC_API mcpc_errcode_t
mcpc_server_set_cache (mcpc_server_t *sv, unsigned long ttl_ms, bool is_public);

// JSON object of extension settings advertised in the capabilities of the 2026-07-28 revision,
// e.g. {"io.modelcontextprotocol/ui":{}}.
MCPC_API mcpc_errcode_t
mcpc_server_set_extensions_json (mcpc_server_t *sv, const char8_t *json_object_nt);

// Key that protects requestState round trips (HMAC-SHA256); without one requestState is not issued.
// Use at least 16 random bytes. The key is copied.
MCPC_API mcpc_errcode_t
mcpc_server_set_state_secret (mcpc_server_t *sv, const void *key, size_t key_len);

// Lifetime of a requestState in seconds (default 300).
MCPC_API mcpc_errcode_t
mcpc_server_set_state_ttl (mcpc_server_t *sv, unsigned seconds);

MCPC_API mcpc_errcode_t
mcpc_server_set_instructions (mcpc_server_t *sv, const char8_t *textnt);

// Largest number of items in one tools/prompts/resources list page, 0 (default) disables pagination.
MCPC_API mcpc_errcode_t
mcpc_server_set_page_size (mcpc_server_t *sv, size_t page_size);

// Advertise the logging utility (logging/setLevel and notifications/message).
MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_log (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_prmt (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_prmt_listchg (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_tool (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_tool_listchg (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_rsc (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_rsc_subscr (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_rsc_listchg (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_capa_enable_complt (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_add_tool (mcpc_server_t *sv, mcpc_tool_t *tool);

MCPC_API mcpc_errcode_t
mcpc_server_add_prmpt (mcpc_server_t *sv, mcpc_prmpt_t *prmpt);

MCPC_API mcpc_errcode_t
mcpc_server_add_rsc (mcpc_server_t *sv, mcpc_rsc_t *rsc);

MCPC_API mcpc_errcode_t
mcpc_server_add_rsctpl (mcpc_server_t *sv, mcpc_rsctpl_t *tpl);

// Tell clients the set of tools/prompts/resources changed. They fail with MCPC_EC_CAPA_OFF
// unless the matching *_listchg capability is enabled. Safe to call from any thread, including
// from inside a callback.
MCPC_API mcpc_errcode_t
mcpc_server_notify_tools_list_changed (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_notify_prmpts_list_changed (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_notify_rscs_list_changed (mcpc_server_t *sv);

// Tell the clients subscribed to uri (via resources/subscribe) that it changed. Needs
// mcpc_server_capa_enable_rsc_subscr.
MCPC_API mcpc_errcode_t
mcpc_server_notify_rsc_updated (mcpc_server_t *sv, const char8_t *urint);

MCPC_API mcpc_errcode_t
mcpc_server_start (mcpc_server_t *sv);

MCPC_API mcpc_errcode_t
mcpc_server_close (mcpc_server_t *sv);

#endif
