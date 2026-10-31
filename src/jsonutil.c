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

#include "jsonutil.h"
#include "mjson.h"

int
mcpc_json_member (const char *obj, size_t obj_len, const char *key, const char **val, size_t *val_len)
{
  const size_t key_len = strlen (key);
  int koff, klen, voff, vlen, vtype;

  if (obj == nullptr || obj_len == 0)
    return MJSON_TOK_INVALID;

  for (int off = 0; (off = mjson_next (obj, (int) obj_len, off, &koff, &klen, &voff, &vlen, &vtype)) != 0;)
    {
      // the key token includes its quotes; reserved keys never contain escapes
      if (klen >= 2 && (size_t) klen - 2 == key_len && memcmp (obj + koff + 1, key, key_len) == 0)
	{
	  if (val != nullptr)
	    *val = obj + voff;
	  if (val_len != nullptr)
	    *val_len = (size_t) vlen;
	  return vtype;
	}
    }
  return MJSON_TOK_INVALID;
}

static bool
utf8_ok (const unsigned char *s, size_t n)
{
  for (size_t i = 0; i < n;)
    {
      const unsigned char c = s[i];
      size_t k;
      unsigned cp;
      if (c < 0x80)
	{
	  i++;
	  continue;
	}
      else if (c >= 0xC2 && c <= 0xDF)
	{
	  k = 1;
	  cp = c & 0x1F;
	}
      else if (c >= 0xE0 && c <= 0xEF)
	{
	  k = 2;
	  cp = c & 0x0F;
	}
      else if (c >= 0xF0 && c <= 0xF4)
	{
	  k = 3;
	  cp = c & 0x07;
	}
      else
	return false;
      if (i + k >= n)
	return false;		// the sequence runs past the end
      for (size_t j = 1; j <= k; j++)
	{
	  if ((s[i + j] & 0xC0) != 0x80)
	    return false;
	  cp = (cp << 6) | (s[i + j] & 0x3F);
	}
      // overlong forms, surrogates and values beyond U+10FFFF are not characters
      if ((k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
	return false;
      i += k + 1;
    }
  return true;
}

bool
mcpc_json_token_clean (int type, const char *tok, size_t len)
{
  if (tok == nullptr || len == 0)
    return false;
  if (type == MJSON_TOK_TRUE || type == MJSON_TOK_FALSE || type == MJSON_TOK_NULL)
    return true;

  if (type == MJSON_TOK_NUMBER)
    {
      size_t i = 0;
      if (tok[i] == '-')
	i++;
      if (i >= len)
	return false;
      if (tok[i] == '0')
	i++;
      else if (tok[i] >= '1' && tok[i] <= '9')
	while (i < len && tok[i] >= '0' && tok[i] <= '9')
	  i++;
      else
	return false;
      if (i < len && tok[i] == '.')
	{
	  size_t d = ++i;
	  while (i < len && tok[i] >= '0' && tok[i] <= '9')
	    i++;
	  if (i == d)
	    return false;
	}
      if (i < len && (tok[i] == 'e' || tok[i] == 'E'))
	{
	  i++;
	  if (i < len && (tok[i] == '+' || tok[i] == '-'))
	    i++;
	  size_t d = i;
	  while (i < len && tok[i] >= '0' && tok[i] <= '9')
	    i++;
	  if (i == d)
	    return false;
	}
      return i == len;
    }

  if (type != MJSON_TOK_STRING || len < 2 || tok[0] != '"' || tok[len - 1] != '"')
    return false;
  for (size_t i = 1; i + 1 < len; i++)
    {
      const unsigned char c = (unsigned char) tok[i];
      if (c < 0x20)
	return false;
      if (c == '"')
	return false;		// an unescaped quote cannot be inside a string
      if (c == '\\')
	{
	  if (i + 2 >= len)
	    return false;
	  const char e = tok[i + 1];
	  if (e == 'u')
	    {
	      if (i + 6 >= len)
		return false;	// \\uXXXX and the closing quote must fit
	      for (int k = 2; k < 6; k++)
		{
		  const char h = tok[i + k];
		  if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')))
		    return false;
		}
	      i += 5;
	    }
	  else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't')
	    i++;
	  else
	    return false;
	}
    }
  return utf8_ok ((const unsigned char *) tok + 1, len - 2);
}
