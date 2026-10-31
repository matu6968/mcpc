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

#include "hmac.h"

typedef struct
{
  uint32_t h[8];
  uint64_t total;		// bytes fed so far
  uint8_t buf[64];
  size_t fill;
} sha256_ctx;

static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
sha256_block (sha256_ctx *c, const uint8_t *p)
{
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t) p[4 * i] << 24) | ((uint32_t) p[4 * i + 1] << 16) | ((uint32_t) p[4 * i + 2] << 8) | p[4 * i + 3];
  for (int i = 16; i < 64; i++)
    {
      const uint32_t s0 = ROR (w[i - 15], 7) ^ ROR (w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = ROR (w[i - 2], 17) ^ ROR (w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
  for (int i = 0; i < 64; i++)
    {
      const uint32_t S1 = ROR (e, 6) ^ ROR (e, 11) ^ ROR (e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + S1 + ch + K[i] + w[i];
      const uint32_t S0 = ROR (a, 2) ^ ROR (a, 13) ^ ROR (a, 22);
      const uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
      const uint32_t t2 = S0 + mj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = cc;
      cc = b;
      b = a;
      a = t1 + t2;
    }
  c->h[0] += a;
  c->h[1] += b;
  c->h[2] += cc;
  c->h[3] += d;
  c->h[4] += e;
  c->h[5] += f;
  c->h[6] += g;
  c->h[7] += h;
}

static void
sha256_init (sha256_ctx *c)
{
  static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  memcpy (c->h, iv, sizeof (iv));
  c->total = 0;
  c->fill = 0;
}

static void
sha256_update (sha256_ctx *c, const void *data, size_t len)
{
  const uint8_t *p = data;
  c->total += len;
  while (len > 0)
    {
      size_t n = 64 - c->fill;
      if (n > len)
	n = len;
      memcpy (c->buf + c->fill, p, n);
      c->fill += n;
      p += n;
      len -= n;
      if (c->fill == 64)
	{
	  sha256_block (c, c->buf);
	  c->fill = 0;
	}
    }
}

static void
sha256_final (sha256_ctx *c, uint8_t out[MCPC_SHA256_LEN])
{
  const uint64_t bits = c->total * 8;
  const uint8_t one = 0x80, zero = 0;
  sha256_update (c, &one, 1);
  while (c->fill != 56)
    sha256_update (c, &zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; i++)
    len[i] = (uint8_t) (bits >> (56 - 8 * i));
  sha256_update (c, len, 8);
  for (int i = 0; i < 8; i++)
    {
      out[4 * i] = (uint8_t) (c->h[i] >> 24);
      out[4 * i + 1] = (uint8_t) (c->h[i] >> 16);
      out[4 * i + 2] = (uint8_t) (c->h[i] >> 8);
      out[4 * i + 3] = (uint8_t) c->h[i];
    }
}

void
mcpc_sha256 (const void *data, size_t len, uint8_t out[MCPC_SHA256_LEN])
{
  sha256_ctx c;
  sha256_init (&c);
  sha256_update (&c, data, len);
  sha256_final (&c, out);
}

void
mcpc_hmac_sha256 (const void *key, size_t key_len, const void *data, size_t len, uint8_t out[MCPC_SHA256_LEN])
{
  uint8_t k[64] = { 0 };
  if (key_len > 64)
    mcpc_sha256 (key, key_len, k);	// long keys are hashed first
  else if (key_len > 0)
    memcpy (k, key, key_len);

  uint8_t pad[64], inner[MCPC_SHA256_LEN];
  sha256_ctx c;

  for (int i = 0; i < 64; i++)
    pad[i] = k[i] ^ 0x36;
  sha256_init (&c);
  sha256_update (&c, pad, 64);
  sha256_update (&c, data, len);
  sha256_final (&c, inner);

  for (int i = 0; i < 64; i++)
    pad[i] = k[i] ^ 0x5c;
  sha256_init (&c);
  sha256_update (&c, pad, 64);
  sha256_update (&c, inner, MCPC_SHA256_LEN);
  sha256_final (&c, out);

  memset (k, 0, sizeof (k));
}

bool
mcpc_ct_equal (const void *a, const void *b, size_t len)
{
  const volatile uint8_t *x = a, *y = b;
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++)
    diff |= (uint8_t) (x[i] ^ y[i]);
  return diff == 0;
}

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t
mcpc_b64_encode (const void *data, size_t len, char *out)
{
  const uint8_t *s = data;
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3)
    {
      const uint32_t a = s[i], b = i + 1 < len ? s[i + 1] : 0, c = i + 2 < len ? s[i + 2] : 0;
      const uint32_t v = (a << 16) | (b << 8) | c;
      out[o++] = b64chars[(v >> 18) & 63];
      out[o++] = b64chars[(v >> 12) & 63];
      out[o++] = i + 1 < len ? b64chars[(v >> 6) & 63] : '=';
      out[o++] = i + 2 < len ? b64chars[v & 63] : '=';
    }
  out[o] = 0;
  return o;
}

long
mcpc_b64_decode (const char *text, size_t len, uint8_t *out)
{
  if (len % 4 != 0)
    return -1;
  size_t o = 0;
  for (size_t i = 0; i < len; i += 4)
    {
      uint32_t v = 0;
      int pad = 0;
      for (int j = 0; j < 4; j++)
	{
	  const char ch = text[i + j];
	  const char *p = ch != 0 && ch != '=' ? strchr (b64chars, ch) : nullptr;
	  if (ch == '=' && i + 4 == len && j >= 2)
	    {
	      pad++;
	      v <<= 6;
	    }
	  else if (p != nullptr && pad == 0)
	    v = (v << 6) | (uint32_t) (p - b64chars);
	  else
	    return -1;
	}
      out[o++] = (uint8_t) (v >> 16);
      if (pad < 2)
	out[o++] = (uint8_t) (v >> 8);
      if (pad < 1)
	out[o++] = (uint8_t) v;
    }
  return (long) o;
}
