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

#define __TU__ "serlz"

#include "serlz.h"

#include "anydata.h"

#include "ext_string.h"
#include "log.h"


/* = = = = = = = = = = = = = = = = = tool = = = = = = = = = = = = = = = = = */

// Optional descriptive members, each emitted only when the negotiated revision defines it.
// annot_feat is the feature that introduced annotations on this kind of object (0 = always there,
// MCPC_FEAT_NEVER = none). Every emitted member is preceded by a comma.
#define MCPC_FEAT_NEVER 0x80000000u
static void
_serlz_extras (retbuf_t rtbf, const mcpc_anydata_t *d, unsigned annot_feat)
{
  const char *v;

  if ((rtbf->feat & MCPC_FEAT_TITLE) && (v = mcpc_anydata_xget (d, MCPC_X_TITLE)) != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "title", v);
  if ((rtbf->feat & MCPC_FEAT_ICONS) && (v = mcpc_anydata_xget (d, MCPC_X_ICONS)) != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%s"), "icons", v);
  if (annot_feat != MCPC_FEAT_NEVER && (annot_feat == 0 || (rtbf->feat & annot_feat)) &&
      (v = mcpc_anydata_xget (d, MCPC_X_ANNOT)) != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%s"), "annotations", v);
  if ((rtbf->feat & MCPC_FEAT_META) && (v = mcpc_anydata_xget (d, MCPC_X_META)) != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%s"), "_meta", v);
}

u8_t
serlz_tool_inputschema_properties_prop (retbuf_t rtbf, const mcpc_toolprop_t *tprop)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));

  const char8_t *name, *desc = nullptr;
  size_t name_len, desc_len = 0;

  bug_if_nz (mcpc_toolprop_getref_name (tprop, &name, &name_len));
  bug_if_nz (mcpc_toolprop_getref_desc (tprop, &desc, &desc_len));

  mcpc_anytype_t typ = MCPC_NONE;
  bug_if_nz (mcpc_toolprop_get_data_type (tprop, &typ));

  // MCPC_JSON takes any JSON value, so it gets no "type" member
  const char *jtype = nullptr;
  if (typ == MCPC_BOOL)
    jtype = "boolean";
  else if (typ == MCPC_F64)
    jtype = "number";
  else if (mcpc_anytype_is_intlike (typ))
    jtype = "integer";
  else if (typ == MCPC_U8STR)
    jtype = "string";

  if (jtype != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%Q,"), "type", jtype);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%.*Q"), "title", name_len, name);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%.*Q"), "description", desc_len, desc);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

u8_t
serlz_tool_inputschema_properties (retbuf_t rtbf, const mcpc_toolproppool_t *tppool)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));

  const size_t n = tppool != nullptr ? mcpc_toolproppool_get_len (tppool) : 0;
  for (size_t i = 0; i < n; i++)
    {
      const mcpc_toolprop_t *tprop = mcpc_toolproppool_get (tppool, i);
      size_t name_len = 0;
      const char8_t *name = nullptr;
      bug_if (mcpc_toolprop_getref_name (tprop, &name, &name_len));

      if (i != 0)
	rtbuf_printf_mjfmt (rtbf, u8c1 (","));
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%.*Q:"), name_len, name);
      serlz_tool_inputschema_properties_prop (rtbf, tprop);
    }

  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

u8_t
serlz_tool_inputschema (retbuf_t rtbf, const mcpc_tool_t *tool)
{
  // a hand written schema wins over the one derived from the properties
  const char *raw = mcpc_anydata_xget (tool, MCPC_X_INSCHEMA);
  if (raw != nullptr)
    {
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%s"), raw);
      return 0;
    }

  const mcpc_toolproppool_t *tppool = mcpc_tool_get_toolproppool (tool);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%Q"), "type", "object");
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "properties");
  serlz_tool_inputschema_properties (rtbf, tppool);

  const size_t n = tppool != nullptr ? mcpc_toolproppool_get_len (tppool) : 0;
  bool first = true;
  for (size_t i = 0; i < n; i++)
    {
      const mcpc_toolprop_t *tprop = mcpc_toolproppool_get (tppool, i);
      if (!(tprop->flags & ANYDATA_FL_REQUIRED))
	continue;
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%s"), first ? ",\"required\":[" : ",");
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%.*Q"), (int) tprop->meta1_len, tprop->meta1);
      first = false;
    }
  if (!first)
    rtbuf_printf_mjfmt (rtbf, u8c1 ("]"));

  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

