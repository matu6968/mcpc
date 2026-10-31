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

#include "rsc.h"

#include <stdint.h>
#include <stdlib.h>

#include "ext_string.h"

#include "errcode.h"
#include "alloc.h"
#include "anydata.h"
#include "mjson.h"

MCPC_API mcpc_rsc_t *
mcpc_rsc_new (const char8_t *uri, const size_t uri_len, const char8_t *name, const size_t name_len)
{
  mcpc_anydata_t *anydata = mcpc_anydata_new_typ_meta1 (MCPC_U8STR, name, name_len);

  anydata->flags |= ANYDATA_FL_RSC;
  anydata->data_mime = MCPC_MIME_TEXT_PLAIN;

  if (MCPC_EC_0 != mcpc_anydata_set_u8str (anydata, uri, uri_len))
    goto badret;

  return (mcpc_rsc_t *) anydata;

badret:
  mcpc_anydata_free (anydata);
  return nullptr;
}

MCPC_API mcpc_rsc_t *
mcpc_rsc_new2 (const char8_t *uri_nt, const char8_t *nament)
{
  return mcpc_rsc_new (uri_nt, u8strlen (uri_nt), nament, u8strlen (nament));
}

MCPC_API mcpc_rsc_t *
mcpc_rsc_new3 (mcpc_mime_t mime, const char8_t *uri_nt, const char8_t *nament)
{
  mcpc_rsc_t *rsc = mcpc_rsc_new2 (uri_nt, nament);
  rsc->data_mime = mime;
  return rsc;
}

MCPC_API mcpc_errcode_t
mcpc_rsc_get_uri (const mcpc_rsc_t *rsc, char8_t *ret, size_t ret_cap, size_t *ret_len)
{
  return mcpc_anydata_get_u8str (rsc, ret, ret_cap, ret_len);
}

MCPC_API mcpc_errcode_t
mcpc_rsc_getref_uri (const mcpc_rsc_t *rsc, const char8_t **ret, size_t *ret_len)
{
  return mcpc_anydata_getref_u8str (rsc, ret, ret_len);
}

MCPC_API mcpc_errcode_t
mcpc_rsc_getref_name (const mcpc_rsc_t *rsc, const char8_t **ret, size_t *ret_len)
{
  return mcpc_anydata_getref_meta1 (rsc, ret, ret_len);
}

MCPC_API mcpc_errcode_t
mcpc_rsc_get_mime (const mcpc_rsc_t *rsc, mcpc_mime_t *ret)
{
  bug_if_nullptr (rsc);
  bug_if_nullptr (ret);
  *ret = rsc->data_mime;
  return MCPC_EC_0;
}

MCPC_API void
mcpc_rsc_set_mime (mcpc_rsc_t *rsc, mcpc_mime_t mime)
{
  bug_if_nullptr (rsc);
  rsc->data_mime = mime;
}

MCPC_API void
mcpc_rsc_free (mcpc_rsc_t *rsc)
{
  mcpc_anydata_free (rsc);
}

/* = = = = = = = = = = = = = = = = metadata = = = = = = = = = = = = = = = = */

