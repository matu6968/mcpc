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

#include <string.h>

#include "proto.h"

static const struct
{
  mcpc_pver_t v;
  const char *str;
  unsigned feats;
} pvers[] = {
  { MCPC_PV_2024_11_05, "2024-11-05", 0 },
  { MCPC_PV_2025_03_26, "2025-03-26", MCPC_FEAT_TOOL_ANNOT | MCPC_FEAT_AUDIO | MCPC_FEAT_PROGRESS_MSG },
  { MCPC_PV_2025_06_18, "2025-06-18",
   MCPC_FEAT_TOOL_ANNOT | MCPC_FEAT_AUDIO | MCPC_FEAT_PROGRESS_MSG | MCPC_FEAT_TITLE | MCPC_FEAT_STRUCTURED |
   MCPC_FEAT_RESLINK | MCPC_FEAT_ELICIT | MCPC_FEAT_META },
  { MCPC_PV_2025_11_25, "2025-11-25",
   MCPC_FEAT_TOOL_ANNOT | MCPC_FEAT_AUDIO | MCPC_FEAT_PROGRESS_MSG | MCPC_FEAT_TITLE | MCPC_FEAT_STRUCTURED |
   MCPC_FEAT_RESLINK | MCPC_FEAT_ELICIT | MCPC_FEAT_META | MCPC_FEAT_ICONS | MCPC_FEAT_ELICIT_URL |
   MCPC_FEAT_SAMPLING_TOOLS | MCPC_FEAT_IMPL_DESC },
  { MCPC_PV_2026_07_28, "2026-07-28",
   MCPC_FEAT_TOOL_ANNOT | MCPC_FEAT_AUDIO | MCPC_FEAT_PROGRESS_MSG | MCPC_FEAT_TITLE | MCPC_FEAT_STRUCTURED |
   MCPC_FEAT_RESLINK | MCPC_FEAT_ELICIT | MCPC_FEAT_META | MCPC_FEAT_ICONS | MCPC_FEAT_ELICIT_URL |
   MCPC_FEAT_SAMPLING_TOOLS | MCPC_FEAT_IMPL_DESC | MCPC_FEAT_MODERN },
};

const char *
mcpc_pver_str (mcpc_pver_t v)
{
  for (size_t i = 0; i < sizeof (pvers) / sizeof (pvers[0]); i++)
    if (pvers[i].v == v)
      return pvers[i].str;
  return "";
}

mcpc_pver_t
mcpc_pver_parse (const char *s, size_t len)
{
  for (size_t i = 0; i < sizeof (pvers) / sizeof (pvers[0]); i++)
    if (strlen (pvers[i].str) == len && memcmp (pvers[i].str, s, len) == 0)
      return pvers[i].v;
  return MCPC_PV_NONE;
}

mcpc_pver_t
mcpc_pver_negotiate (const char *s, size_t len)
{
  const mcpc_pver_t v = mcpc_pver_parse (s, len);
  return (v != MCPC_PV_NONE && !mcpc_pver_is_modern (v)) ? v : MCPC_PV_LATEST_LEGACY;
}

unsigned
mcpc_pver_feats (mcpc_pver_t v)
{
  for (size_t i = 0; i < sizeof (pvers) / sizeof (pvers[0]); i++)
    if (pvers[i].v == v)
      return pvers[i].feats;
  return 0;
}

unsigned
mcpc_pver_bit (mcpc_pver_t v)
{
  return v == MCPC_PV_NONE ? 0 : 1u << ((unsigned) v - 1);
}