uint8_t
serlz_tool (retbuf_t rtbf, const mcpc_tool_t *tool)
{
  const char8_t *name, *desc = nullptr;
  size_t name_len, desc_len = 0;
  bug_if_nz (mcpc_toolprop_getref_name (tool, &name, &name_len));
  bug_if_nz (mcpc_toolprop_getref_desc (tool, &desc, &desc_len));

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%.*Q"), "name", name_len, name);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%.*Q"), "description", desc_len, desc);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "inputSchema");
  serlz_tool_inputschema (rtbf, tool);

  const char *outschema = mcpc_anydata_xget (tool, MCPC_X_OUTSCHEMA);
  if ((rtbf->feat & MCPC_FEAT_STRUCTURED) && outschema != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%s"), "outputSchema", outschema);

  _serlz_extras (rtbf, tool, MCPC_FEAT_TOOL_ANNOT);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

// Items [offset, end) of a list of total entries, limit 0 means "all of them".
static size_t
_page_end (size_t total, size_t offset, size_t limit)
{
  if (offset > total)
    offset = total;
  if (limit == 0 || limit > total - offset)
    return total;
  return offset + limit;
}

// ttlMs and cacheScope of a cacheable result, 2026-07-28 only
static void
_serlz_cache (retbuf_t rtbf)
{
  if (rtbf->feat & MCPC_FEAT_MODERN)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%lu,%Q:%Q"), "ttlMs", rtbf->ttl_ms, "cacheScope",
			rtbf->cache_public ? "public" : "private");
}

void
_serlz_cache_public (retbuf_t rtbf)
{
  _serlz_cache (rtbf);
}

// closes a list result: the optional nextCursor member when more items follow
static void
_serlz_next_cursor (retbuf_t rtbf, size_t total, size_t end)
{
  if (end < total)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:\"%lu\""), "nextCursor", (unsigned long) end);
}

u8_t
serlz_toolpool (retbuf_t rtbf, const mcpc_toolpool_t *toolpool, size_t offset, size_t limit)
{
  const size_t total = toolpool != nullptr ? mcpc_toolpool_get_len (toolpool) : 0;
  const size_t end = _page_end (total, offset, limit);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "tools");
  rtbuf_printf_mjfmt (rtbf, u8c1 ("["));

  for (size_t i = offset; i < end; i++)
    {
      if (i != offset)
	rtbuf_printf_mjfmt (rtbf, u8c1 (","));
      serlz_tool (rtbf, mcpc_toolpool_get (toolpool, i));
    }

  rtbuf_printf_mjfmt (rtbf, u8c1 ("]"));
  _serlz_next_cursor (rtbf, total, end);
  _serlz_cache (rtbf);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

/* = = = = = = = = = = = = = = = = = prmpt = = = = = = = = = = = = = = = = = */

u8_t
serlz_prmptarg (retbuf_t rtbf, const mcpc_prmptarg_t *prmptarg)
{
  const char8_t *name, *desc = nullptr;
  size_t name_len, desc_len = 0;
  bool required = false;

  bug_if_nz (mcpc_prmptarg_getref_name (prmptarg, &name, &name_len));
  bug_if_nz (mcpc_prmptarg_getref_desc (prmptarg, &desc, &desc_len));
  required = mcpc_prmptarg_is_required (prmptarg);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));

  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%.*Q"), "name", name_len, name);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%.*Q"), "description", desc_len, desc);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%B"), "required", required);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

