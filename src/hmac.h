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

#ifndef h_hmac
#define h_hmac

#include <stddef.h>
#include <stdint.h>

#include <mcpc/_c23_keywords.h>

#define MCPC_SHA256_LEN 32

void mcpc_sha256 (const void *data, size_t len, uint8_t out[MCPC_SHA256_LEN]);

void mcpc_hmac_sha256 (const void *key, size_t key_len, const void *data, size_t len, uint8_t out[MCPC_SHA256_LEN]);

// compares in time independent of where the buffers differ
bool mcpc_ct_equal (const void *a, const void *b, size_t len);

// standard base64 with padding; the encoded text is NUL-terminated, out needs 4*((len+2)/3)+1 bytes
size_t mcpc_b64_encode (const void *data, size_t len, char *out);

// returns the decoded length, or -1 for text that is not valid base64; out needs 3*(len/4) bytes
long mcpc_b64_decode (const char *text, size_t len, uint8_t * out);

#endif
