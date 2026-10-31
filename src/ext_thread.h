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

#ifndef h_ext_thread
#define h_ext_thread

// Start a detached thread running fn(arg). Returns false if it could not be created.

#if is_win
#include <stdlib.h>
#include <winsock2.h>
typedef struct
{
  void (*fn) (void *);
  void *arg;
} mcpc_thr_start_t;

static inline DWORD WINAPI
mcpc_thr_tramp (LPVOID p)
{
  mcpc_thr_start_t s = *(mcpc_thr_start_t *) p;
  free (p);
  s.fn (s.arg);
  return 0;
}

static inline bool
mcpc_thread_start (void (*fn) (void *), void *arg)
{
  mcpc_thr_start_t *s = malloc (sizeof (*s));
  if (s == 0)
    return false;
  s->fn = fn;
  s->arg = arg;
  HANDLE h = CreateThread (0, 0, mcpc_thr_tramp, s, 0, 0);
  if (h == 0)
    {
      free (s);
      return false;
    }
  CloseHandle (h);
  return true;
}
#else
#include <pthread.h>
#include <stdlib.h>

typedef struct
{
  void (*fn) (void *);
  void *arg;
} mcpc_thr_start_t;

static inline void *
mcpc_thr_tramp (void *p)
{
  mcpc_thr_start_t s = *(mcpc_thr_start_t *) p;
  free (p);
  s.fn (s.arg);
  return 0;
}

static inline bool
mcpc_thread_start (void (*fn) (void *), void *arg)
{
  mcpc_thr_start_t *s = malloc (sizeof (*s));
  if (s == 0)
    return false;
  s->fn = fn;
  s->arg = arg;
  pthread_t t;
  if (pthread_create (&t, 0, mcpc_thr_tramp, s) != 0)
    {
      free (s);
      return false;
    }
  pthread_detach (t);
  return true;
}
#endif

#endif
