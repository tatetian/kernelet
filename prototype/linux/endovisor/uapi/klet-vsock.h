/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
// This experimental socket interface accompanies the Kernelet Vsock proposal.
// Host programs use the lifecycle wrappers in `host/runtime/klet-vsock.h`.
#ifndef KLET_VSOCK_UAPI_H
#define KLET_VSOCK_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define KLET_VSOCK_CAPACITY 65536u
#define KLET_VSOCK_HOST 0ULL
#define KLET_VSOCK_BACKLOG 8u
#define KLET_VSOCK_OWNER_LIMIT 16u
#define KLET_VSOCK_CONNECTIONS 64u

// These operation numbers are also used by the experimental image service.
enum klet_vsock_op {
	KVS_SOCKET, KVS_BIND, KVS_LISTEN, KVS_CONNECT, KVS_ACCEPT,
	KVS_SHUTDOWN, KVS_CLOSE, KVS_RESERVE_TX, KVS_COMMIT_TX,
	KVS_RESERVE_RX, KVS_COMMIT_RX,
};

// The service returns a private reservation in `addr` and `len`.
// Host control ioctls never expose these kernel addresses.
struct klet_vsock_req {
	__u64 handle;
	__u64 arg0;
	__u64 arg1;
	__u64 addr;
	__u64 len;
	__u32 op;
	__u32 nonblock;
};

// The ioctl for creating an unbound host socket endpoint.
//
// Call on the system descriptor with no argument.
// Returns a close-on-exec file descriptor, or `ENOMEM`/`ENOSPC` on exhaustion.
// The endpoint supports `read`, `write`, `poll`, and `KLET_VSOCK_CTL`.
//
// # Security
//
// Connecting requires an explicit host permission on the peer sandbox
// and matching creator effective UIDs; opening alone grants no peer access.
#define KLET_VSOCK_SOCKET _IO(0xEB, 0x40)

// The ioctl for performing a socket control operation.
//
// Call on a host endpoint with `klet_vsock_req`.
// `BIND` takes a nonzero port in `arg0`; `LISTEN` takes backlog 1..8.
// `CONNECT` takes destination CID and port in `arg0` and `arg1`.
// `ACCEPT` returns a new close-on-exec descriptor; other operations return zero.
// `SHUTDOWN` takes 0 (read), 1 (write), or 2 (both) in `arg0`.
// Host programs use `close`, not the internal `CLOSE` operation.
// `nonblock` is 0 or 1; no listener gives `ECONNREFUSED`, a full backlog
// or a nonblocking empty accept gives `EAGAIN`, and denied access gives `EACCES`.
// Invalid operations/state give `EINVAL`, interrupts give `EINTR`,
// and allocation or endpoint exhaustion gives `ENOMEM` or `ENOSPC`.
// A listener's death cancels its queued connections.
#define KLET_VSOCK_CTL _IOW(0xEB, 0x41, struct klet_vsock_req)

// The ioctl for obtaining a sandbox's experimental channel identity.
//
// Call on a sandbox descriptor with a writable `__u64` argument.
// Returns zero and writes its CID, or `EFAULT` for an invalid output pointer.
// CIDs increase without reuse during one module lifetime; zero names the host.
#define KLET_VSOCK_GET_CID _IOR(0xEB, 0x42, __u64)

// The ioctl for authorizing communication with a peer.
//
// Call on a sandbox descriptor with a pointer to a signed file descriptor.
// `-1` permits host endpoints with the sandbox creator's effective UID.
// A sandbox descriptor permits both sandboxes to communicate.
// Returns zero, `EBADF` for an invalid peer descriptor, `EINVAL` for self,
// `ESHUTDOWN` for a dying peer, or `ENOSPC` after 16 permissions per sandbox.
//
// # Security
//
// The runtime must possess the sandbox descriptor and, for a sandbox pair,
// the peer descriptor. Connections also require matching creator effective UIDs.
#define KLET_VSOCK_PERMIT _IOW(0xEB, 0x43, __s32)

// The ioctl for delaying a host write after it reserves ring storage.
//
// Call on an endpoint with a pointer to a delay in milliseconds, from 0 to 1000.
// Returns zero, `EINVAL` for an excessive delay, or `EFAULT` for a bad pointer.
// Each subsequent host write sleeps once after reserving and before copying.
//
// # Security
//
// This bounded fault-injection hook requires the operator to load the module
// with `klet_test_hooks=1`; otherwise the operation returns `EOPNOTSUPP`.
#define KLET_VSOCK_TEST_DELAY _IOW(0xEB, 0x44, __u32)

// The ioctl for resetting optional broker diagnostic counters.
//
// Call on the system descriptor with no argument; returns zero.
// Returns `EOPNOTSUPP` unless the module was loaded with `klet_vsock_profile=1`.
// Use only between isolated diagnostic cases; normal benchmarks disable profiling.
#define KLET_VSOCK_PROFILE_RESET _IO(0xEB, 0x45)

// The ioctl for printing optional broker diagnostic counters to the kernel log.
//
// Call on the system descriptor with no argument; returns zero.
// Returns `EOPNOTSUPP` unless `klet_vsock_profile=1`.
// Timings are elapsed intervals including preemption, not CPU execution time.
#define KLET_VSOCK_PROFILE_REPORT _IO(0xEB, 0x46)

// The ioctl for delaying a host read after reserving its incoming span.
//
// The argument and 1000 ms bound match `KLET_VSOCK_TEST_DELAY`.
// Returns zero, `EINVAL` for an excessive delay, or `EFAULT` for a bad pointer.
//
// # Security
//
// Requires the operator to enable `klet_test_hooks`, or returns `EOPNOTSUPP`.
#define KLET_VSOCK_TEST_RX_DELAY _IOW(0xEB, 0x47, __u32)

struct klet_vsock_waiters {
	__u64 consumer[2];
	__u64 producer[2];
};

// The ioctl for inspecting registered blocking demand on a connection's rings.
//
// Writes four counters to `klet_vsock_waiters` and returns zero.
// Ring zero sends from the connecting endpoint to the accepted endpoint.
// Returns `ENOTCONN` for an unconnected endpoint and `EFAULT` for a bad pointer.
//
// # Security
//
// Requires `klet_test_hooks`, or returns `EOPNOTSUPP`.
// The descriptor authorizes access to this connection only; no pointer is exposed.
#define KLET_VSOCK_TEST_WAITERS _IOR(0xEB, 0x48, struct klet_vsock_waiters)

#endif
