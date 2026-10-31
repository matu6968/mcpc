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

// Minimal portable recursive mutex, enough to serialize request handling per server.

#if is_win
#include <winsock2.h>
// The same owner + depth scheme as the POSIX branch below, so both platforms behave alike (a
// CRITICAL_SECTION could be entered again by its owner, but would not tell how deeply).
typedef struct
{
  CRITICAL_SECTION mu;		// the lock proper
  CRITICAL_SECTION guard;	// protects owner and depth
  DWORD owner;			// valid only while depth > 0
  unsigned depth;
} mcpc_mutex_t;

static inline void
mcpc_mutex_init (mcpc_mutex_t * m)
{
  InitializeCriticalSection (&m->mu);
  InitializeCriticalSection (&m->guard);
  m->depth = 0;
}

static inline void
mcpc_mutex_lock (mcpc_mutex_t * m)
{
  EnterCriticalSection (&m->guard);
  if (m->depth > 0 && m->owner == GetCurrentThreadId ())
    {
      m->depth++;
      LeaveCriticalSection (&m->guard);
      return;
    }
  LeaveCriticalSection (&m->guard);

  EnterCriticalSection (&m->mu);
  EnterCriticalSection (&m->guard);
  m->owner = GetCurrentThreadId ();
  m->depth = 1;
  LeaveCriticalSection (&m->guard);
}

static inline void
mcpc_mutex_unlock (mcpc_mutex_t * m)
{
  EnterCriticalSection (&m->guard);
  const unsigned left = --m->depth;
  LeaveCriticalSection (&m->guard);
  if (left == 0)
    LeaveCriticalSection (&m->mu);
}

static inline void
mcpc_mutex_destroy (mcpc_mutex_t * m)
{
  DeleteCriticalSection (&m->mu);
  DeleteCriticalSection (&m->guard);
}

// Give the lock up completely, however deeply it is held, and take it back later to the same depth.
static inline unsigned
mcpc_mutex_release_all (mcpc_mutex_t * m)
{
  EnterCriticalSection (&m->guard);
  const unsigned d = m->depth;
  m->depth = 0;
  LeaveCriticalSection (&m->guard);
  LeaveCriticalSection (&m->mu);
  return d;
}

static inline void
mcpc_mutex_restore (mcpc_mutex_t * m, unsigned depth)
{
  EnterCriticalSection (&m->mu);
  EnterCriticalSection (&m->guard);
  m->owner = GetCurrentThreadId ();
  m->depth = depth;
  LeaveCriticalSection (&m->guard);
}

typedef struct
{
  CONDITION_VARIABLE cv;
  CRITICAL_SECTION mu;
} mcpc_cond_t;
static inline void mcpc_cond_init (mcpc_cond_t * c) { InitializeConditionVariable (&c->cv); InitializeCriticalSection (&c->mu); }
static inline void mcpc_cond_lock (mcpc_cond_t * c) { EnterCriticalSection (&c->mu); }
static inline void mcpc_cond_unlock (mcpc_cond_t * c) { LeaveCriticalSection (&c->mu); }
static inline void mcpc_cond_broadcast (mcpc_cond_t * c) { WakeAllConditionVariable (&c->cv); }
// wait with the cond lock held, for a broadcast or until ms have passed
static inline void mcpc_cond_wait_ms (mcpc_cond_t * c, unsigned ms) { SleepConditionVariableCS (&c->cv, &c->mu, ms); }
static inline void mcpc_cond_destroy (mcpc_cond_t * c) { DeleteCriticalSection (&c->mu); }
#else
#include <pthread.h>
#include <time.h>
// Recursion is done by hand (owner + depth) because PTHREAD_MUTEX_RECURSIVE needs a feature-test
// macro that strict -std=c23/c11 builds do not define. owner and depth are only touched while
// holding the small guard mutex, so no thread ever reads them mid-update.
typedef struct
{
  pthread_mutex_t mu;		// the lock proper
  pthread_mutex_t guard;	// protects owner and depth
  pthread_t owner;		// valid only while depth > 0
  unsigned depth;
} mcpc_mutex_t;

static inline void
mcpc_mutex_init (mcpc_mutex_t * m)
{
  pthread_mutex_init (&m->mu, 0);
  pthread_mutex_init (&m->guard, 0);
  m->depth = 0;
}

static inline void
mcpc_mutex_lock (mcpc_mutex_t * m)
{
  pthread_mutex_lock (&m->guard);
  if (m->depth > 0 && pthread_equal (m->owner, pthread_self ()))
    {
      m->depth++;
      pthread_mutex_unlock (&m->guard);
      return;
    }
  pthread_mutex_unlock (&m->guard);

  pthread_mutex_lock (&m->mu);
  pthread_mutex_lock (&m->guard);
  m->owner = pthread_self ();
  m->depth = 1;
  pthread_mutex_unlock (&m->guard);
}

static inline void
mcpc_mutex_unlock (mcpc_mutex_t * m)
{
  pthread_mutex_lock (&m->guard);
  const unsigned left = --m->depth;
  pthread_mutex_unlock (&m->guard);
  if (left == 0)
    pthread_mutex_unlock (&m->mu);
}

static inline void
mcpc_mutex_destroy (mcpc_mutex_t * m)
{
  pthread_mutex_destroy (&m->mu);
  pthread_mutex_destroy (&m->guard);
}

// Give the lock up completely, however deeply it is held, and take it back later to the same depth.
// Needed to wait for something another thread has to do while holding the same lock.
static inline unsigned
mcpc_mutex_release_all (mcpc_mutex_t * m)
{
  pthread_mutex_lock (&m->guard);
  const unsigned d = m->depth;
  m->depth = 0;
  pthread_mutex_unlock (&m->guard);
  pthread_mutex_unlock (&m->mu);
  return d;
}

static inline void
mcpc_mutex_restore (mcpc_mutex_t * m, unsigned depth)
{
  pthread_mutex_lock (&m->mu);
  pthread_mutex_lock (&m->guard);
  m->owner = pthread_self ();
  m->depth = depth;
  pthread_mutex_unlock (&m->guard);
}

typedef struct
{
  pthread_mutex_t mu;
  pthread_cond_t cv;
} mcpc_cond_t;

static inline void
mcpc_cond_init (mcpc_cond_t * c)
{
  pthread_mutex_init (&c->mu, 0);
  pthread_cond_init (&c->cv, 0);
}

static inline void mcpc_cond_lock (mcpc_cond_t * c) { pthread_mutex_lock (&c->mu); }
static inline void mcpc_cond_unlock (mcpc_cond_t * c) { pthread_mutex_unlock (&c->mu); }
static inline void mcpc_cond_broadcast (mcpc_cond_t * c) { pthread_cond_broadcast (&c->cv); }

// wait with the cond lock held, for a broadcast or until ms have passed
static inline void
mcpc_cond_wait_ms (mcpc_cond_t * c, unsigned ms)
{
  // pthread_cond_timedwait takes an absolute time, build it from the monotonic-agnostic realtime clock
  struct timespec ts;
  timespec_get (&ts, TIME_UTC);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long) (ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L)
    {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
    }
  pthread_cond_timedwait (&c->cv, &c->mu, &ts);
}

static inline void
mcpc_cond_destroy (mcpc_cond_t * c)
{
  pthread_mutex_destroy (&c->mu);
  pthread_cond_destroy (&c->cv);
}
#endif

#endif
