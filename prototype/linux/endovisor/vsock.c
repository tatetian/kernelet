// SPDX-License-Identifier: GPL-2.0
// The experimental broker owns channel storage independently of sandbox heaps.
//
// The mutex protects metadata, never a payload copy or a trip into image code.
// A reservation pins its bytes through the owning endpoint's lifetime.
// Forced teardown drops that endpoint only after every carrier is gone.
// The other endpoint keeps both rings alive even after its peer is destroyed.
#include <linux/anon_inodes.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>

#include "kernelet.h"
#include "uapi/klet-vsock.h"

#define KVS_ENDPOINTS 128

struct kv_endpoint;

// Each ring has one producer reservation and one consumer reservation.
struct kv_ring {
	u64 head, tail;
	u64 tx_lease, rx_lease;
	u64 waiters[2];	// consumer, producer; protected by `kv_lock`
	u8 data[KLET_VSOCK_CAPACITY];
};

struct kv_connection {
	struct kv_endpoint *end[2];
	bool reset[2];
	struct kv_ring ring[2];
};

struct kv_endpoint {
	u64 id, cid, port;
	kuid_t uid;
	struct klet_inst *owner;
	struct kv_connection *conn;
	unsigned int side, backlog, nr_pending;
	struct kv_endpoint *pending[KLET_VSOCK_BACKLOG];
	bool listening, rd_closed, wr_closed;
	u32 test_delay_ms;
	u32 test_rx_delay_ms;
};

static DEFINE_MUTEX(kv_lock);
static DECLARE_WAIT_QUEUE_HEAD(kv_wait);
static DECLARE_WAIT_QUEUE_HEAD(kv_poll_wait);
static atomic64_t kv_generation = ATOMIC64_INIT(0);
static atomic64_t kv_cids = ATOMIC64_INIT(0);
static struct kv_endpoint *kv_endpoints[KVS_ENDPOINTS];
static u64 kv_next_id;
static unsigned int kv_live_connections;
static unsigned int kv_optimize;

// Selects a read-only policy for controlled notification ablations.
static int kv_set_mode(const char *value, const struct kernel_param *param)
{
	unsigned int mode;
	int ret = kstrtouint(value, 0, &mode);

	if (ret || mode > 2)
		return -EINVAL;
	*(unsigned int *)param->arg = mode;
	return 0;
}

static const struct kernel_param_ops kv_mode_ops = {
	.set = kv_set_mode,
	.get = param_get_uint,
};
module_param_cb(klet_vsock_opt, &kv_mode_ops, &kv_optimize, 0444);
MODULE_PARM_DESC(klet_vsock_opt, "notifications: 0 baseline, 1 no reserve wakes, 2 demand plus separate poll queue");
static atomic64_t kv_sleeps = ATOMIC64_INIT(0);
static atomic64_t kv_committed = ATOMIC64_INIT(0);
static bool kv_profile;
module_param_named(klet_vsock_profile, kv_profile, bool, 0444);
MODULE_PARM_DESC(klet_vsock_profile, "collect diagnostic counters and elapsed intervals (default off)");

// Diagnostic state is written only while holding `kv_lock`.
static struct {
	u64 calls[11], step_ns[11], wakes[11];
	u64 lookups, comparisons, lookup_ns, lock_ns;
	u64 waits, slept, skipped, wait_ns, retry_eagain;
	u64 host_copy_ns, host_copy_bytes, poll_notifies;
} kv_prof;

// Notifies waiters after metadata changes, including an atomic-context kill.
void klet_vsock_wake(void)
{
	atomic64_inc(&kv_generation);
	wake_up_all(&kv_wait);
	if (kv_optimize == 2)
		wake_up_all(&kv_poll_wait);
}

// Maps a send or receive operation to the same ring at every call site.
static struct kv_ring *kv_data_ring(struct kv_endpoint *ep, bool tx)
{
	return &ep->conn->ring[ep->side ^ !tx];
}

