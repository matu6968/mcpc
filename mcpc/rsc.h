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

#ifndef h_pub_rsc
#define h_pub_rsc

#include "_c23_uchar.h"
#include "visb.h"

#include "anydata.h"
#include "errcode.h"
#include "ucbr.h"
#include "ctx.h"

typedef mcpc_anydata_t mcpc_rsc_t;

MCPC_API mcpc_rsc_t *
mcpc_rsc_new (const char8_t *uri, const size_t uri_len, const char8_t *name, const size_t name_len);

MCPC_API mcpc_rsc_t *
mcpc_rsc_new2 (const char8_t *uri_nt, const char8_t *nament);

MCPC_API mcpc_rsc_t *
mcpc_rsc_new3 (mcpc_mime_t mime, const char8_t *uri_nt, const char8_t *nament);

MCPC_API void
mcpc_rsc_free (mcpc_rsc_t *rsc);

MCPC_API mcpc_errcode_t
mcpc_rsc_get_uri (const mcpc_rsc_t *rsc, char8_t *ret, size_t ret_cap, size_t *ret_len);

MCPC_API mcpc_errcode_t
mcpc_rsc_getref_uri (const mcpc_rsc_t *rsc, const char8_t **ret, size_t *ret_len);

MCPC_API mcpc_errcode_t
mcpc_rsc_getref_name (const mcpc_rsc_t *rsc, const char8_t **ret, size_t *ret_len);

MCPC_API mcpc_errcode_t
mcpc_rsc_get_mime (const mcpc_rsc_t *rsc, mcpc_mime_t *ret);

MCPC_API void
mcpc_rsc_set_mime (mcpc_rsc_t *rsc, mcpc_mime_t mime);

// Produces the contents of the resource in answer to resources/read. uri is what the client
// asked for (for a template, the expanded one). Add results with mcpc_ucbr_rscread_add_text
// or mcpc_ucbr_rscread_add_blob; a callback that adds nothing answers with empty contents.
typedef void (*mcpc_rcallcb_t) (const mcpc_rsc_t *rsc, const char8_t *uri, size_t uri_len, mcpc_ucbr_t *ucbr);

MCPC_API mcpc_errcode_t
mcpc_rsc_set_read_cb (mcpc_rsc_t *rsc, mcpc_rcallcb_t cb);

// Like mcpc_rcallcb_t with the request context, which lets the callback ask the client for input
// (see mcpc_ctx_elicit_form and the multi round-trip notes in mcpc/ctx.h). Takes precedence.
typedef void (*mcpc_rcallcb2_t) (const mcpc_rsc_t *rsc, mcpc_ctx_t *ctx, const char8_t *uri, size_t uri_len, mcpc_ucbr_t *ucbr);

MCPC_API mcpc_errcode_t
mcpc_rsc_set_read_cb2 (mcpc_rsc_t *rsc, mcpc_rcallcb2_t cb);

MCPC_API mcpc_errcode_t
mcpc_rsc_set_description (mcpc_rsc_t *rsc, const char8_t *descnt);

// Mime type as text, for types the mcpc_mime_t list lacks (e.g. "text/markdown"). Wins over set_mime.
MCPC_API mcpc_errcode_t
mcpc_rsc_set_mime_str (mcpc_rsc_t *rsc, const char8_t *miment);

// Size of the raw content in bytes, if known.
MCPC_API void
mcpc_rsc_set_size (mcpc_rsc_t *rsc, size_t bytes);

#define mcpc_rsc_set_title mcpc_anydata_set_title
#define mcpc_rsc_set_icons_json mcpc_anydata_set_icons_json
#define mcpc_rsc_set_meta_json mcpc_anydata_set_meta_json
#define mcpc_rsc_set_annotations_json mcpc_anydata_set_annotations_json

// ---- resource templates -----------------------------------------------------------
//
// A family of resources addressed by an RFC 6570 URI template. {name} matches one path
// segment and {+name} matches across '/'; other template operators are not supported.

typedef mcpc_anydata_t mcpc_rsctpl_t;

MCPC_API mcpc_rsctpl_t *
mcpc_rsctpl_new (const char8_t *uri_template_nt, const char8_t *name_nt);

MCPC_API void
mcpc_rsctpl_free (mcpc_rsctpl_t *tpl);

MCPC_API mcpc_errcode_t
mcpc_rsctpl_set_read_cb (mcpc_rsctpl_t *tpl, mcpc_rcallcb_t cb);

MCPC_API mcpc_errcode_t
mcpc_rsctpl_set_read_cb2 (mcpc_rsctpl_t *tpl, mcpc_rcallcb2_t cb);

MCPC_API mcpc_errcode_t
mcpc_rsctpl_set_description (mcpc_rsctpl_t *tpl, const char8_t *descnt);

MCPC_API mcpc_errcode_t
mcpc_rsctpl_set_mime_str (mcpc_rsctpl_t *tpl, const char8_t *miment);

#define mcpc_rsctpl_set_title mcpc_anydata_set_title
#define mcpc_rsctpl_set_icons_json mcpc_anydata_set_icons_json
#define mcpc_rsctpl_set_meta_json mcpc_anydata_set_meta_json
#define mcpc_rsctpl_set_annotations_json mcpc_anydata_set_annotations_json

// Value of a template variable in a URI the template matched (UTF-8 text, NUL-terminated when it fits).
MCPC_API mcpc_errcode_t
mcpc_rsctpl_get_var (const mcpc_rsctpl_t *tpl, const char8_t *uri, size_t uri_len, const char8_t *var_nt,
		     char8_t *ret, size_t ret_cap, size_t *ret_len);

#endif
