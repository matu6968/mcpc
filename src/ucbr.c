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

#include "ucbr.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "alloc.h"
#include "anydata.h"
#include "errcode.h"
#include "ext_string.h"
#include "mjson.h"

/* = = = = = = = = = = = = = = = = ucbrchd = = = = = = = = = = = = = = = = */

mcpc_ucbrchd_t *
mcpc_ucbrchd_new_vprintf8 (const char8_t *fmt, va_list *vargs)
{
  size_t sbuf_cap = 1024;
  size_t sbuf_len = 0;
  char8_t *sbuf = mcpc_alloc (sbuf_cap);
  int printed = 0;
  while (true)
    {
      va_list vargs_cp;
      va_copy (vargs_cp, *vargs);
      printed
          = vsnprintf ((char *)sbuf, sbuf_cap, (const char *)fmt, vargs_cp);
      bug_if (printed < 0);
      if ((size_t)printed >= sbuf_cap)
        {
          sbuf_cap += sbuf_cap;
          sbuf = mcpc_realloc (sbuf, sbuf_cap);
        }
      else
        {
          sbuf_len = (size_t)printed;
          break;
        }
    }

  mcpc_anydata_t *anydata = mcpc_anydata_new_typ (MCPC_U8STR);

  if (MCPC_EC_0 != mcpc_anydata_set_u8str (anydata, sbuf, sbuf_len))
    goto badret;

  free (sbuf);
  return (mcpc_ucbrchd_t *)anydata;

badret:
  free (sbuf);
  mcpc_anydata_free (anydata);
  return nullptr;
}

mcpc_errcode_t
mcpc_ucbrchd_getref_u8str (const mcpc_ucbrchd_t *ucbrchd, const char8_t **ret,
                           size_t *ret_len)
{
  return mcpc_anydata_getref_u8str (ucbrchd, ret, ret_len);
}

mcpc_errcode_t
mcpc_ucbrchd_get_data_type (const mcpc_ucbrchd_t *ucbrchd, mcpc_anytype_t *ret)
{
  bug_if_nullptr (ucbrchd);
  bug_if_nullptr (ret);

  *ret = ucbrchd->data_typ;

  return MCPC_EC_0;
}

bool
mcpc_ucbrchd_is_tool_callres (const mcpc_ucbrchd_t *ucbrchd)
{
  return ucbrchd->flags & ANYDATA_FL_USRCBRES_TOOLCALL;
}

bool
mcpc_ucbrchd_is_prmpt_callres (const mcpc_ucbrchd_t *ucbrchd)
{
  return ucbrchd->flags & ANYDATA_FL_USRCBRES_PRMPTCALL;
}

bool
mcpc_ucbrchd_is_assist_prmpt (const mcpc_ucbrchd_t *ucbrchd)
{
  return ucbrchd->flags & ANYDATA_FL_USRCBRES_PRMPTCALL_ASSIST;
}

/* = = = = = = = = = = = = = = = = = ucbr = = = = = = = = = = = = = = = = = */

mcpc_ucbr_t *
mcpc_ucbr_new ()
{
  return mcpc_anypool_new ();
}

void
mcpc_ucbr_free (mcpc_ucbr_t *ucbr)
{
  mcpc_anypool_free (ucbr);
}

void
mcpc_ucbr_addcpy (mcpc_ucbr_t *ucbr, const mcpc_ucbrchd_t *ucbrchd)
{
  mcpc_anypool_addcpy (ucbr, ucbrchd);
}

void
mcpc_ucbr_addfre (mcpc_ucbr_t *ucbr, mcpc_ucbrchd_t *ucbrchd)
{
  mcpc_anypool_addfre (ucbr, ucbrchd);
}

bool
mcpc_ucbr_is_err (const mcpc_ucbr_t *ucbr)
{
  bug_if_nullptr (ucbr);
  return ucbr->flags & ANYPOOL_FL_USRCBRES_TOOLCALL_ERROR;
}

size_t
mcpc_ucbr_get_len (const mcpc_ucbr_t *ucbr)
{
  bug_if_nullptr (ucbr);
  size_t ret = 0;
  ret = ((const mcpc_anypool_t *)ucbr)->len;
  return ret;
}