// Notifies after successful locked operations without weakening terminal wakes.
static void kv_notify_success(struct kv_endpoint *ep, const struct klet_vsock_req *r)
{
	struct kv_ring *ring;

	if (kv_optimize && (r->op == KVS_RESERVE_TX || r->op == KVS_RESERVE_RX))
		return;
	if (kv_optimize == 2 && (r->op == KVS_COMMIT_TX || r->op == KVS_COMMIT_RX)) {
		ring = kv_data_ring(ep, r->op == KVS_COMMIT_TX);
		// Either commit releases a lease as well as changing ring positions.
		// Zero-prefix cancellation can enable a same-direction waiter or EOF.
		if (ring->waiters[0] || ring->waiters[1]) {
			if (kv_profile)
				kv_prof.wakes[r->op]++;
			atomic64_inc(&kv_generation);
			wake_up_all(&kv_wait);
		}
		if (kv_profile)
			kv_prof.poll_notifies++;
		wake_up_all(&kv_poll_wait);
		return;
	}
	if (kv_profile && r->op < ARRAY_SIZE(kv_prof.wakes))
		kv_prof.wakes[r->op]++;
	klet_vsock_wake();
}

u64 klet_vsock_new_cid(void)
{
	s64 old;

	for (;;) {
		old = atomic64_read(&kv_cids);
		if (old == S64_MAX)
			return 0;
		if (atomic64_cmpxchg(&kv_cids, old, old + 1) == old)
			return old + 1;
	}
}

static bool kv_dead(struct kv_endpoint *ep)
{
	return ep->owner && READ_ONCE(ep->owner->dying);
}

static struct kv_endpoint *kv_lookup(struct klet_inst *owner, u64 id)
{
	unsigned int i;
	u64 start = kv_profile ? ktime_get_ns() : 0;
	struct kv_endpoint *found = NULL;

	for (i = 0; i < KVS_ENDPOINTS; i++) {
		if (kv_endpoints[i] && kv_endpoints[i]->id == id &&
		    kv_endpoints[i]->owner == owner) {
			found = kv_endpoints[i];
			break;
		}
	}
	if (kv_profile) {
		kv_prof.lookups++;
		kv_prof.comparisons += found ? i + 1 : KVS_ENDPOINTS;
		kv_prof.lookup_ns += ktime_get_ns() - start;
	}
	return found;
}

// Allocates under the metadata mutex, with a fixed global and per-owner bound.
static struct kv_endpoint *kv_new(struct klet_inst *owner, kuid_t uid)
{
	unsigned int i, count = 0, slot = KVS_ENDPOINTS;
	struct kv_endpoint *ep;

	for (i = 0; i < KVS_ENDPOINTS; i++) {
		if (!kv_endpoints[i])
			slot = i;
		else if (kv_endpoints[i]->owner == owner && uid_eq(kv_endpoints[i]->uid, uid))
			count++;
	}
	if (slot == KVS_ENDPOINTS || count >= KLET_VSOCK_OWNER_LIMIT ||
	    kv_next_id == S64_MAX)
		return ERR_PTR(-ENOSPC);
	ep = kzalloc(sizeof(*ep), GFP_KERNEL_ACCOUNT);
	if (!ep)
		return ERR_PTR(-ENOMEM);
	ep->id = ++kv_next_id;
	ep->cid = owner ? owner->vsock_cid : KLET_VSOCK_HOST;
	ep->uid = uid;
	ep->owner = owner;
	if (owner)
		klet_inst_get(owner);
	kv_endpoints[slot] = ep;
	return ep;
}

// Drops one endpoint; the connection survives until its other endpoint drops.
static void kv_drop(struct kv_endpoint *ep, bool reset)
{
	unsigned int i;
	struct kv_connection *conn = ep->conn;

	while (ep->nr_pending)
		kv_drop(ep->pending[--ep->nr_pending], true);
	if (conn) {
		conn->reset[ep->side] = reset;
		conn->end[ep->side] = NULL;
		if (!conn->end[ep->side ^ 1]) {
			WARN_ON_ONCE(conn->ring[0].waiters[0] || conn->ring[0].waiters[1] ||
				     conn->ring[1].waiters[0] || conn->ring[1].waiters[1]);
			kvfree(conn);
			kv_live_connections--;
		}
	}
	for (i = 0; i < KVS_ENDPOINTS; i++)
		if (kv_endpoints[i] == ep)
			kv_endpoints[i] = NULL;
	if (ep->owner)
		klet_inst_put(ep->owner);
	kfree(ep);
	klet_vsock_wake();
}

