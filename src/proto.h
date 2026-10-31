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

#ifndef h_proto
#define h_proto

#include <stddef.h>

#include <mcpc/_c23_keywords.h>

// Protocol revisions the server can speak. The values are ordered, so a later
// revision compares greater. MCPC_PV_NONE means "not negotiated yet".
enum mcpc_pver
{
  MCPC_PV_NONE = 0,
  MCPC_PV_2024_11_05,
  MCPC_PV_2025_03_26,
  MCPC_PV_2025_06_18,
  MCPC_PV_2025_11_25,
  MCPC_PV_2026_07_28,		// stateless: per-request _meta, no initialize
};
typedef enum mcpc_pver mcpc_pver_t;

// newest revision spoken through the initialize handshake
#define MCPC_PV_LATEST_LEGACY MCPC_PV_2025_11_25

// Wire features, each one introduced by a revision. Serializers test these
// instead of comparing revisions, so a field is gated in exactly one place.
enum mcpc_feat
{
  MCPC_FEAT_TOOL_ANNOT = 1u << 0,	// tool annotations, 2025-03-26
  MCPC_FEAT_AUDIO = 1u << 1,	// audio content, 2025-03-26
  MCPC_FEAT_PROGRESS_MSG = 1u << 2,	// progress notification message, 2025-03-26
  MCPC_FEAT_TITLE = 1u << 3,	// title on everything, 2025-06-18
  MCPC_FEAT_STRUCTURED = 1u << 4,	// outputSchema, structuredContent, 2025-06-18
  MCPC_FEAT_RESLINK = 1u << 5,	// resource_link content, 2025-06-18
  MCPC_FEAT_ELICIT = 1u << 6,	// elicitation, 2025-06-18
  MCPC_FEAT_META = 1u << 7,	// _meta on most types, 2025-06-18
  MCPC_FEAT_ICONS = 1u << 8,	// icons, 2025-11-25
  MCPC_FEAT_ELICIT_URL = 1u << 9,	// url-mode elicitation, 2025-11-25
  MCPC_FEAT_SAMPLING_TOOLS = 1u << 10,	// tools in sampling, 2025-11-25
  MCPC_FEAT_IMPL_DESC = 1u << 11,	// description on Implementation, 2025-11-25
  MCPC_FEAT_MODERN = 1u << 12,	// 2026-07-28: resultType, cache hints, serverInfo in _meta, MRTR, listen
};

// bit of a revision in a protocol mask, see MCPC_PROTO_* in mcpc/server.h
unsigned mcpc_pver_bit (mcpc_pver_t v);

static inline bool
mcpc_pver_is_modern (mcpc_pver_t v)
{
  return v >= MCPC_PV_2026_07_28;
}

const char *mcpc_pver_str (mcpc_pver_t v);

// Exact match of a version string, MCPC_PV_NONE when unknown.
mcpc_pver_t mcpc_pver_parse (const char *s, size_t len);

// Version to answer an initialize request with: the client's if it is a legacy revision we
// speak, otherwise the newest legacy one (a modern revision is never reached through initialize).
mcpc_pver_t mcpc_pver_negotiate (const char *s, size_t len);

unsigned mcpc_pver_feats (mcpc_pver_t v);

#endif
