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

// getaddrinfo, MSG_NOSIGNAL and friends are not visible under strict -std=c11/c23 without these
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if is_win
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0		// macOS has SO_NOSIGPIPE instead, set on every accepted socket
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

bool
mcpc_net_init (void)
{
#if is_win
  static bool done = false;
  if (!done)
    {
      WSADATA wsa;
      if (WSAStartup (MAKEWORD (2, 2), &wsa) != 0)
	return false;
      done = true;
    }
#endif
  return true;
}

void
mcpc_net_sleep_ms (int ms)
{
#if is_win
  Sleep ((DWORD) ms);
#else
  poll (nullptr, 0, ms);
#endif
}

void
mcpc_net_close (mcpc_fd_t fd)
{
#if is_win
  closesocket (fd);
#else
  close (fd);
#endif
}

static int
_poll1 (mcpc_fd_t fd, short events, int timeout_ms)
{
#if is_win
  WSAPOLLFD p = { fd, events, 0 };
  int n = WSAPoll (&p, 1, timeout_ms);
#else
  struct pollfd p = { fd, events, 0 };
  int n;
  do
    n = poll (&p, 1, timeout_ms);
  while (n < 0 && errno == EINTR);
#endif
  return n > 0 ? (int) p.revents : n;
}

int
mcpc_net_wait_readable (mcpc_fd_t fd, int timeout_ms)
{
  const int ev = _poll1 (fd, POLLIN, timeout_ms);
  if (ev < 0)
    return -1;
  return ev != 0 ? 1 : 0;
}

// is_loopback: every address of the listening socket is a loopback one
static bool
_addr_is_loopback (const struct sockaddr *sa)
{
  if (sa->sa_family == AF_INET)
    return (ntohl (((const struct sockaddr_in *) sa)->sin_addr.s_addr) >> 24) == 127;
  if (sa->sa_family == AF_INET6)
    {
      const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *) sa;
      return IN6_IS_ADDR_LOOPBACK (&s6->sin6_addr);
    }
  return false;
}

mcpc_fd_t
mcpc_net_listen (const char *host, unsigned short port, unsigned short *bound_port, bool *loopback)
{
  if (!mcpc_net_init ())
    return MCPC_FD_INVALID;

  char portstr[8];
  snprintf (portstr, sizeof (portstr), "%u", (unsigned) port);

  struct addrinfo hints, *res = nullptr;
  memset (&hints, 0, sizeof (hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  if (getaddrinfo (host != nullptr ? host : "127.0.0.1", portstr, &hints, &res) != 0 || res == nullptr)
    return MCPC_FD_INVALID;

  mcpc_fd_t fd = MCPC_FD_INVALID;
  for (struct addrinfo *ai = res; ai != nullptr; ai = ai->ai_next)
    {
      fd = socket (ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd == MCPC_FD_INVALID)
	continue;
#if !is_win
      // SO_REUSEADDR on Windows would allow stealing the port of a running server
      int one = 1;
      setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof (one));
#endif
      if (bind (fd, ai->ai_addr, (socklen_t) ai->ai_addrlen) == 0 && listen (fd, 64) == 0)
	{
	  if (loopback != nullptr)
	    *loopback = _addr_is_loopback (ai->ai_addr);
	  struct sockaddr_storage ss;
	  socklen_t sl = sizeof (ss);
	  if (bound_port != nullptr)
	    {
	      *bound_port = port;
	      if (getsockname (fd, (struct sockaddr *) &ss, &sl) == 0)
		*bound_port = ntohs (ss.ss_family == AF_INET6 ? ((struct sockaddr_in6 *) &ss)->sin6_port :
				     ((struct sockaddr_in *) &ss)->sin_port);
	    }
	  break;
	}
      mcpc_net_close (fd);
      fd = MCPC_FD_INVALID;
    }
  freeaddrinfo (res);
  return fd;
}

mcpc_fd_t
mcpc_net_accept (mcpc_fd_t lfd, int timeout_ms)
{
  if (_poll1 (lfd, POLLIN, timeout_ms) <= 0)
    return MCPC_FD_INVALID;
  mcpc_fd_t fd = accept (lfd, nullptr, nullptr);
  if (fd == MCPC_FD_INVALID)
    return MCPC_FD_INVALID;
  int one = 1;
  setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof (one));
#ifdef SO_NOSIGPIPE
  setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *) &one, sizeof (one));
#endif
  return fd;
}

long
mcpc_net_read (mcpc_fd_t fd, void *buf, size_t n, int timeout_ms)
{
  if (timeout_ms >= 0)
    {
      const int ev = _poll1 (fd, POLLIN, timeout_ms);
      if (ev == 0)
	return -2;
      if (ev < 0)
	return -1;
    }
#if is_win
  const int got = recv (fd, (char *) buf, (int) n, 0);
#else
  ssize_t got;
  do
    got = recv (fd, buf, n, 0);
  while (got < 0 && errno == EINTR);
#endif
  return got < 0 ? -1 : (long) got;
}

bool
mcpc_net_write (mcpc_fd_t fd, const void *buf, size_t n)
{
  const char *p = buf;
  while (n > 0)
    {
#if is_win
      const int w = send (fd, p, (int) (n > 1u << 20 ? 1u << 20 : n), 0);
#else
      const ssize_t w = send (fd, p, n, MSG_NOSIGNAL);
      if (w < 0 && errno == EINTR)
	continue;
#endif
      if (w <= 0)
	return false;
      p += w;
      n -= (size_t) w;
    }
  return true;
}

bool
mcpc_net_peer_gone (mcpc_fd_t fd)
{
  if (_poll1 (fd, POLLIN, 0) <= 0)
    return false;		// nothing to read, so the peer has not closed
  char c;
#if is_win
  const int got = recv (fd, &c, 1, MSG_PEEK);
  return got == 0 || (got < 0 && WSAGetLastError () != WSAEWOULDBLOCK && WSAGetLastError () != WSAEMSGSIZE);
#else
  const ssize_t got = recv (fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
  // data waiting is a pipelined request, not a hang up
  return got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
#endif
}

void
mcpc_net_set_send_timeout (mcpc_fd_t fd, int timeout_ms)
{
#if is_win
  DWORD t = (DWORD) timeout_ms;
  setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, (const char *) &t, sizeof (t));
#else
  struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
  setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv));
#endif
}
