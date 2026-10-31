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

#ifndef h_net
#define h_net

#include <stddef.h>

#include <mcpc/_c23_keywords.h>

#if is_win
#include <winsock2.h>
typedef SOCKET mcpc_fd_t;
#define MCPC_FD_INVALID INVALID_SOCKET
#else
typedef int mcpc_fd_t;
#define MCPC_FD_INVALID (-1)
#endif

// Thin portable layer over BSD / Winsock sockets: just what the HTTP transport needs.

// Initialize the socket library (Winsock), harmless elsewhere. Returns false on failure.
bool mcpc_net_init (void);

// Listen on host:port (port 0 picks a free one, *bound_port reports the real one). host may be a name
// or an address; NULL means 127.0.0.1. Returns MCPC_FD_INVALID on failure.
mcpc_fd_t mcpc_net_listen (const char *host, unsigned short port, unsigned short *bound_port, bool *loopback);

// Wait up to timeout_ms for a connection. Returns the new socket, or MCPC_FD_INVALID on timeout or error.
mcpc_fd_t mcpc_net_accept (mcpc_fd_t lfd, int timeout_ms);

// Read at most n bytes, waiting up to timeout_ms for the first one. Returns the count, 0 at end of
// stream, -1 on error and -2 on timeout.
long mcpc_net_read (mcpc_fd_t fd, void *buf, size_t n, int timeout_ms);

// Write everything or fail. Returns false when the peer is gone or does not drain within send timeout.
bool mcpc_net_write (mcpc_fd_t fd, const void *buf, size_t n);

// Has the peer closed (or reset) its end? Never blocks and never consumes data.
bool mcpc_net_peer_gone (mcpc_fd_t fd);

// Wait for the socket to become readable (data, end of stream or error).
// Returns 1 when readable, 0 on timeout, -1 on error.
int mcpc_net_wait_readable (mcpc_fd_t fd, int timeout_ms);

void mcpc_net_close (mcpc_fd_t fd);

void mcpc_net_sleep_ms (int ms);

// bound on how long a single write may stall, in milliseconds
void mcpc_net_set_send_timeout (mcpc_fd_t fd, int timeout_ms);

#endif
