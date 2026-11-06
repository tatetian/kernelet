// SPDX-License-Identifier: GPL-2.0
// These wrappers expose socket lifecycle operations over the prototype adapter.
#ifndef KLET_VSOCK_RUNTIME_H
#define KLET_VSOCK_RUNTIME_H

#include <sys/ioctl.h>
#include <klet-vsock.h>

static inline int kv_socket(int system)
{
	return ioctl(system, KLET_VSOCK_SOCKET);
}

static inline int kv_control(int fd, unsigned int op, unsigned long long a,
			     unsigned long long b, int nonblock)
{
	struct klet_vsock_req request = {
		.op = op, .arg0 = a, .arg1 = b, .nonblock = nonblock,
	};

	return ioctl(fd, KLET_VSOCK_CTL, &request);
}

static inline int kv_bind(int fd, unsigned int port)
{
	return kv_control(fd, KVS_BIND, port, 0, 0);
}

static inline int kv_listen(int fd, unsigned int backlog)
{
	return kv_control(fd, KVS_LISTEN, backlog, 0, 0);
}

static inline int kv_connect(int fd, unsigned long long cid, unsigned int port)
{
	return kv_control(fd, KVS_CONNECT, cid, port, 0);
}

static inline int kv_accept(int fd)
{
	return kv_control(fd, KVS_ACCEPT, 0, 0, 0);
}

static inline int kv_shutdown(int fd, unsigned int how)
{
	return kv_control(fd, KVS_SHUTDOWN, how, 0, 0);
}

#endif
