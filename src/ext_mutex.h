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

#ifndef h_ext_mutex
#define h_ext_mutex

// Minimal portable mutex, enough to serialize request handling per server.

#if is_win
#include <winsock2.h>
typedef CRITICAL_SECTION mcpc_mutex_t;
static inline void mcpc_mutex_init (mcpc_mutex_t * m) { InitializeCriticalSection (m); }
static inline void mcpc_mutex_lock (mcpc_mutex_t * m) { EnterCriticalSection (m); }
static inline void mcpc_mutex_unlock (mcpc_mutex_t * m) { LeaveCriticalSection (m); }
static inline void mcpc_mutex_destroy (mcpc_mutex_t * m) { DeleteCriticalSection (m); }
#else
#include <pthread.h>
typedef pthread_mutex_t mcpc_mutex_t;
static inline void mcpc_mutex_init (mcpc_mutex_t * m) { pthread_mutex_init (m, 0); }
static inline void mcpc_mutex_lock (mcpc_mutex_t * m) { pthread_mutex_lock (m); }
static inline void mcpc_mutex_unlock (mcpc_mutex_t * m) { pthread_mutex_unlock (m); }
static inline void mcpc_mutex_destroy (mcpc_mutex_t * m) { pthread_mutex_destroy (m); }
#endif

#endif