uint8_t
serlz_prmpt (retbuf_t rtbf, const mcpc_prmpt_t *prmpt)
{
  const char8_t *name, *desc = nullptr;
  size_t name_len, desc_len = 0;
  bug_if_nz (mcpc_prmptarg_getref_name (prmpt, &name, &name_len));
  bug_if_nz (mcpc_prmptarg_getref_desc (prmpt, &desc, &desc_len));

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%.*Q"), "name", name_len, name);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%.*Q"), "description", desc_len, desc);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "arguments");

  rtbuf_printf_mjfmt (rtbf, u8c1 ("["));

  const size_t n_args = prmpt->chd_pool != nullptr ? prmpt->chd_pool->len : 0;
  for (size_t i = 0; i < n_args; i++)
    {
      if (i != 0)
	rtbuf_printf_mjfmt (rtbf, u8c1 (","));
      serlz_prmptarg (rtbf, mcpc_anydata_get_child (prmpt, i));
    }

  rtbuf_printf_mjfmt (rtbf, u8c1 ("]"));

  _serlz_extras (rtbf, prmpt, MCPC_FEAT_NEVER);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

u8_t
serlz_prmptpool (retbuf_t rtbf, const mcpc_prmptpool_t *prmptpool, size_t offset, size_t limit)
{
  const size_t total = prmptpool != nullptr ? mcpc_prmptpool_get_len (prmptpool) : 0;
  const size_t end = _page_end (total, offset, limit);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "prompts");
  rtbuf_printf_mjfmt (rtbf, u8c1 ("["));

  for (size_t i = offset; i < end; i++)
    {
      if (i != offset)
	rtbuf_printf_mjfmt (rtbf, u8c1 (","));
      serlz_prmpt (rtbf, mcpc_prmptpool_get (prmptpool, i));
    }

  rtbuf_printf_mjfmt (rtbf, u8c1 ("]"));
  _serlz_next_cursor (rtbf, total, end);
  _serlz_cache (rtbf);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

/* = = = = = = = = = = = = = = = = = = rsc = = = = = = = = = = = = = = = = = = */

u8_t
serlz_rsc (retbuf_t rtbf, const mcpc_rsc_t *rsc)
{
  const char8_t *ref_uri = nullptr;
  size_t ref_uri_len = 0;
  const char8_t *ref_name = nullptr;
  size_t ref_name_len = 0;
  mcpc_rsc_getref_uri (rsc, &ref_uri, &ref_uri_len);
  mcpc_rsc_getref_name (rsc, &ref_name, &ref_name_len);

  // uri is the ID, hence first, and name is required by the schema
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:%.*Q,%Q:%.*Q"), "uri", (int) ref_uri_len,
		      ref_uri != nullptr ? (const char *) ref_uri : "", "name", (int) ref_name_len,
		      ref_name != nullptr ? (const char *) ref_name : "");

  const char *desc = mcpc_anydata_xget (rsc, MCPC_X_DESC);
  if (desc != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "description", desc);

  const char *mime = mcpc_rsc_mime_cstr (rsc);
  if (mime != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "mimeType", mime);

  if (rsc->extra != nullptr && rsc->extra->has_size)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%lu"), "size", (unsigned long) rsc->extra->size);

  // resources have carried annotations since the first revision
  _serlz_extras (rtbf, rsc, 0);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

u8_t
serlz_rsctpl (retbuf_t rtbf, const mcpc_rsctpl_t *tpl)
{
  const char *uritpl = mcpc_anydata_xget (tpl, MCPC_X_URITPL);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:%Q,%Q:%.*Q"), "uriTemplate", uritpl != nullptr ? uritpl : "", "name",
		      (int) tpl->meta1_len, tpl->meta1 != nullptr ? (const char *) tpl->meta1 : "");

  const char *desc = mcpc_anydata_xget (tpl, MCPC_X_DESC);
  if (desc != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "description", desc);
  const char *mime = mcpc_anydata_xget (tpl, MCPC_X_MIME);
  if (mime != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "mimeType", mime);

  _serlz_extras (rtbf, tpl, 0);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