// Reaps image handles after no carrier can still access a reserved span.
void klet_vsock_gone(struct klet_inst *inst)
{
	unsigned int i, j;

	mutex_lock(&kv_lock);
	// A pending endpoint belongs to its listener until accepted.
	// Remove it from that queue before the general owner sweep.
	for (i = 0; i < KVS_ENDPOINTS; i++) {
		struct kv_endpoint *ep = kv_endpoints[i];

		if (!ep || ep->owner != inst)
			continue;
		for (j = 0; j < ep->nr_pending; j++)
			kv_drop(ep->pending[j], true);
		ep->nr_pending = 0;
	}
	for (i = 0; i < KVS_ENDPOINTS; i++)
		if (kv_endpoints[i] && kv_endpoints[i]->owner == inst)
			kv_drop(kv_endpoints[i], true);
	mutex_unlock(&kv_lock);
}

static bool kv_has_permission(struct klet_inst *inst, u64 cid)
{
	unsigned int i;

	for (i = 0; i < inst->vsock_nr_allow; i++)
		if (inst->vsock_allow[i] == cid)
			return true;
	return false;
}

long klet_vsock_permit(struct klet_inst *a, struct klet_inst *b)
{
	long ret = 0;
	u64 cid = b ? b->vsock_cid : 0;
	bool add_a, add_b;

	mutex_lock(&kv_lock);
	add_a = !kv_has_permission(a, cid);
	add_b = b && !kv_has_permission(b, a->vsock_cid);
	if (a == b)
		ret = -EINVAL;
	else if (READ_ONCE(a->dying) || (b && READ_ONCE(b->dying)))
		ret = -ESHUTDOWN;
	else if ((add_a && a->vsock_nr_allow == KLET_VSOCK_OWNER_LIMIT) ||
		 (add_b && b->vsock_nr_allow == KLET_VSOCK_OWNER_LIMIT))
		ret = -ENOSPC;
	else {
		if (add_a)
			a->vsock_allow[a->vsock_nr_allow++] = cid;
		if (add_b)
			b->vsock_allow[b->vsock_nr_allow++] = a->vsock_cid;
	}
	mutex_unlock(&kv_lock);
	return ret;
}

static bool kv_authorized(struct kv_endpoint *a, struct kv_endpoint *b)
{
	return uid_eq(a->uid, b->uid) && a->cid != b->cid &&
	       (!a->owner || kv_has_permission(a->owner, b->cid)) &&
	       (!b->owner || kv_has_permission(b->owner, a->cid));
}

// Returns only a contiguous prefix, so ring wraparound is a partial transfer.
static long kv_reserve(struct kv_endpoint *ep, struct klet_vsock_req *r, bool tx)
{
	struct kv_connection *c = ep->conn;
	struct kv_endpoint *peer;
	struct kv_ring *ring;
	u64 available, offset, *lease;

	if (!c)
		return -ENOTCONN;
	peer = c->end[ep->side ^ 1];
	ring = kv_data_ring(ep, tx);
	lease = tx ? &ring->tx_lease : &ring->rx_lease;
	if (tx) {
		if (ep->wr_closed || !peer || kv_dead(peer) || peer->rd_closed)
			return peer && kv_dead(peer) ? -ECONNRESET : -EPIPE;
		if (*lease)
			return -EAGAIN;
		available = KLET_VSOCK_CAPACITY - (ring->tail - ring->head);
		offset = ring->tail & (KLET_VSOCK_CAPACITY - 1);
	} else {
		if (ep->rd_closed)
			return 0;
		if (*lease)
			return -EAGAIN;
		available = ring->tail - ring->head;
		offset = ring->head & (KLET_VSOCK_CAPACITY - 1);
		// A write admitted before shutdown may still publish its prefix.
		// Do not expose EOF until that producer reservation resolves.
		if (!available && peer && !kv_dead(peer) && peer->wr_closed && ring->tx_lease)
			return -EAGAIN;
		if (!available && (!peer || kv_dead(peer) || peer->wr_closed))
			return c->reset[ep->side ^ 1] || (peer && kv_dead(peer)) ? -ECONNRESET : 0;
	}
	if (!available)
		return -EAGAIN;
	*lease = min3(available, r->arg0, (u64)KLET_VSOCK_CAPACITY - offset);
	r->addr = (u64)(ring->data + offset);
	r->len = *lease;
	return *lease;
}

