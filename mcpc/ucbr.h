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

#ifndef h_pub_ucbr
#define h_pub_ucbr

#include <stdarg.h>
#include <stddef.h>

#include "_c23_uchar.h"
#include "visb.h"

#include "anydata.h"
#include "errcode.h"

typedef mcpc_anypool_t mcpc_ucbr_t;

MCPC_API void
mcpc_ucbr_toolcall_add_text_printf8 (mcpc_ucbr_t *ucbr, const char8_t *fmt, ...);

MCPC_API void
mcpc_ucbr_toolcall_add_errmsg_printf8 (mcpc_ucbr_t *ucbr, const char8_t *fmt, ...);

MCPC_API void
mcpc_ucbr_prmptget_add_user_printf8 (mcpc_ucbr_t *ucbr, const char8_t *fmt, ...);

MCPC_API void
mcpc_ucbr_prmptget_add_assist_printf8 (mcpc_ucbr_t *ucbr, const char8_t *fmt, ...);

#define mcpc_toolcall_result_add_text_printf8 mcpc_ucbr_toolcall_add_text_printf8

#define mcpc_toolcall_result_add_errmsg_printf8 mcpc_ucbr_toolcall_add_errmsg_printf8

#define mcpc_prmptget_result_add_user_printf8 mcpc_ucbr_prmptget_add_user_printf8

#define mcpc_prmptget_result_add_assist_printf8 mcpc_ucbr_prmptget_add_assist_printf8

// ---- richer tool results ----------------------------------------------------
//
// Each call appends one content block to the result. Bytes are base64-encoded for you.
// A block a client's protocol revision cannot represent is degraded to text instead
// (audio before 2025-03-26, resource links before 2025-06-18).

MCPC_API void
mcpc_ucbr_toolcall_add_image (mcpc_ucbr_t *ucbr, const void *bytes, size_t len, const char8_t *mime_nt);

MCPC_API void
mcpc_ucbr_toolcall_add_audio (mcpc_ucbr_t *ucbr, const void *bytes, size_t len, const char8_t *mime_nt);

// Points at a resource without embedding it. desc_nt and mime_nt may be NULL.
MCPC_API void
mcpc_ucbr_toolcall_add_resource_link (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *name_nt,
				      const char8_t *desc_nt, const char8_t *mime_nt);

// Embeds a resource's contents in the result.
MCPC_API void
mcpc_ucbr_toolcall_add_embedded_text (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt,
				      const char8_t *text_nt);

MCPC_API void
mcpc_ucbr_toolcall_add_embedded_blob (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt,
				      const void *bytes, size_t len);

// Machine readable result matching the tool's output schema (2025-06-18+). It is also
// added as a text block, which is how clients without structured output read it.
// Returns an error and changes nothing when json_nt is not a JSON value.
MCPC_API mcpc_errcode_t
mcpc_ucbr_toolcall_set_structured_json (mcpc_ucbr_t *ucbr, const char8_t *json_nt);

// _meta object of the result (2025-06-18+)
MCPC_API mcpc_errcode_t
mcpc_ucbr_set_meta_json (mcpc_ucbr_t *ucbr, const char8_t *json_object_nt);

// ---- resources/read results ----------------------------------------------------
//
// uri_nt may be NULL to answer with the URI that was asked for, mime_nt to use the
// resource's own mime type.

MCPC_API void
mcpc_ucbr_rscread_add_text (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt, const char8_t *fmt, ...);

MCPC_API void
mcpc_ucbr_rscread_add_blob (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt, const void *bytes,
			    size_t len);

// ---- richer prompt messages ----------------------------------------------------
//
// Like the text messages above, with other kinds of content. user_* and assist_* choose the role.
// The same degradation applies as for tool results (audio before 2025-03-26, links before 2025-06-18).

MCPC_API void
mcpc_ucbr_prmptget_add_user_image (mcpc_ucbr_t *ucbr, const void *bytes, size_t len, const char8_t *mime_nt);

MCPC_API void
mcpc_ucbr_prmptget_add_assist_image (mcpc_ucbr_t *ucbr, const void *bytes, size_t len, const char8_t *mime_nt);

MCPC_API void
mcpc_ucbr_prmptget_add_user_audio (mcpc_ucbr_t *ucbr, const void *bytes, size_t len, const char8_t *mime_nt);

MCPC_API void
mcpc_ucbr_prmptget_add_assist_audio (mcpc_ucbr_t *ucbr, const void *bytes, size_t len, const char8_t *mime_nt);

MCPC_API void
mcpc_ucbr_prmptget_add_user_embedded_text (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt, const char8_t *text_nt);

MCPC_API void
mcpc_ucbr_prmptget_add_assist_embedded_text (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt, const char8_t *text_nt);

MCPC_API void
mcpc_ucbr_prmptget_add_user_embedded_blob (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt, const void *bytes, size_t len);

MCPC_API void
mcpc_ucbr_prmptget_add_assist_embedded_blob (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt, const void *bytes, size_t len);

MCPC_API void
mcpc_ucbr_prmptget_add_user_resource_link (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *name_nt, const char8_t *desc_nt, const char8_t *mime_nt);

MCPC_API void
mcpc_ucbr_prmptget_add_assist_resource_link (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *name_nt, const char8_t *desc_nt, const char8_t *mime_nt);

#endif