u8_t
serlz_rsctplpool (retbuf_t rtbf, const mcpc_rsctplpool_t *pool, size_t offset, size_t limit)
{
  const size_t total = pool != nullptr ? pool->len : 0;
  const size_t end = _page_end (total, offset, limit);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:["), "resourceTemplates");
  for (size_t i = offset; i < end; i++)
    {
      if (i != offset)
	rtbuf_printf_mjfmt (rtbf, u8c1 (","));
      serlz_rsctpl (rtbf, pool->head + i);
    }
  rtbuf_printf_mjfmt (rtbf, u8c1 ("]"));
  _serlz_next_cursor (rtbf, total, end);
  _serlz_cache (rtbf);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

u8_t
serlz_rscpool (retbuf_t rtbf, const mcpc_rscpool_t *rscpool, size_t offset, size_t limit)
{
  const size_t total = rscpool != nullptr ? mcpc_rscpool_get_len (rscpool) : 0;
  const size_t end = _page_end (total, offset, limit);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "resources");
  rtbuf_printf_mjfmt (rtbf, u8c1 ("["));

  for (size_t i = offset; i < end; i++)
    {
      if (i != offset)
	rtbuf_printf_mjfmt (rtbf, u8c1 (","));
      serlz_rsc (rtbf, mcpc_rscpool_get (rscpool, i));
    }

  rtbuf_printf_mjfmt (rtbf, u8c1 ("]"));
  _serlz_next_cursor (rtbf, total, end);
  _serlz_cache (rtbf);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

/* = = = = = = = = = = = = = = = = = = = - = = = = = = = = = = = = = = = = = = = */

uint8_t
serlz_serverInfo (retbuf_t rtbf, const mcpc_server_t *sv)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%.*Q"), "name", sv->name_len, sv->name);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%.*Q"), "version", sv->ver_len, sv->ver);

  if ((rtbf->feat & MCPC_FEAT_TITLE) && sv->title != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "title", sv->title);
  if ((rtbf->feat & MCPC_FEAT_IMPL_DESC) && sv->desc != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "description", sv->desc);
  if ((rtbf->feat & MCPC_FEAT_ICONS) && sv->icons_json != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%s"), "icons", sv->icons_json);
  if ((rtbf->feat & MCPC_FEAT_ICONS) && sv->website != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "websiteUrl", sv->website);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

uint8_t
serlz_capasprompts (retbuf_t rtbf, const mcpc_server_t *sv)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "listChanged");
  bool supp_lschg = mcpc_server_supp_prompts_lschg (sv);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%B"), supp_lschg);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

uint8_t
serlz_capastools (retbuf_t rtbf, const mcpc_server_t *sv)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "listChanged");
  bool supp_lschg = mcpc_server_supp_tools_lschg (sv);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%B"), supp_lschg);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

uint8_t
serlz_capasresources (retbuf_t rtbf, const mcpc_server_t *sv)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "subscribe");
  bool supp_subs = mcpc_server_supp_rscs_subs (sv);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%B"), supp_subs);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "listChanged");
  bool supp_lschg = mcpc_server_supp_rscs_lschg (sv);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%B"), supp_lschg);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

uint8_t
serlz_capas (retbuf_t rtbf, const mcpc_server_t *sv)
{
  // members are comma-separated, so emit the separator before every member but the first
  const char *sep = "";

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));

  if (mcpc_server_supp_prompts (sv))
    {
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%s%Q:"), sep, "prompts");
      serlz_capasprompts (rtbf, sv);
      sep = ",";
    }

  if (mcpc_server_supp_tools (sv))
    {
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%s%Q:"), sep, "tools");
      serlz_capastools (rtbf, sv);
      sep = ",";
    }

  if (mcpc_server_supp_rscs (sv))
    {
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%s%Q:"), sep, "resources");
      serlz_capasresources (rtbf, sv);
      sep = ",";
    }

  if (mcpc_server_supp_complt (sv))
    {
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%s%Q:{}"), sep, "completions");
      sep = ",";
    }

  if (mcpc_server_supp_log (sv))
    {
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%s%Q:{}"), sep, "logging");
      sep = ",";
    }

  if ((rtbf->feat & MCPC_FEAT_MODERN) && sv->extensions_json != nullptr)
    {
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%s%Q:%s"), sep, "extensions", sv->extensions_json);
      sep = ",";
    }

  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