MCPC_API mcpc_errcode_t
mcpc_rsc_set_read_cb (mcpc_rsc_t *rsc, mcpc_rcallcb_t cb)
{
  bug_if_nullptr (rsc);
  mcpc_anydata_xensure (rsc)->cb = (void (*) (void)) cb;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_rsc_set_read_cb2 (mcpc_rsc_t *rsc, mcpc_rcallcb2_t cb)
{
  bug_if_nullptr (rsc);
  mcpc_anydata_xensure (rsc)->cb2 = (void (*) (void)) cb;
  return MCPC_EC_0;
}

MCPC_API mcpc_errcode_t
mcpc_rsctpl_set_read_cb2 (mcpc_rsctpl_t *tpl, mcpc_rcallcb2_t cb)
{
  return mcpc_rsc_set_read_cb2 (tpl, cb);
}

MCPC_API mcpc_errcode_t
mcpc_rsc_set_description (mcpc_rsc_t *rsc, const char8_t *descnt)
{
  return mcpc_anydata_xset (rsc, MCPC_X_DESC, (const char *) descnt, descnt ? strlen ((const char *) descnt) : 0, 0);
}

MCPC_API mcpc_errcode_t
mcpc_rsc_set_mime_str (mcpc_rsc_t *rsc, const char8_t *miment)
{
  return mcpc_anydata_xset (rsc, MCPC_X_MIME, (const char *) miment, miment ? strlen ((const char *) miment) : 0, 0);
}

MCPC_API void
mcpc_rsc_set_size (mcpc_rsc_t *rsc, size_t bytes)
{
  bug_if_nullptr (rsc);
  struct mcpc_anyextra *x = mcpc_anydata_xensure (rsc);
  x->size = bytes;
  x->has_size = true;
}

const char *
mcpc_rsc_mime_cstr (const mcpc_rsc_t *rsc)
{
  const char *s = mcpc_anydata_xget (rsc, MCPC_X_MIME);
  if (s != nullptr)
    return s;

  switch (rsc->data_mime)
    {
    case MCPC_MIME_TEXT_PLAIN:
      return "text/plain";
    case MCPC_MIME_IMAGE_PNG:
      return "image/png";
    case MCPC_MIME_IMAGE_JPEG:
      return "image/jpeg";
    case MCPC_MIME_IMAGE_GIF:
      return "image/gif";
    case MCPC_MIME_AUDIO_MPEG:
      return "audio/mpeg";
    case MCPC_MIME_AUDIO_WAV:
      return "audio/wav";
    case MCPC_MIME_VIDEO_MPEG:
      return "video/mpeg";
    case MCPC_MIME_VIDEO_AVI:
      return "video/avi";
    case MCPC_MIME_APPLI_OCTS:
      return "application/octet-stream";
    case MCPC_MIME_APPLI_JSON:
      return "application/json";
    case MCPC_MIME_APPLI_PDF:
      return "application/pdf";
    default:
      return nullptr;
    }
}

const mcpc_rsc_t *
mcpc_rscpool_getbyuri (const mcpc_rscpool_t *rscpool, const char8_t *uri, size_t uri_len)
{
  const size_t n = mcpc_rscpool_get_len (rscpool);
  for (size_t i = 0; i < n; i++)
    {
      const mcpc_rsc_t *rsc = mcpc_rscpool_get (rscpool, i);
      const char8_t *u = nullptr;
      size_t u_len = 0;
      if (mcpc_anydata_getref_u8str (rsc, &u, &u_len) == MCPC_EC_0 && u_len == uri_len && memcmp (u, uri, uri_len) == 0)
	return rsc;
    }
  return nullptr;
}

/* = = = = = = = = = = = = = = = rsctpl = = = = = = = = = = = = = = = */

MCPC_API mcpc_rsctpl_t *
mcpc_rsctpl_new (const char8_t *uri_template_nt, const char8_t *name_nt)
{
  bug_if_nullptr (uri_template_nt);
  bug_if_nullptr (name_nt);
  mcpc_anydata_t *d = mcpc_anydata_new_typ_meta1 (MCPC_NONE, name_nt, strlen ((const char *) name_nt));
  d->flags |= ANYDATA_FL_RSCTPL;
  if (MCPC_EC_0 != mcpc_anydata_xset (d, MCPC_X_URITPL, (const char *) uri_template_nt,
				      strlen ((const char *) uri_template_nt), 0))
    {
      mcpc_anydata_free (d);
      return nullptr;
    }
  return d;
}

MCPC_API void
mcpc_rsctpl_free (mcpc_rsctpl_t *tpl)
{
  mcpc_anydata_free (tpl);
}

MCPC_API mcpc_errcode_t
mcpc_rsctpl_set_read_cb (mcpc_rsctpl_t *tpl, mcpc_rcallcb_t cb)
{
  return mcpc_rsc_set_read_cb (tpl, cb);
}

MCPC_API mcpc_errcode_t
mcpc_rsctpl_set_description (mcpc_rsctpl_t *tpl, const char8_t *descnt)
{
  return mcpc_rsc_set_description (tpl, descnt);
}

MCPC_API mcpc_errcode_t
mcpc_rsctpl_set_mime_str (mcpc_rsctpl_t *tpl, const char8_t *miment)
{
  return mcpc_rsc_set_mime_str (tpl, miment);
}

// Match uri against an RFC 6570 level 1 template ({name}, and {+name} to cross '/').
// A variable ends at the literal that follows it in the template, or at the end of the uri.
bool
mcpc_rsctpl_match (const mcpc_rsctpl_t *tpl, const char *uri, size_t uri_len, mcpc_tplvar_fn fn, void *ud)
{
  const char *t = mcpc_anydata_xget (tpl, MCPC_X_URITPL);
  if (t == nullptr)
    return false;
  const char *tend = t + strlen (t);
  size_t up = 0;

  while (t < tend)
    {
      if (*t != '{')
	{
	  // literal run up to the next variable
	  const char *lit_end = t;
	  while (lit_end < tend && *lit_end != '{')
	    lit_end++;
	  const size_t n = (size_t) (lit_end - t);
	  if (up + n > uri_len || memcmp (uri + up, t, n) != 0)
	    return false;
	  up += n;
	  t = lit_end;
	  continue;
	}

      const char *close = memchr (t, '}', (size_t) (tend - t));
      if (close == nullptr)
	return false;
      const char *name = t + 1;
      bool reserved = false;
      if (name < close && *name == '+')
	{
	  reserved = true;
	  name++;
	}
      // only simple names are supported, operators such as {/x} or {?q} never match
      for (const char *q = name; q < close; q++)
	if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '.'))
	  return false;
      if (name == close)
	return false;
      t = close + 1;

      // the literal after the variable bounds it
      const char *next_lit = t;
      const char *next_end = t;
      while (next_end < tend && *next_end != '{')
	next_end++;
      const size_t lit_len = (size_t) (next_end - next_lit);

      size_t vend = up;
      if (lit_len == 0)
	{
	  vend = uri_len;		// last variable, or two in a row: take the rest
	  if (!reserved)
	    {
	      vend = up;
	      while (vend < uri_len && uri[vend] != '/')
		vend++;
	    }
	}
      else
	{
	  // the variable ends where the literal starts; a plain variable also stops at '/'
	  bool found = false;
	  for (size_t i = up; i + lit_len <= uri_len; i++)
	    {
	      if (memcmp (uri + i, next_lit, lit_len) == 0)
		{
		  vend = i;
		  found = true;
		  break;
		}
	      if (!reserved && uri[i] == '/')
		break;
	    }
	  if (!found)
	    return false;
	}

      if (vend == up)
	return false;		// variables must not be empty
      if (!reserved && memchr (uri + up, '/', vend - up) != nullptr)
	return false;
      if (fn != nullptr)
	fn (name, (size_t) (close - name), uri + up, vend - up, ud);
      up = vend;
    }

  return up == uri_len;
}