static long kv_step(struct kv_endpoint *ep, struct klet_vsock_req *r)
{
	struct kv_endpoint *listener = NULL, *accepted;
	struct kv_connection *c;
	struct kv_ring *ring;
	unsigned int i;
	bool tx;
	u64 *lease;

	if (kv_dead(ep))
		return -ESHUTDOWN;
	switch (r->op) {
	case KVS_BIND:
		if (!r->arg0 || r->arg0 > U32_MAX || ep->port || ep->conn || ep->listening)
			return -EINVAL;
		for (i = 0; i < KVS_ENDPOINTS; i++)
			if (kv_endpoints[i] && kv_endpoints[i]->cid == ep->cid &&
			    uid_eq(kv_endpoints[i]->uid, ep->uid) &&
			    kv_endpoints[i]->port == r->arg0)
				return -EADDRINUSE;
		ep->port = r->arg0;
		return 0;
	case KVS_LISTEN:
		if (!ep->port || ep->conn || !r->arg0 || r->arg0 > KLET_VSOCK_BACKLOG)
			return -EINVAL;
		ep->listening = true;
		ep->backlog = r->arg0;
		return 0;
	case KVS_CONNECT:
		if (ep->conn || ep->listening || !r->arg1 || r->arg1 > U32_MAX)
			return -EINVAL;
		for (i = 0; i < KVS_ENDPOINTS; i++) {
			struct kv_endpoint *p = kv_endpoints[i];

			if (p && p->listening && p->cid == r->arg0 && p->port == r->arg1 &&
			    uid_eq(p->uid, ep->uid) && !kv_dead(p))
				listener = p;
		}
		if (!listener)
			return -ECONNREFUSED;
		if (!kv_authorized(ep, listener))
			return -EACCES;
		if (listener->nr_pending >= listener->backlog)
			return -EAGAIN;
		if (kv_live_connections >= KLET_VSOCK_CONNECTIONS)
			return -ENOSPC;
		c = kvzalloc(sizeof(*c), GFP_KERNEL_ACCOUNT);
		if (!c)
			return -ENOMEM;
		accepted = kv_new(listener->owner, listener->uid);
		if (IS_ERR(accepted)) {
			kvfree(c);
			return PTR_ERR(accepted);
		}
		ep->conn = accepted->conn = c;
		ep->side = 0;
		accepted->side = 1;
		c->end[0] = ep;
		c->end[1] = accepted;
		listener->pending[listener->nr_pending++] = accepted;
		kv_live_connections++;
		return 0;
	case KVS_ACCEPT:
		if (!ep->listening)
			return -EINVAL;
		if (!ep->nr_pending)
			return -EAGAIN;
		accepted = ep->pending[0];
		for (i = 1; i < ep->nr_pending; i++)
			ep->pending[i - 1] = ep->pending[i];
		ep->nr_pending--;
		return accepted->id;
	case KVS_SHUTDOWN:
		if (!ep->conn || r->arg0 > 2)
			return -EINVAL;
		if (r->arg0 != 1)
			ep->rd_closed = true;
		if (r->arg0 != 0)
			ep->wr_closed = true;
		return 0;
	case KVS_RESERVE_TX:
	case KVS_RESERVE_RX:
		if (!r->arg0)
			return 0;
		return kv_reserve(ep, r, r->op == KVS_RESERVE_TX);
	case KVS_COMMIT_TX:
	case KVS_COMMIT_RX:
		if (!ep->conn)
			return -ENOTCONN;
		tx = r->op == KVS_COMMIT_TX;
		ring = kv_data_ring(ep, tx);
		lease = tx ? &ring->tx_lease : &ring->rx_lease;
		if (!*lease || r->arg0 > *lease)
			return -EINVAL;
		if (tx) {
			ring->tail += r->arg0;
			atomic64_add(r->arg0, &kv_committed);
		} else {
			ring->head += r->arg0;
		}
		*lease = 0;
		return 0;
	default:
		return -EINVAL;
	}
}