uint8_t
serlz_initres (retbuf_t rtbf, const mcpc_server_t *sv, mcpc_pver_t pver)
{
  rtbf->feat = mcpc_pver_feats (pver);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%Q"), "protocolVersion", mcpc_pver_str (pver));
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "serverInfo");
  serlz_serverInfo (rtbf, sv);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "capabilities");
  serlz_capas (rtbf, sv);
  if (sv->instructions != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "instructions", sv->instructions);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

/* = = = = = = = = = = = = = = = = = ucbr = = = = = = = = = = = = = = = = = */

// bytes of a media or blob entry, never NULL
static const char *
_chd_bytes (const mcpc_ucbrchd_t *d, size_t *len)
{
  *len = d->data_len;
  return d->data_arrlk != nullptr ? (const char *) d->data_arrlk : "";
}

// {"uri","mimeType","text"|"blob"} of an embedded or read resource
static void
_serlz_res_contents (retbuf_t rtbf, const mcpc_ucbrchd_t *d, const char *dflt_uri, const char *dflt_mime)
{
  const char *uri = mcpc_anydata_xget (d, MCPC_X_URI);
  const char *mime = mcpc_anydata_xget (d, MCPC_X_MIME);
  if (uri == nullptr)
    uri = dflt_uri != nullptr ? dflt_uri : "";
  if (mime == nullptr)
    mime = dflt_mime;

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:%Q"), "uri", uri);
  if (mime != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "mimeType", mime);

  if (d->ckind == MCPC_CK_RES_BLOB)
    {
      size_t n = 0;
      const char *p = _chd_bytes (d, &n);
      rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%V"), "blob", (int) n, p);
    }
  else
    {
      const char8_t *t = nullptr;
      size_t t_len = 0;
      mcpc_ucbrchd_getref_u8str (d, &t, &t_len);
      rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%.*Q"), "text", (int) t_len, t != nullptr ? (const char *) t : "");
    }
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
}

// one entry of a tool result "content" array, degraded to text where the revision lacks the type
static void
_serlz_tool_block (retbuf_t rtbf, const mcpc_ucbrchd_t *d)
{
  const char *mime = mcpc_anydata_xget (d, MCPC_X_MIME);
  const char *uri = mcpc_anydata_xget (d, MCPC_X_URI);

  switch ((mcpc_ckind_t) d->ckind)
    {
    case MCPC_CK_IMAGE:
    case MCPC_CK_AUDIO:
      {
	const bool audio = d->ckind == MCPC_CK_AUDIO;
	if (audio && !(rtbf->feat & MCPC_FEAT_AUDIO))
	  {
	    rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:%Q,%Q:%Q}"), "type", "text", "text",
				"[audio omitted: not supported by this protocol version]");
	    break;
	  }
	size_t n = 0;
	const char *p = _chd_bytes (d, &n);
	rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:%Q,%Q:%V,%Q:%Q}"), "type", audio ? "audio" : "image", "data", (int) n, p,
			    "mimeType", mime != nullptr ? mime : "application/octet-stream");
	break;
      }

    case MCPC_CK_RESLINK:
      {
	const char *name = d->meta1_len > 0 ? (const char *) d->meta1 : uri;
	const int name_len = d->meta1_len > 0 ? (int) d->meta1_len : (uri != nullptr ? (int) strlen (uri) : 0);
	if (!(rtbf->feat & MCPC_FEAT_RESLINK))
	  {
	    // older clients only know text, so keep the pointer readable
	    rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:%Q,%Q:"), "type", "text", "text");
	    rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q}"), uri != nullptr ? uri : "");
	    break;
	  }
	rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:%Q,%Q:%Q,%Q:%.*Q"), "type", "resource_link", "uri", uri != nullptr ? uri : "",
			    "name", name_len, name != nullptr ? name : "");
	const char *desc = mcpc_anydata_xget (d, MCPC_X_DESC);
	if (desc != nullptr)
	  rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "description", desc);
	if (mime != nullptr)
	  rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%Q"), "mimeType", mime);
	rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
	break;
      }

    case MCPC_CK_RES_TEXT:
    case MCPC_CK_RES_BLOB:
      rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:%Q,%Q:"), "type", "resource", "resource");
      _serlz_res_contents (rtbf, d, nullptr, nullptr);
      rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
      break;

    default:			// plain text
      {
	const char8_t *s = nullptr;
	size_t s_len = 0;
	mcpc_ucbrchd_getref_u8str (d, &s, &s_len);
	rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:%Q,%Q:%.*Q}"), "type", "text", "text", (int) s_len,
			    s != nullptr ? (const char *) s : "");
	break;
      }
    }
}