const mcpc_ucbrchd_t *
mcpc_ucbr_get (const mcpc_ucbr_t *ucbr, size_t idx)
{
  return mcpc_anypool_get (ucbr, idx);
}

const mcpc_ucbrchd_t *
mcpc_ucbr_getbyname (const mcpc_ucbr_t *ucbr, const char8_t *name,
                     size_t name_len)
{
  return mcpc_anypool_getbymeta1 (ucbr, name, name_len);
}

const mcpc_ucbrchd_t *
mcpc_ucbr_get_next (const mcpc_ucbr_t *ucbr)
{
  bug_if_nullptr (ucbr);

  static size_t idx = 0;

  const mcpc_anypool_t *pool = ucbr;
  if (pool == nullptr)
    return nullptr;

  const size_t pool_len = pool->len;
  if (pool_len == 0)
    return nullptr;

  if (idx == pool_len) // TODO  MT-SAFE
    {
      idx = 0;
      return nullptr;
    }

  const mcpc_anydata_t *ret = pool->head + idx;
  idx += 1;

  return ret;
}

size_t
mcpc_ucbr_get_serlz_lenhint (const mcpc_ucbr_t *ucbr)
{
  bug_if_nullptr (ucbr);
  size_t ret = 0;

  for (size_t i = 0; i < ucbr->len; i++)
    ret += ucbr->head[i].data_len;
  ret += ret / 3;		// base64 expansion of binary blocks

  // the buffer grows on demand, this only saves reallocations
  return ret + 1024;
}

void
mcpc_ucbr_toolcallres_add_text_vprintf8 (mcpc_ucbr_t *ucbr, const char8_t *fmt,
                                         va_list *vargs)
{
  bug_if_nullptr (ucbr);
  bug_if_nullptr (fmt);
  bug_if_nullptr (vargs);

  ucbr->flags |= ANYPOOL_FL_USRCBRES_TOOLCALL;

  mcpc_ucbrchd_t *newctn = mcpc_ucbrchd_new_vprintf8 (fmt, vargs);
  newctn->flags |= ANYDATA_FL_USRCBRES_TOOLCALL;
  newctn->flags |= ANYDATA_FL_USRCBRES_TOOLCALL_TXTCTN;
  mcpc_ucbr_addfre (ucbr, newctn);
}

MCPC_API void
mcpc_ucbr_toolcall_add_text_printf8 (mcpc_ucbr_t *ucbr, const char8_t *fmt,
                                     ...)
{
  va_list vargs;
  va_start (vargs, fmt);

  mcpc_ucbr_toolcallres_add_text_vprintf8 (ucbr, fmt, &vargs);

  va_end (vargs);
}

/* = = = = = = = = = = = = = = = = rich results = = = = = = = = = = = = = = = = */

// who a result entry belongs to: a tool result, a prompt message of either role, or neither (resources/read)
enum chd_owner
{
  CHD_NONE = 0,
  CHD_TOOL,
  CHD_PRMPT_USER,
  CHD_PRMPT_ASSIST,
};

static mcpc_ucbrchd_t *
_chd_new (mcpc_ckind_t kind, int owner)
{
  mcpc_anydata_t *d = mcpc_anydata_new ();
  d->ckind = (u8_t) kind;
  if (owner == CHD_TOOL)
    d->flags |= ANYDATA_FL_USRCBRES_TOOLCALL;
  else if (owner == CHD_PRMPT_USER || owner == CHD_PRMPT_ASSIST)
    {
      d->flags |= ANYDATA_FL_USRCBRES_PRMPTCALL;
      if (owner == CHD_PRMPT_ASSIST)
	d->flags |= ANYDATA_FL_USRCBRES_PRMPTCALL_ASSIST;
    }
  return d;
}