// Tests the condition and snapshots its generation under the same mutex.
// A publish between unlocking and sleeping therefore cannot be lost.
static long kv_request(struct kv_endpoint *ep, struct klet_vsock_req *r)
{
	long ret;
	s64 generation;
	u64 start, switches = 0;
	bool retry = false;
	u64 *demand;

	for (;;) {
		generation = atomic64_read(&kv_generation);
		start = kv_profile ? ktime_get_ns() : 0;
		ret = kv_step(ep, r);
		if (kv_profile && r->op < ARRAY_SIZE(kv_prof.calls)) {
			kv_prof.calls[r->op]++;
			kv_prof.step_ns[r->op] += ktime_get_ns() - start;
			kv_prof.retry_eagain += retry && ret == -EAGAIN;
		}
		if (ret != -EAGAIN || r->nonblock)
			break;
		demand = NULL;
		if (r->op == KVS_RESERVE_TX || r->op == KVS_RESERVE_RX) {
			bool tx = r->op == KVS_RESERVE_TX;

			demand = &kv_data_ring(ep, tx)->waiters[tx];
			(*demand)++;
		}
		if (kv_profile) {
			kv_prof.waits++;
			start = ktime_get_ns();
			switches = current->nvcsw;
		}
		mutex_unlock(&kv_lock);
		atomic64_inc(&kv_sleeps);
		ret = wait_event_interruptible(kv_wait,
			atomic64_read(&kv_generation) != generation || kv_dead(ep));
		mutex_lock(&kv_lock);
		// The endpoint's active file/carrier pins this counter through relock.
		if (demand) {
			if (WARN_ON_ONCE(!*demand))
				ret = -EIO;
			else
				(*demand)--;
		}
		if (kv_profile) {
			kv_prof.wait_ns += ktime_get_ns() - start;
			if (current->nvcsw != switches)
				kv_prof.slept++;
			else
				kv_prof.skipped++;
		}
		if (ret)
			break;
		retry = true;
	}
	if (ret >= 0)
		kv_notify_success(ep, r);
	return ret;
}

// The image service uses owner-scoped handles; image pointers never name peers.
s64 klet_vsock_service(struct klet_inst *inst, struct klet_vsock_req *r)
{
	struct kv_endpoint *ep;
	s64 ret;
	u64 start = kv_profile ? ktime_get_ns() : 0;

	mutex_lock(&kv_lock);
	if (kv_profile)
		kv_prof.lock_ns += ktime_get_ns() - start;
	if (READ_ONCE(inst->dying)) {
		ret = -ESHUTDOWN;
	} else if (r->op == KVS_SOCKET) {
		ep = kv_new(inst, inst->vsock_uid);
		ret = IS_ERR(ep) ? PTR_ERR(ep) : ep->id;
	} else if (!(ep = kv_lookup(inst, r->handle))) {
		ret = -EBADF;
	} else if (r->op == KVS_CLOSE) {
		kv_drop(ep, false);
		ret = 0;
	} else {
		ret = kv_request(ep, r);
	}
	mutex_unlock(&kv_lock);
	return ret;
}

static const struct file_operations kv_fops;

static int kv_install(struct kv_endpoint *ep)
{
	int fd = anon_inode_getfd("[klet-vsock]", &kv_fops, ep, O_RDWR | O_CLOEXEC);

	if (fd < 0)
		kv_drop(ep, false);
	return fd;
}