uint8_t
serlz_ucbrchd_tool_callres (retbuf_t rtbf, const mcpc_ucbrchd_t *ucbrchd)
{
  _serlz_tool_block (rtbf, ucbrchd);
  return 0;
}

uint8_t
serlz_ucbrchd_prmpt_callres (retbuf_t rtbf, const mcpc_ucbrchd_t *ucbrchd)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:%Q,%Q:"), "role", mcpc_ucbrchd_is_assist_prmpt (ucbrchd) ? "assistant" : "user", "content");
  // text, image, audio, embedded resource or link: the same content blocks tool results use
  _serlz_tool_block (rtbf, ucbrchd);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

uint8_t
serlz_ucbrchd (retbuf_t rtbf, const mcpc_ucbrchd_t *ucbrchd)
{
  if (false);
  else if (mcpc_ucbrchd_is_tool_callres (ucbrchd))
    {
      return serlz_ucbrchd_tool_callres (rtbf, ucbrchd);
    }
  else if (mcpc_ucbrchd_is_prmpt_callres (ucbrchd))
    {
      return serlz_ucbrchd_prmpt_callres (rtbf, ucbrchd);
    }
  return 0;
}

u8_t
serlz_ucbr (retbuf_t rtbf, const mcpc_ucbr_t *ucbr)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("["));

  for (size_t i = 0; i < ucbr->len; i++)
    {
      if (i != 0)
	rtbuf_printf_mjfmt (rtbf, u8c1 (","));
      serlz_ucbrchd (rtbf, ucbr->head + i);
    }

  rtbuf_printf_mjfmt (rtbf, u8c1 ("]"));
  return 0;
}

u8_t
serlz_tool_callres (retbuf_t rtbf, const mcpc_ucbr_t *ucbr)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));

  if (mcpc_ucbr_is_err (ucbr))
    {
      // https://modelcontextprotocol.io/docs/concepts/tools#error-handling-2
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%B"), "isError", true);
      rtbuf_printf_mjfmt (rtbf, u8c1 (","));
    }

  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:["), "content");
  bool first = true;
  const mcpc_anydata_t *structured = nullptr;
  const mcpc_anydata_t *meta = nullptr;
  for (size_t i = 0; i < ucbr->len; i++)
    {
      const mcpc_anydata_t *d = ucbr->head + i;
      if (d->ckind == MCPC_CK_STRUCTURED)
	{
	  structured = d;
	  continue;
	}
      if (d->ckind == MCPC_CK_RESMETA)
	{
	  meta = d;
	  continue;
	}
      if (!first)
	rtbuf_printf_mjfmt (rtbf, u8c1 (","));
      _serlz_tool_block (rtbf, d);
      first = false;
    }
  rtbuf_printf_mjfmt (rtbf, u8c1 ("]"));

  // revisions before structured output only see the text copy set_structured_json adds
  if (structured != nullptr && (rtbf->feat & MCPC_FEAT_STRUCTURED))
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%.*s"), "structuredContent", (int) structured->data_len,
			(const char *) structured->data_arrlk);
  if (meta != nullptr && (rtbf->feat & MCPC_FEAT_META))
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%.*s"), "_meta", (int) meta->data_len, (const char *) meta->data_arrlk);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