// which pool flag goes with an owner
static void
_flag_pool (mcpc_ucbr_t *ucbr, int owner)
{
  if (owner == CHD_TOOL)
    ucbr->flags |= ANYPOOL_FL_USRCBRES_TOOLCALL;
  else if (owner == CHD_PRMPT_USER || owner == CHD_PRMPT_ASSIST)
    ucbr->flags |= ANYPOOL_FL_USRCBRES_PRMPTCALL;
}

static void
_chd_set_bytes (mcpc_anydata_t *d, const void *bytes, size_t len)
{
  if (len > 0 && bytes != nullptr)
    mcpc_anydata_set_u8s (d, bytes, len);
  else
    d->data_typ = MCPC_U8S;	// empty payload, nothing to allocate
}

static void
_chd_set_str (mcpc_anydata_t *d, mcpc_xslot_t slot, const char8_t *s)
{
  if (s != nullptr && s[0] != 0)
    mcpc_anydata_xset (d, slot, (const char *) s, strlen ((const char *) s), 0);
}

static void
_add_media (mcpc_ucbr_t *ucbr, int owner, mcpc_ckind_t kind, const void *bytes, size_t len, const char8_t *mime_nt)
{
  bug_if_nullptr (ucbr);
  _flag_pool (ucbr, owner);
  mcpc_ucbrchd_t *d = _chd_new (kind, owner);
  _chd_set_bytes (d, bytes, len);
  _chd_set_str (d, MCPC_X_MIME, mime_nt != nullptr && mime_nt[0] ? mime_nt : u8c1 ("application/octet-stream"));
  mcpc_ucbr_addfre (ucbr, d);
}

static void
_add_link (mcpc_ucbr_t *ucbr, int owner, const char8_t *uri_nt, const char8_t *name_nt, const char8_t *desc_nt, const char8_t *mime_nt)
{
  bug_if_nullptr (ucbr);
  bug_if_nullptr (uri_nt);
  _flag_pool (ucbr, owner);
  mcpc_ucbrchd_t *d = _chd_new (MCPC_CK_RESLINK, owner);
  if (name_nt != nullptr && strlen ((const char *) name_nt) <= UINT8_MAX)
    mcpc_anydata_set_meta1nt (d, name_nt);
  _chd_set_str (d, MCPC_X_URI, uri_nt);
  _chd_set_str (d, MCPC_X_DESC, desc_nt);
  _chd_set_str (d, MCPC_X_MIME, mime_nt);
  mcpc_ucbr_addfre (ucbr, d);
}

static void
_add_embedded_text (mcpc_ucbr_t *ucbr, int owner, const char8_t *uri_nt, const char8_t *mime_nt, const char8_t *text_nt)
{
  bug_if_nullptr (ucbr);
  _flag_pool (ucbr, owner);
  mcpc_ucbrchd_t *d = _chd_new (MCPC_CK_RES_TEXT, owner);
  mcpc_anydata_set_u8str (d, text_nt != nullptr ? text_nt : u8c1 (""), text_nt != nullptr ? strlen ((const char *) text_nt) : 0);
  _chd_set_str (d, MCPC_X_URI, uri_nt);
  _chd_set_str (d, MCPC_X_MIME, mime_nt);
  mcpc_ucbr_addfre (ucbr, d);
}

static void
_add_embedded_blob (mcpc_ucbr_t *ucbr, int owner, const char8_t *uri_nt, const char8_t *mime_nt, const void *bytes, size_t len)
{
  bug_if_nullptr (ucbr);
  _flag_pool (ucbr, owner);
  mcpc_ucbrchd_t *d = _chd_new (MCPC_CK_RES_BLOB, owner);
  _chd_set_bytes (d, bytes, len);
  _chd_set_str (d, MCPC_X_URI, uri_nt);
  _chd_set_str (d, MCPC_X_MIME, mime_nt);
  mcpc_ucbr_addfre (ucbr, d);
}

MCPC_API void
mcpc_ucbr_toolcall_add_image (mcpc_ucbr_t *ucbr, const void *bytes, size_t len, const char8_t *mime_nt)
{
  _add_media (ucbr, CHD_TOOL, MCPC_CK_IMAGE, bytes, len, mime_nt);
}

