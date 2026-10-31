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

#ifndef h_ext_atomic
#define h_ext_atomic

// A flag that one thread (or a signal handler) sets and others poll. Plain volatile would not order the
// accesses, so use the compiler's atomic builtins where there are any.

#if defined(__GNUC__) || defined(__clang__)
#define mcpc_flag_get(p) __atomic_load_n ((p), __ATOMIC_ACQUIRE)
#define mcpc_flag_set(p, v) __atomic_store_n ((p), (v), __ATOMIC_RELEASE)
#elif is_win
#include <windows.h>
#define mcpc_flag_get(p) InterlockedCompareExchange ((volatile LONG *) (p), 0, 0)
#define mcpc_flag_set(p, v) InterlockedExchange ((volatile LONG *) (p), (LONG) (v))
#else
#define mcpc_flag_get(p) (*(volatile int *) (p))
#define mcpc_flag_set(p, v) (*(volatile int *) (p) = (v))
#endif

#endif
