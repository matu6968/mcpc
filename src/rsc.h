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

#ifndef h_rsc
#define h_rsc

#include <stddef.h>
#include <stdio.h>

#include <mcpc/_c23_keywords.h>
#include <mcpc/_c23_uchar.h>

#include <mcpc/rsc.h>

typedef mcpc_anypool_t mcpc_rscpool_t;
typedef mcpc_anypool_t mcpc_rsctplpool_t;

// mime type of a resource or template as text, NULL when it has none
const char *mcpc_rsc_mime_cstr (const mcpc_rsc_t * rsc);

// does uri match the template; on a match the callback is called for each variable
typedef void (*mcpc_tplvar_fn) (const char *name, size_t name_len, const char *val, size_t val_len, void *ud);
bool mcpc_rsctpl_match (const mcpc_rsctpl_t * tpl, const char *uri, size_t uri_len, mcpc_tplvar_fn fn, void *ud);

// first resource whose uri equals uri, or NULL
const mcpc_rsc_t *mcpc_rscpool_getbyuri (const mcpc_rscpool_t * rscpool, const char8_t * uri, size_t uri_len);

// first template matching uri, or NULL
const mcpc_rsctpl_t *mcpc_rsctplpool_find (const mcpc_rsctplpool_t * pool, const char8_t * uri, size_t uri_len);

// ---------------------------- rscpool ----------------------------

mcpc_rscpool_t *mcpc_rscpool_new ();

void mcpc_rscpool_free (mcpc_rscpool_t * rscpool);

void mcpc_rscpool_add_cp (mcpc_rscpool_t * rscpool, const mcpc_rsc_t * tmp_rsc);

void mcpc_rscpool_addfre (mcpc_rscpool_t * rscpool, mcpc_rsc_t * tmp_rsc);

size_t mcpc_rscpool_get_len (const mcpc_rscpool_t * rscpool);

const mcpc_rsc_t *mcpc_rscpool_get (const mcpc_rscpool_t * rscpool, size_t idx);

const mcpc_rsc_t *mcpc_rscpool_get_next (const mcpc_rscpool_t * rscpool);

const mcpc_rsc_t *mcpc_rscpool_getbyname (const mcpc_rscpool_t * rscpool, const char8_t * name, size_t name_len);

#endif