MCPC_API void
mcpc_ucbr_toolcall_add_audio (mcpc_ucbr_t *ucbr, const void *bytes, size_t len, const char8_t *mime_nt)
{
  _add_media (ucbr, CHD_TOOL, MCPC_CK_AUDIO, bytes, len, mime_nt);
}

MCPC_API void
mcpc_ucbr_toolcall_add_resource_link (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *name_nt, const char8_t *desc_nt,
				      const char8_t *mime_nt)
{
  _add_link (ucbr, CHD_TOOL, uri_nt, name_nt, desc_nt, mime_nt);
}

MCPC_API void
mcpc_ucbr_toolcall_add_embedded_text (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt, const char8_t *text_nt)
{
  _add_embedded_text (ucbr, CHD_TOOL, uri_nt, mime_nt, text_nt);
}

MCPC_API void
mcpc_ucbr_toolcall_add_embedded_blob (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt, const void *bytes, size_t len)
{
  _add_embedded_blob (ucbr, CHD_TOOL, uri_nt, mime_nt, bytes, len);
}

#define PRMPT_VARIANTS(NAME, ROLE_SUFFIX, OWNER)                                                                                   \
MCPC_API void mcpc_ucbr_prmptget_add_##ROLE_SUFFIX##_image (mcpc_ucbr_t *u, const void *b, size_t n, const char8_t *m)              \
{ _add_media (u, OWNER, MCPC_CK_IMAGE, b, n, m); }                                                                                \
MCPC_API void mcpc_ucbr_prmptget_add_##ROLE_SUFFIX##_audio (mcpc_ucbr_t *u, const void *b, size_t n, const char8_t *m)              \
{ _add_media (u, OWNER, MCPC_CK_AUDIO, b, n, m); }                                                                                \
MCPC_API void mcpc_ucbr_prmptget_add_##ROLE_SUFFIX##_embedded_text (mcpc_ucbr_t *u, const char8_t *uri, const char8_t *m, const char8_t *t) \
{ _add_embedded_text (u, OWNER, uri, m, t); }                                                                                     \
MCPC_API void mcpc_ucbr_prmptget_add_##ROLE_SUFFIX##_embedded_blob (mcpc_ucbr_t *u, const char8_t *uri, const char8_t *m, const void *b, size_t n) \
{ _add_embedded_blob (u, OWNER, uri, m, b, n); }                                                                                  \
MCPC_API void mcpc_ucbr_prmptget_add_##ROLE_SUFFIX##_resource_link (mcpc_ucbr_t *u, const char8_t *uri, const char8_t *name, const char8_t *d, const char8_t *m) \
{ _add_link (u, OWNER, uri, name, d, m); }

PRMPT_VARIANTS (user, user, CHD_PRMPT_USER)
PRMPT_VARIANTS (assist, assist, CHD_PRMPT_ASSIST)

// JSON text must be one complete value, whitespace around it is fine
static bool
_is_json_value (const char8_t *s, int want_tok)
{
  const char *tok = nullptr;
  int toklen = 0;
  const int n = (int) strlen ((const char *) s);
  const int tt = mjson_find ((const char *) s, n, "$", &tok, &toklen);
  if (tt == MJSON_TOK_INVALID || (want_tok != 0 && tt != want_tok))
    return false;
  // nothing but whitespace may follow the value
  for (const char *p = tok + toklen; p < (const char *) s + n; p++)
    if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
      return false;
  return true;
}