long klet_vsock_socket(void)
{
	struct kv_endpoint *ep;
	long ret;

	mutex_lock(&kv_lock);
	ep = kv_new(NULL, current_euid());
	ret = IS_ERR(ep) ? PTR_ERR(ep) : kv_install(ep);
	mutex_unlock(&kv_lock);
	return ret;
}

static long kv_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct kv_endpoint *ep = file->private_data;
	struct klet_vsock_req r;
	long ret;

	if (cmd == KLET_VSOCK_TEST_WAITERS) {
		struct klet_vsock_waiters state;

		if (!klet_test_hooks)
			return -EOPNOTSUPP;
		mutex_lock(&kv_lock);
		if (!ep->conn) {
			mutex_unlock(&kv_lock);
			return -ENOTCONN;
		}
		for (unsigned int i = 0; i < 2; i++) {
			state.consumer[i] = ep->conn->ring[i].waiters[0];
			state.producer[i] = ep->conn->ring[i].waiters[1];
		}
		mutex_unlock(&kv_lock);
		return copy_to_user((void __user *)arg, &state, sizeof(state)) ? -EFAULT : 0;
	}
	if (cmd == KLET_VSOCK_TEST_DELAY || cmd == KLET_VSOCK_TEST_RX_DELAY) {
		u32 delay;

		if (!klet_test_hooks)
			return -EOPNOTSUPP;
		if (get_user(delay, (u32 __user *)arg))
			return -EFAULT;
		if (delay > 1000)
			return -EINVAL;
		if (cmd == KLET_VSOCK_TEST_DELAY)
			WRITE_ONCE(ep->test_delay_ms, delay);
		else
			WRITE_ONCE(ep->test_rx_delay_ms, delay);
		return 0;
	}
	if (cmd != KLET_VSOCK_CTL)
		return -ENOTTY;
	if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
		return -EFAULT;
	if (r.op < KVS_BIND || r.op > KVS_SHUTDOWN || r.nonblock > 1)
		return -EINVAL;
	mutex_lock(&kv_lock);
	ret = kv_request(ep, &r);
	if (ret >= 0 && r.op == KVS_ACCEPT)
		ret = kv_install(kv_lookup(NULL, ret));
	mutex_unlock(&kv_lock);
	return ret;
}

// Payload copies run without the broker mutex, directly on reserved ring bytes.
static ssize_t kv_io(struct kiocb *iocb, struct iov_iter *iter, bool tx)
{
	struct kv_endpoint *ep = iocb->ki_filp->private_data;
	struct klet_vsock_req r = {
		.op = tx ? KVS_RESERVE_TX : KVS_RESERVE_RX,
		.arg0 = iov_iter_count(iter),
		.nonblock = !!(iocb->ki_filp->f_flags & O_NONBLOCK),
	};
	long ret;
	size_t done;
	u64 start = kv_profile ? ktime_get_ns() : 0;
	u64 copied_ns = 0;
	u32 delay;

	mutex_lock(&kv_lock);
	if (kv_profile)
		kv_prof.lock_ns += ktime_get_ns() - start;
	ret = kv_request(ep, &r);
	mutex_unlock(&kv_lock);
	if (ret <= 0)
		return ret;
	delay = tx ? READ_ONCE(ep->test_delay_ms) : READ_ONCE(ep->test_rx_delay_ms);
	if (delay)
		msleep(delay);
	start = kv_profile ? ktime_get_ns() : 0;
	done = tx ? copy_from_iter((void *)r.addr, r.len, iter) :
		    copy_to_iter((void *)r.addr, r.len, iter);
	if (kv_profile) {
		copied_ns = ktime_get_ns() - start;
		start = ktime_get_ns();
	}
	r.op = tx ? KVS_COMMIT_TX : KVS_COMMIT_RX;
	r.arg0 = done;
	mutex_lock(&kv_lock);
	if (kv_profile) {
		kv_prof.lock_ns += ktime_get_ns() - start;
		kv_prof.host_copy_ns += copied_ns;
		kv_prof.host_copy_bytes += done;
		kv_prof.calls[r.op]++;
	}
	start = kv_profile ? ktime_get_ns() : 0;
	ret = kv_step(ep, &r);
	if (kv_profile)
		kv_prof.step_ns[r.op] += ktime_get_ns() - start;
	if (ret >= 0)
		kv_notify_success(ep, &r);
	mutex_unlock(&kv_lock);
	if (ret)
		return ret;
	return done ? done : -EFAULT;
}