const mcpc_rsctpl_t *
mcpc_rsctplpool_find (const mcpc_rsctplpool_t *pool, const char8_t *uri, size_t uri_len)
{
  for (size_t i = 0; i < pool->len; i++)
    {
      const mcpc_rsctpl_t *tpl = pool->head + i;
      if (mcpc_rsctpl_match (tpl, (const char *) uri, uri_len, nullptr, nullptr))
	return tpl;
    }
  return nullptr;
}

struct getvar_ud
{
  const char *want;
  size_t want_len;
  const char *val;
  size_t val_len;
  bool hit;
};

static void
_getvar_cb (const char *name, size_t name_len, const char *val, size_t val_len, void *ud)
{
  struct getvar_ud *g = ud;
  if (!g->hit && name_len == g->want_len && memcmp (name, g->want, name_len) == 0)
    {
      g->val = val;
      g->val_len = val_len;
      g->hit = true;
    }
}

MCPC_API mcpc_errcode_t
mcpc_rsctpl_get_var (const mcpc_rsctpl_t *tpl, const char8_t *uri, size_t uri_len, const char8_t *var_nt,
		     char8_t *ret, size_t ret_cap, size_t *ret_len)
{
  struct getvar_ud g = { (const char *) var_nt, strlen ((const char *) var_nt), nullptr, 0, false };
  if (!mcpc_rsctpl_match (tpl, (const char *) uri, uri_len, _getvar_cb, &g) || !g.hit)
    return MCPC_EC_ANYDATA_NOTFOUND;

  if (ret_len != nullptr)
    *ret_len = g.val_len;
  if (ret_cap <= g.val_len)
    return MCPC_EC_BUFCAP;
  memcpy (ret, g.val, g.val_len);
  ret[g.val_len] = 0;
  return MCPC_EC_0;
}

/* = = = = = = = = = = = = = = = = rscpool = = = = = = = = = = = = = = = = */

mcpc_rscpool_t *
mcpc_rscpool_new ()
{
  return mcpc_anypool_new ();
}

void
mcpc_rscpool_free (mcpc_rscpool_t *rscpool)
{
  mcpc_anypool_free (rscpool);
}

void
mcpc_rscpool_addcpy (mcpc_rscpool_t *rscpool, const mcpc_rsc_t *rsc)
{
  mcpc_anypool_addcpy (rscpool, rsc);
}

void
mcpc_rscpool_addfre (mcpc_rscpool_t *rscpool, mcpc_rsc_t *rsc)
{
  mcpc_anypool_addfre (rscpool, rsc);
}

size_t
mcpc_rscpool_get_len (const mcpc_rscpool_t *rscpool)
{
  bug_if_nullptr (rscpool);
  size_t ret = 0;
  ret = ((const mcpc_anypool_t *) rscpool)->len;
  return ret;
}

const mcpc_rsc_t *
mcpc_rscpool_get (const mcpc_rscpool_t *rscpool, size_t idx)
{
  return mcpc_anypool_get (rscpool, idx);
}

const mcpc_rsc_t *
mcpc_rscpool_getbyname (const mcpc_rscpool_t *rscpool, const char8_t *name, size_t name_len)
{
  return mcpc_anypool_getbymeta1 (rscpool, name, name_len);
}

const mcpc_rsc_t *
mcpc_rscpool_get_next (const mcpc_rscpool_t *rscpool)
{
  bug_if_nullptr (rscpool);

  static size_t idx = 0;

  const mcpc_anypool_t *pool = rscpool;
  if (pool == nullptr)
    return nullptr;

  const size_t pool_len = pool->len;
  if (pool_len == 0)
    return nullptr;

  if (idx == pool_len)		// TODO  MT-SAFE
    {
      idx = 0;
      return nullptr;
    }

  const mcpc_anydata_t *ret = pool->head + idx;
  idx += 1;

  return ret;
}