MCPC_API mcpc_errcode_t
mcpc_ucbr_toolcall_set_structured_json (mcpc_ucbr_t *ucbr, const char8_t *json_nt)
{
  bug_if_nullptr (ucbr);
  bug_if_nullptr (json_nt);
  if (!_is_json_value (json_nt, 0))
    return MCPC_EC_JSON_FIELD_INVALID;

  ucbr->flags |= ANYPOOL_FL_USRCBRES_TOOLCALL;
  mcpc_ucbrchd_t *d = _chd_new (MCPC_CK_STRUCTURED, CHD_TOOL);
  mcpc_anydata_set_json (d, json_nt, strlen ((const char *) json_nt));
  mcpc_ucbr_addfre (ucbr, d);

  // clients that predate structured output read the same data from a text block
  mcpc_ucbr_toolcall_add_text_printf8 (ucbr, u8c1 ("%s"), json_nt);
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_ucbr_set_meta_json (mcpc_ucbr_t *ucbr, const char8_t *json_object_nt)
{
  bug_if_nullptr (ucbr);
  bug_if_nullptr (json_object_nt);
  if (!_is_json_value (json_object_nt, MJSON_TOK_OBJECT))
    return MCPC_EC_JSON_FIELD_INVALID;

  mcpc_ucbrchd_t *d = _chd_new (MCPC_CK_RESMETA, CHD_NONE);
  mcpc_anydata_set_json (d, json_object_nt, strlen ((const char *) json_object_nt));
  mcpc_ucbr_addfre (ucbr, d);
  return MCPC_EC_0;
}

MCPC_API void
mcpc_ucbr_rscread_add_text (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt, const char8_t *fmt, ...)
{
  bug_if_nullptr (ucbr);
  bug_if_nullptr (fmt);
  va_list vargs;
  va_start (vargs, fmt);
  mcpc_ucbrchd_t *d = mcpc_ucbrchd_new_vprintf8 (fmt, &vargs);
  va_end (vargs);
  if (d == nullptr)
    return;
  d->ckind = (u8_t) MCPC_CK_RES_TEXT;
  _chd_set_str (d, MCPC_X_URI, uri_nt);
  _chd_set_str (d, MCPC_X_MIME, mime_nt);
  mcpc_ucbr_addfre (ucbr, d);
}

MCPC_API void
mcpc_ucbr_rscread_add_blob (mcpc_ucbr_t *ucbr, const char8_t *uri_nt, const char8_t *mime_nt, const void *bytes,
			    size_t len)
{
  bug_if_nullptr (ucbr);
  mcpc_ucbrchd_t *d = _chd_new (MCPC_CK_RES_BLOB, CHD_NONE);
  _chd_set_bytes (d, bytes, len);
  _chd_set_str (d, MCPC_X_URI, uri_nt);
  _chd_set_str (d, MCPC_X_MIME, mime_nt);
  mcpc_ucbr_addfre (ucbr, d);
}

MCPC_API void
mcpc_ucbr_toolcall_add_errmsg_printf8 (mcpc_ucbr_t *ucbr, const char8_t *fmt,
                                       ...)
{
  va_list vargs;
  va_start (vargs, fmt);

  ucbr->flags |= ANYPOOL_FL_USRCBRES_TOOLCALL_ERROR;
  mcpc_ucbr_toolcallres_add_text_vprintf8 (ucbr, fmt, &vargs);

  va_end (vargs);
}

MCPC_API void
mcpc_ucbr_prmptget_add_user_printf8 (mcpc_ucbr_t *ucbr, const char8_t *fmt,
                                     ...)
{
  va_list vargs;
  va_start (vargs, fmt);

  ucbr->flags |= ANYPOOL_FL_USRCBRES_PRMPTCALL;

  mcpc_ucbrchd_t *newctn = mcpc_ucbrchd_new_vprintf8 (fmt, &vargs);
  newctn->flags |= ANYDATA_FL_USRCBRES_PRMPTCALL;

  mcpc_ucbr_addfre (ucbr, newctn);

  va_end (vargs);
}

MCPC_API void
mcpc_ucbr_prmptget_add_assist_printf8 (mcpc_ucbr_t *ucbr, const char8_t *fmt,
                                       ...)
{
  va_list vargs;
  va_start (vargs, fmt);

  ucbr->flags |= ANYPOOL_FL_USRCBRES_PRMPTCALL;

  mcpc_ucbrchd_t *newctn = mcpc_ucbrchd_new_vprintf8 (fmt, &vargs);
  newctn->flags |= ANYDATA_FL_USRCBRES_PRMPTCALL;
  newctn->flags |= ANYDATA_FL_USRCBRES_PRMPTCALL_ASSIST;

  mcpc_ucbr_addfre (ucbr, newctn);

  va_end (vargs);
}