static ssize_t kv_read(struct kiocb *iocb, struct iov_iter *iter)
{
	return kv_io(iocb, iter, false);
}

static ssize_t kv_write(struct kiocb *iocb, struct iov_iter *iter)
{
	return kv_io(iocb, iter, true);
}

static __poll_t kv_poll(struct file *file, poll_table *wait)
{
	struct kv_endpoint *ep = file->private_data, *peer;
	__poll_t mask = 0;

	poll_wait(file, kv_optimize == 2 ? &kv_poll_wait : &kv_wait, wait);
	mutex_lock(&kv_lock);
	if (ep->listening) {
		if (ep->nr_pending)
			mask |= EPOLLIN;
	} else if (ep->conn) {
		struct kv_ring *rx = &ep->conn->ring[ep->side ^ 1];
		struct kv_ring *tx = &ep->conn->ring[ep->side];

		peer = ep->conn->end[ep->side ^ 1];
		if ((!rx->rx_lease && rx->tail != rx->head) || ep->rd_closed || !peer || kv_dead(peer) ||
		    (peer->wr_closed && !rx->tx_lease))
			mask |= EPOLLIN;
		if (!peer || kv_dead(peer))
			mask |= EPOLLHUP;
		else if (!ep->wr_closed && !peer->rd_closed && !tx->tx_lease &&
			 tx->tail - tx->head < KLET_VSOCK_CAPACITY)
			mask |= EPOLLOUT;
	}
	mutex_unlock(&kv_lock);
	return mask;
}

static int kv_release(struct inode *inode, struct file *file)
{
	mutex_lock(&kv_lock);
	kv_drop(file->private_data, false);
	mutex_unlock(&kv_lock);
	return 0;
}

static const struct file_operations kv_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = kv_ioctl,
	.read_iter = kv_read,
	.write_iter = kv_write,
	.poll = kv_poll,
	.release = kv_release,
	.llseek = noop_llseek,
};

void klet_vsock_report(void)
{
	unsigned int i, live = 0;

	for (i = 0; i < KVS_ENDPOINTS; i++)
		live += !!kv_endpoints[i];
	pr_info("kernelet: VSOCK: %u endpoints live, %u connections live, %lld waits, %lld bytes published, notification_mode=%u\n",
		live, kv_live_connections, atomic64_read(&kv_sleeps), atomic64_read(&kv_committed), kv_optimize);
}

long klet_vsock_profile_report(bool reset)
{
	unsigned int i;

	if (!kv_profile)
		return -EOPNOTSUPP;
	mutex_lock(&kv_lock);
	if (reset) {
		memset(&kv_prof, 0, sizeof(kv_prof));
	} else {
		pr_info("kernelet: VSOCK-PROFILE lookups=%llu comparisons=%llu lookup_ns=%llu lock_ns=%llu waits=%llu slept=%llu skipped=%llu wait_ns=%llu retry_eagain=%llu host_copy_ns=%llu host_copy_bytes=%llu poll_notifies=%llu mode=%u\n",
			kv_prof.lookups, kv_prof.comparisons, kv_prof.lookup_ns, kv_prof.lock_ns,
			kv_prof.waits, kv_prof.slept, kv_prof.skipped, kv_prof.wait_ns,
			kv_prof.retry_eagain, kv_prof.host_copy_ns, kv_prof.host_copy_bytes, kv_prof.poll_notifies, kv_optimize);
		for (i = 0; i < ARRAY_SIZE(kv_prof.calls); i++)
			pr_info("kernelet: VSOCK-OP op=%u calls=%llu step_ns=%llu wakes=%llu\n",
				i, kv_prof.calls[i], kv_prof.step_ns[i], kv_prof.wakes[i]);
	}
	mutex_unlock(&kv_lock);
	return 0;
}
