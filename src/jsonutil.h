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

#ifndef h_jsonutil
#define h_jsonutil

#include <stddef.h>

#include <mcpc/_c23_keywords.h>

// Value of the member `key` of the JSON object obj. Unlike mjson_find paths this takes the key
// literally, so keys such as "io.modelcontextprotocol/protocolVersion" work. Returns the
// MJSON_TOK_* type of the value, MJSON_TOK_INVALID when there is no such member.
int mcpc_json_member (const char *obj, size_t obj_len, const char *key, const char **val, size_t *val_len);

// Is the value token (of MJSON_TOK_* type `type`, as mjson_find reports it) one we may copy verbatim into
// our own output? mjson is lenient about what it calls a string or number (raw control characters, invalid
// UTF-8, "1e", "-"), copying such a token would make our reply invalid JSON. Only strings, numbers, true,
// false and null can pass, and only in strict JSON form.
bool mcpc_json_token_clean (int type, const char *tok, size_t len);

#endif