u8_t
serlz_rscread_res (retbuf_t rtbf, const mcpc_ucbr_t *ucbr, const char *req_uri, const char *dflt_mime)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{%Q:["), "contents");
  bool first = true;
  const mcpc_anydata_t *meta = nullptr;
  for (size_t i = 0; i < ucbr->len; i++)
    {
      const mcpc_anydata_t *d = ucbr->head + i;
      if (d->ckind == MCPC_CK_RESMETA)
	{
	  meta = d;
	  continue;
	}
      if (d->ckind != MCPC_CK_RES_TEXT && d->ckind != MCPC_CK_RES_BLOB)
	continue;
      if (!first)
	rtbuf_printf_mjfmt (rtbf, u8c1 (","));
      _serlz_res_contents (rtbf, d, req_uri, dflt_mime);
      first = false;
    }
  rtbuf_printf_mjfmt (rtbf, u8c1 ("]"));
  if (meta != nullptr && (rtbf->feat & MCPC_FEAT_META))
    rtbuf_printf_mjfmt (rtbf, u8c1 (",%Q:%.*s"), "_meta", (int) meta->data_len, (const char *) meta->data_arrlk);
  _serlz_cache (rtbf);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

u8_t
serlz_prmpt_callres (retbuf_t rtbf, const mcpc_ucbr_t *ucbr, const char *desc, size_t desc_len)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));

  if (desc != nullptr && desc_len > 0)
    rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%.*Q,"), "description", (int) desc_len, desc);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "messages");
  serlz_ucbr (rtbf, ucbr);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

// The body of an InputRequiredResult: the questions recorded by the callback and the state to echo back.
u8_t
serlz_input_required (retbuf_t rtbf, const struct mcpc_ctx *ctx)
{
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  const char *sep = "";
  if (ctx->n_inreqs > 0)
    {
      rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:{"), "inputRequests");
      for (size_t i = 0; i < ctx->n_inreqs; i++)
	rtbuf_printf_mjfmt (rtbf, u8c1 ("%s%Q:{%Q:%Q,%Q:%s}"), i ? "," : "", ctx->inreqs[i].key, "method",
			    ctx->inreqs[i].method, "params", ctx->inreqs[i].params);
      rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
      sep = ",";
    }
  if (ctx->state_out != nullptr)
    rtbuf_printf_mjfmt (rtbf, u8c1 ("%s%Q:%Q"), sep, "requestState", ctx->state_out);
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}

/* = = = = = = = = = = = = = = = = complt = = = = = = = = = = = = = = = = */

u8_t
serlz_complt (retbuf_t rtbf, const mcpc_complt_t *complt)
{
  size_t total = 0;

  mcpc_complt_get_compltcandi_len (complt, &total);

  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "completion");
  rtbuf_printf_mjfmt (rtbf, u8c1 ("{"));

  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%lu"), "total", total);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:%B"), "hasMore", false);
  rtbuf_printf_mjfmt (rtbf, u8c1 (","));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("%Q:"), "values");
  rtbuf_printf_mjfmt (rtbf, u8c1 ("["));

  const mcpc_compltcand_t *cur_compltcand = nullptr;
  bool first = true;
  while ((cur_compltcand = mcpc_complt_get_next_compltcand (complt)))
    {
      if (!first)
	rtbuf_printf_mjfmt (rtbf, u8c1 (","));
      const char8_t *sbuf = nullptr;
      size_t sbuf_len = 0;
      bug_if_nz (mcpc_compltcand_getref_u8str (cur_compltcand, &sbuf, &sbuf_len));
      bug_if_nullptr (sbuf);
      bug_if_zero (sbuf_len);

      rtbuf_printf_mjfmt (rtbf, u8c1 ("%.*Q"), sbuf_len, sbuf);
      first = false;
    }

  rtbuf_printf_mjfmt (rtbf, u8c1 ("]"));


  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  rtbuf_printf_mjfmt (rtbf, u8c1 ("}"));
  return 0;
}
