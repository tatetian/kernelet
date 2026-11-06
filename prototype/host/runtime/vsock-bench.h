// SPDX-License-Identifier: GPL-2.0
// The comparison protocol brackets timed work with untimed control tokens.
// Virtio sessions reuse their stream; other transports use a control stream.
#ifndef KLET_VSOCK_BENCH_H
#define KLET_VSOCK_BENCH_H

#include <errno.h>
#include <linux/vm_sockets.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#define VB_PORT 58473
#define VB_MAGIC 0x4b564231u
#define VB_CAPACITY 65536u
#define VB_BEGIN 0x42454749u
#define VB_END 0x454e4421u

// Both experimental programs are x86-64; this is not a portable wire protocol.
struct vb_header {
	uint32_t magic, transport, size, rounds, warmup, repeat;
};

struct vb_result {
	uint64_t peer_cpu_ns, qemu_cpu_ns, vhost_cpu_ns, outer_wall_ns;
	uint64_t vhost_tasks, buffer_bytes;
};

static inline int vb_full(int fd, void *data, size_t len, int output)
{
	unsigned char *p = data;

	while (len) {
		ssize_t n = output ? write(fd, p, len) : read(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static inline int vb_buffers(int fd)
{
	uint64_t size = VB_CAPACITY, actual;
	socklen_t length = sizeof(actual);

	if (setsockopt(fd, AF_VSOCK, SO_VM_SOCKETS_BUFFER_SIZE, &size, sizeof(size)) ||
	    setsockopt(fd, AF_VSOCK, SO_VM_SOCKETS_BUFFER_MIN_SIZE, &size, sizeof(size)) ||
	    setsockopt(fd, AF_VSOCK, SO_VM_SOCKETS_BUFFER_MAX_SIZE, &size, sizeof(size)) ||
	    getsockopt(fd, AF_VSOCK, SO_VM_SOCKETS_BUFFER_SIZE, &actual, &length))
		return -1;
	if (actual != size) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

#endif
