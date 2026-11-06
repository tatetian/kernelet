// SPDX-License-Identifier: GPL-2.0
// The sandbox owns its descriptor, exec descriptors, image mapping, and lifecycle.
//
// `KLET_CREATE` allocates one `struct klet_inst` in the sandbox table.
// The sandbox file and every carrier record hold references to the instance,
// so it outlives whichever of its descriptor or carriers goes last.
//
// Every termination path goes through `klet_end`:
// power-off, `KLET_KILL`, the last descriptor closing, or a live carrier dying.
// The first request records the reason and marks the sandbox dying.
// It sends `SIGKILL` to the carriers and arms the eviction sweep.
// Once the last carrier is gone, `klet_sandbox_exited` finishes teardown,
// wakes blocked readers, and makes the descriptor readable to polling callers.
// The endovisor releases the remaining resources once the sandbox has exited
// and its last descriptor is closed, whichever happens last.
// Closing a live sandbox never waits for its carriers.

#include <linux/anon_inodes.h>
#include <linux/binfmts.h>
#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <asm/tsc.h>

#include "kernelet.h"
#include "uapi/klet-vsock.h"

extern atomic_t klet_sweeping;

/* ------------------------------------------------------------------ */
/* References                                                         */
/* ------------------------------------------------------------------ */

void klet_inst_get(struct klet_inst *inst)
{
	refcount_inc(&inst->refs);
}

void klet_inst_put(struct klet_inst *inst)
{
	if (!refcount_dec_and_test(&inst->refs))
		return;
	/* Destroyed by now: the file is gone, and so is every carrier record. */
	WARN_ON_ONCE(!inst->destroyed && inst->slot >= 0);
	kfree(inst->cmdline);
	kvfree(inst);
}

/* ------------------------------------------------------------------ */
/* The console and the log                                            */
/* ------------------------------------------------------------------ */

/*
 * Called by a service on the carrier's Linux stack, with the kernelet perhaps
 * holding one of its own spin locks: no blocking here. The line goes into the
 * ring whole or not at all (log_dropped counts it), and the work item writes it.
 */
void klet_out_write(struct klet_inst *inst, struct klet_out *out, const char *ptr, u64 len)
{
	unsigned long flags;
	bool ok;

	spin_lock_irqsave(&out->lock, flags);
	ok = kfifo_initialized(&out->fifo) && kfifo_avail(&out->fifo) >= len + 1;
	if (ok) {
		kfifo_in(&out->fifo, ptr, len);
		kfifo_in(&out->fifo, "\n", 1);
	}
	spin_unlock_irqrestore(&out->lock, flags);
	if (!ok) {
		atomic64_inc(&inst->log_dropped);
		return;
	}
	queue_work(klet_wq, &inst->out_work);
}

static void klet_out_drain(struct klet_out *out)
{
	char buf[256];
	unsigned int n;

	if (!out->file || !kfifo_initialized(&out->fifo))
		return;
	for (;;) {
		loff_t pos;

		n = kfifo_out_spinlocked(&out->fifo, buf, sizeof(buf), &out->lock);
		if (!n)
			break;
		/* As write(2) would, without its f_pos lock. */
		pos = out->file->f_pos;
		kernel_write(out->file, buf, n, &pos);
		out->file->f_pos = pos;
	}
}

static void klet_out_work(struct work_struct *work)
{
	struct klet_inst *inst = container_of(work, struct klet_inst, out_work);

	klet_out_drain(&inst->console);
	klet_out_drain(&inst->log);
}

static void klet_out_free(struct klet_out *out)
{
	if (out->file)
		fput(out->file);
	out->file = NULL;
	if (kfifo_initialized(&out->fifo))
		kfifo_free(&out->fifo);
}

static long klet_set_out(struct klet_inst *inst, struct klet_out *out, s32 __user *uarg)
{
	struct file *file;
	s32 fd;
	long err = 0;

	if (get_user(fd, uarg))
		return -EFAULT;
	file = fget(fd);
	if (!file)
		return -EBADF;
	if (!(file->f_mode & FMODE_WRITE)) {
		fput(file);
		return -EBADF;
	}
	mutex_lock(&inst->mutex);
	spin_lock(&inst->lock);
	if (inst->started || inst->dying)
		err = -EBUSY;
	spin_unlock(&inst->lock);
	if (!err && !kfifo_initialized(&out->fifo))
		err = kfifo_alloc(&out->fifo, KLET_OUT_RING, GFP_KERNEL);
	if (!err) {
		struct file *old = out->file;

		WRITE_ONCE(out->file, file);
		file = old;	/* a second KLET_SET_* replaces the first */
	}
	mutex_unlock(&inst->mutex);
	if (file)
		fput(file);
	return err;
}

/* ------------------------------------------------------------------ */
/* KLET_CREATE                                                        */
/* ------------------------------------------------------------------ */

static const struct file_operations klet_sandbox_fops;

long klet_sandbox_create(struct klet_create __user *uarg)
{
	struct klet_create cr;
	struct klet_kind *kind;
	struct klet_inst *inst;
	struct cgroup *cgrp;
	u64 initial, max;
	char *cmdline;
	int slot, fd, err;

	if (copy_from_user(&cr, uarg, sizeof(cr)))
		return -EFAULT;
	if (cr.flags || cr.nr_vcpus < 1 || cr.nr_vcpus > KLET_MAX_VCPUS ||
	    cr.cmdline_len > KLET_CMDLINE_MAX)
		return -EINVAL;
	if (cr.memory_initial > U64_MAX - KLET_GRANT_UNIT ||
	    cr.memory_max > U64_MAX - KLET_GRANT_UNIT)
		return -EINVAL;
	initial = round_up(cr.memory_initial, KLET_GRANT_UNIT);
	max = round_up(cr.memory_max, KLET_GRANT_UNIT);
	if (initial > max)
		return -EINVAL;
	/* The prototype's grant is one contiguous run of at most 4 MiB. */
	if (initial == 0 || initial > KLET_GRANT_MAX)
		return -EINVAL;

	/* The caller's group on the unified hierarchy; never the root. */
	rcu_read_lock();
	cgrp = task_dfl_cgroup(current);
	if (cgroup_parent(cgrp))
		cgroup_get(cgrp);
	else
		cgrp = NULL;
	rcu_read_unlock();
	if (!cgrp)
		return -EPERM;

	cmdline = kmalloc(cr.cmdline_len + 1, GFP_KERNEL);
	inst = kvzalloc(sizeof(*inst), GFP_KERNEL);
	err = -ENOMEM;
	if (!cmdline || !inst)
		goto fail;
	err = -EFAULT;
	if (copy_from_user(cmdline, u64_to_user_ptr(cr.cmdline), cr.cmdline_len))
		goto fail;
	cmdline[cr.cmdline_len] = '\0';

	refcount_set(&inst->refs, 1);
	inst->vsock_cid = klet_vsock_new_cid();
	if (!inst->vsock_cid) {
		err = -ENOSPC;
		goto fail;
	}
	inst->vsock_uid = current_euid();
	inst->slot = -1;
	mutex_init(&inst->mutex);
	spin_lock_init(&inst->lock);
	init_waitqueue_head(&inst->arrive_wq);
	init_waitqueue_head(&inst->exit_wq);
	INIT_LIST_HEAD(&inst->carriers);
	INIT_LIST_HEAD(&inst->kstack_all);
	INIT_WORK(&inst->out_work, klet_out_work);
	spin_lock_init(&inst->console.lock);
	spin_lock_init(&inst->log.lock);
	klet_heap_init(inst);
	inst->nr_vcpus = cr.nr_vcpus;
	inst->memory_initial = initial;
	inst->memory_max = max;
	inst->max_tasks = cr.max_tasks;
	inst->kstack_max = min_t(u64, cr.max_tasks, UINT_MAX - KLET_MAX_VCPUS) + cr.nr_vcpus;
	inst->cmdline = cmdline;
	cmdline = NULL;
	inst->cgrp = cgrp;
	inst->boot.cmdline_ptr = (u64)(unsigned long)inst->cmdline;
	inst->boot.cmdline_len = cr.cmdline_len ? strlen(inst->cmdline) : 0;
	err = klet_spaces_init(inst);
	if (err)
		goto fail;

	mutex_lock(&klet_table_lock);
	kind = cr.kind < KLET_MAX_KINDS ? &klet_kinds[cr.kind] : NULL;
	if (!kind || !kind->used)
		err = -ENOENT;
	else {
		err = -ENOSPC;
		for (slot = 0; slot < KLET_MAX_SANDBOXES; slot++)
			if (!klet_sandboxes[slot]) {
				err = 0;
				break;
			}
	}
	if (!err) {
		inst->kind = kind;
		inst->slot = slot;
		kind->nr_sandboxes++;
		klet_sandboxes[slot] = inst;
		atomic_inc(&klet_totals.sandboxes);
	}
	mutex_unlock(&klet_table_lock);
	if (err) {
		klet_spaces_free(inst);
		kfree(inst->mem_stats);
		goto fail;
	}

	/* The owner of every address space the kernelet will create. */
	inst->owner = get_task_struct(current->group_leader);

	fd = anon_inode_getfd("[endovisor-sandbox]", &klet_sandbox_fops, inst,
			      O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		/* As if it had been created, killed at once and released. */
		inst->released = true;
		klet_end(inst, KLET_END_KILLED, 0, KLET_KILLED_REQUESTED);
		klet_inst_put(inst);
		return fd;
	}
	pr_info("kernelet: sandbox %d created: kind %u \"%s\", %u vcpu(s), memory %llu KiB (max %llu KiB), max_tasks %llu, cmdline \"%s\"\n",
		inst->slot, cr.kind, kind->name, inst->nr_vcpus, initial >> 10, max >> 10,
		inst->max_tasks, inst->cmdline);
	return fd;

fail:
	cgroup_put(cgrp);
	kfree(cmdline);
	if (inst) {
		kfree(inst->cmdline);
		kvfree(inst);
	}
	return err;
}

/* ------------------------------------------------------------------ */
/* Exec descriptors                                                   */
/* ------------------------------------------------------------------ */

/*
 * One file per virtual CPU, in memory only: shmem's, owned by root and
 * execute-only, so that it can be executed but neither read nor written by
 * anyone without CAP_DAC_OVERRIDE. Its first bytes are a magic, the slot and
 * the virtual CPU, which klet_load_binary() reads only to turn other files
 * away quickly.
 *
 * The endovisor keeps the file shmem_file_setup() opened, read-write, to write
 * those bytes. What it hands out is never that file: each descriptor is an
 * O_PATH open of the same inode, which execveat(fd, "", AT_EMPTY_PATH)
 * executes (the exec opens the inode afresh, for execution, and checks the
 * execute bit) and on which read(2), write(2) and fchmod(2) fail (EBADF): a
 * descriptor on the read-write file would let its holder read and write the
 * inode whatever its mode says.
 */
static struct file *klet_exec_file_make(struct klet_inst *inst, unsigned int vcpu)
{
	struct klet_exec_head head = { .slot = inst->slot, .vcpu = vcpu };
	struct inode *inode;
	struct file *file;
	loff_t pos = 0;
	ssize_t n;

	memcpy(head.magic, KLET_EXEC_MAGIC, KLET_EXEC_MAGIC_LEN);
	file = shmem_file_setup("klet-exec", PAGE_SIZE, 0);
	if (IS_ERR(file))
		return file;
	n = kernel_write(file, &head, sizeof(head), &pos);
	if (n != sizeof(head)) {
		fput(file);
		return ERR_PTR(n < 0 ? n : -EIO);
	}
	inode = file_inode(file);
	inode_lock(inode);
	inode->i_uid = GLOBAL_ROOT_UID;
	inode->i_gid = GLOBAL_ROOT_GID;
	inode->i_mode = S_IFREG | 0111;
	inode_unlock(inode);
	return file;
}

static long klet_get_vcpu_exec_fd(struct klet_inst *inst, u32 __user *uarg)
{
	struct file *file;
	u32 vcpu;
	long err = 0;
	int fd;

	if (get_user(vcpu, uarg))
		return -EFAULT;
	if (vcpu >= inst->nr_vcpus)
		return -EINVAL;
	mutex_lock(&inst->mutex);
	spin_lock(&inst->lock);
	if (inst->dying)
		err = -ESRCH;
	else if (inst->vslot[vcpu].claimed)
		err = -EBUSY;
	spin_unlock(&inst->lock);
	if (err)
		goto out;
	file = inst->vslot[vcpu].exec_file;
	if (!file) {
		file = klet_exec_file_make(inst, vcpu);
		if (IS_ERR(file)) {
			err = PTR_ERR(file);
			goto out;
		}
		WRITE_ONCE(inst->vslot[vcpu].exec_file, file);
	}
	file = dentry_open(&file->f_path, O_PATH, current_cred());
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto out;
	}
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(file);
		err = fd;
		goto out;
	}
	fd_install(fd, file);
	err = fd;
out:
	mutex_unlock(&inst->mutex);
	return err;
}

/*
 * The sandbox and the virtual CPU an exec is of, by the identity of the file
 * executed: its inode, since the exec opened the file afresh. A file with the
 * same bytes elsewhere is not one; nor is the file of a sandbox destroyed
 * since. Returns the instance with a reference, or NULL.
 */
struct klet_inst *klet_exec_lookup(struct linux_binprm *bprm, unsigned int *vcpu)
{
	const struct klet_exec_head *head = (const void *)bprm->buf;
	struct klet_inst *inst = NULL;
	struct file *exec_file;

	if (head->slot >= KLET_MAX_SANDBOXES)
		return NULL;
	mutex_lock(&klet_table_lock);
	inst = klet_sandboxes[head->slot];
	if (inst && head->vcpu < inst->nr_vcpus) {
		exec_file = READ_ONCE(inst->vslot[head->vcpu].exec_file);
		if (exec_file && file_inode(exec_file) == file_inode(bprm->file)) {
			klet_inst_get(inst);
			*vcpu = head->vcpu;
		} else {
			inst = NULL;
		}
	} else {
		inst = NULL;
	}
	mutex_unlock(&klet_table_lock);
	return inst;
}

/* The checks of an exec, in the header's order; on success the vCPU is the caller's. */
int klet_exec_claim(struct klet_inst *inst, unsigned int vcpu)
{
	struct cgroup *cgrp;
	int err = 0;

	rcu_read_lock();
	cgrp = task_dfl_cgroup(current);
	rcu_read_unlock();
	spin_lock(&inst->lock);
	if (inst->dying)
		err = -ESRCH;
	else if (inst->vslot[vcpu].claimed)
		err = -EBUSY;
	else if (cgrp != inst->cgrp || !task_no_new_privs(current))
		err = -EPERM;
	else
		inst->vslot[vcpu].claimed = true;
	spin_unlock(&inst->lock);
	if (err)
		pr_info("kernelet: sandbox %d: an exec of vcpu %u's descriptor by pid %d refused: %d\n",
			inst->slot, vcpu, current->pid, err);
	return err;
}

/* begin_new_exec() failed: the exec never happened. */
void klet_exec_unclaim(struct klet_inst *inst, unsigned int vcpu)
{
	spin_lock(&inst->lock);
	inst->vslot[vcpu].claimed = false;
	spin_unlock(&inst->lock);
}

/* ------------------------------------------------------------------ */
/* The start                                                          */
/* ------------------------------------------------------------------ */

// Prepare the image, initial grant, and boot arguments for the first carrier.
//
// Map the kind's immutable pages with private data for this instance.
// Charge the private image pages and `memory_initial` grant to the sandbox's group:
// the carrier is in that group, as the exec handler checked.
// No kernelet code runs until every carrier has arrived,
// and only then does vCPU 0 enter the image.
int klet_sandbox_start(struct klet_inst *inst)
{
	struct klet_kind *kind = inst->kind;
	struct page *pages;
	unsigned int order = get_order(inst->memory_initial);
	int err;

	err = klet_image_start(inst);
	if (err)
		return err;
	pages = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_NOWARN, order);
	if (!pages)
		return -ENOMEM;
	inst->grant_pages = pages;
	inst->grant_order = order;

	inst->boot.abi_version = KLET_ABI_VERSION;
	inst->boot.grant_paddr = page_to_phys(pages);
	inst->boot.grant_len = PAGE_SIZE << order;
	inst->boot.linear_map_base = (u64)(unsigned long)__va(0);
	inst->boot.nr_vcpus = inst->nr_vcpus;
	inst->boot.fpu_area_size = klet_fpu_area_size();
	inst->boot.fpu_area_align = 64;
	inst->boot.tsc_khz = tsc_khz;
	inst->boot.mirror_preempt = klet_mirror ? 1 : 0;
	inst->boot.image_text_lo = inst->text_lo;
	inst->boot.image_text_hi = inst->text_hi;
	inst->boot.pgtable_levels = pgtable_l5_enabled() ? 5 : 4;
	err = klet_publish_host_offsets(inst);
	if (err)
		return err;
	pr_info("kernelet: sandbox %d started: kind \"%s\" loaded as its instance (%zu bytes of private data), grant %llu KiB at %#llx, charged to its control group\n",
		inst->slot, kind->name, kind->data_size, inst->boot.grant_len >> 10,
		inst->boot.grant_paddr);
	return 0;
}

/* ------------------------------------------------------------------ */
/* The end                                                            */
/* ------------------------------------------------------------------ */

/* With inst->lock held: whether the caller is the one to run the exit. */
void klet_check_exited_locked(struct klet_inst *inst, bool *claim)
{
	if (inst->dying && !inst->exit_claimed &&
	    atomic_read(&inst->nr_carriers) == 0) {
		inst->exit_claimed = true;
		*claim = true;
	}
}

static const char *klet_reason_name(u32 reason)
{
	switch (reason) {
	case KLET_KILLED_REQUESTED:
		return "its last descriptor closed";
	case KLET_KILLED_CARRIER_LOST:
		return "a carrier is gone";
	default:
		return "killed by its holder";
	}
}

/*
 * Every end of a sandbox: record why, if nothing has yet, mark it dying, kill
 * every carrier and arm the eviction sweep; and if no carrier is left, it has
 * exited.
 */
void klet_end(struct klet_inst *inst, u32 how, s32 code, u32 reason)
{
	bool first_mark, claim = false;
	int left;

	spin_lock(&inst->lock);
	if (!inst->exit.how) {
		inst->exit.how = how;
		inst->exit.code = how == KLET_END_EXITED ? code : 0;
		inst->exit.reason = how == KLET_END_KILLED ? reason : 0;
	}
	first_mark = !inst->dying;
	WRITE_ONCE(inst->dying, true);
	klet_check_exited_locked(inst, &claim);
	left = atomic_read(&inst->nr_carriers);
	spin_unlock(&inst->lock);

	if (first_mark) {
		if (how == KLET_END_KILLED)
			pr_info("kernelet: sandbox %d: %s, the sandbox is dying; %d carrier(s) left\n",
				inst->slot, klet_reason_name(reason), left);
		klet_kill_carriers(inst);
		klet_evict_arm(inst);
	}
	if (claim)
		klet_sandbox_exited(inst);
}

static void klet_destroy_work(struct work_struct *work);

/* No carrier is left, and none can come: the sandbox has exited. */
void klet_sandbox_exited(struct klet_inst *inst)
{
	bool armed, released;

	klet_sandbox_gone(inst);

	spin_lock(&inst->lock);
	armed = inst->sweep_armed;
	inst->sweep_armed = false;
	spin_unlock(&inst->lock);
	if (armed)
		atomic_dec(&klet_sweeping);

	if (inst->exit.how == KLET_END_EXITED)
		pr_info("kernelet: sandbox %d exited: the kernel proper powered off, exit code %d\n",
			inst->slot, inst->exit.code);
	else
		pr_info("kernelet: sandbox %d exited: killed, reason %#x (%s)\n",
			inst->slot, inst->exit.reason, klet_reason_name(inst->exit.reason));

	spin_lock(&inst->lock);
	WRITE_ONCE(inst->exited, true);
	released = inst->released;
	spin_unlock(&inst->lock);
	wake_up_all(&inst->exit_wq);

	/* Its last descriptor is closed already: nobody else will destroy it. */
	if (released) {
		klet_inst_get(inst);
		INIT_WORK(&inst->destroy_work, klet_destroy_work);
		queue_work(klet_wq, &inst->destroy_work);
	}
}

/* Folds the sandbox's counters into the module's totals. */
static void klet_fold_counters(struct klet_inst *inst)
{
	struct klet_totals *t = &klet_totals;
	atomic_t *from = (atomic_t *)&inst->n, *to = (atomic_t *)&t->n;
	atomic64_t *mfrom = (atomic64_t *)inst->mem_stats, *mto = (atomic64_t *)&t->mem;
	unsigned int i, f;

	for (i = 0; i < sizeof(inst->n) / sizeof(atomic_t); i++)
		atomic_add(atomic_read(&from[i]), &to[i]);
	if (mfrom)
		for (i = 0; i < sizeof(t->mem) / sizeof(atomic64_t); i++)
			atomic64_add(atomic64_read(&mfrom[i]), &mto[i]);
	if (atomic_read(&inst->longest_defer_us) > atomic_read(&t->longest_defer_us))
		atomic_set(&t->longest_defer_us, atomic_read(&inst->longest_defer_us));
	if (atomic_read(&inst->longest_tick_gap_us) > atomic_read(&t->longest_tick_gap_us))
		atomic_set(&t->longest_tick_gap_us, atomic_read(&inst->longest_tick_gap_us));
	for (f = 0; f < min(atomic_read(&inst->forced_recorded), KLET_FORCED_RECORDS); f++) {
		int j = atomic_inc_return(&t->forced_recorded) - 1;

		if (j < KLET_FORCED_RECORDS)
			t->forced_rec[j] = inst->forced_rec[f];
	}
	atomic64_add(atomic64_read(&inst->heap_allocs), &t->heap_allocs);
	atomic64_add(atomic64_read(&inst->heap_frees), &t->heap_frees);
	atomic64_add(atomic64_read(&inst->heap_reaped), &t->heap_reaped);
	atomic64_add(atomic64_read(&inst->heap_bad_frees), &t->heap_bad_frees);
	atomic_add(inst->kstack_total, &t->kstack_total);
	atomic_add(inst->kstack_returned, &t->kstack_returned);
}

// Release remaining resources after exit and the last descriptor close.
static long klet_sandbox_destroy(struct klet_inst *inst)
{
	unsigned int i;
	long err = 0;

	mutex_lock(&inst->mutex);
	if (!READ_ONCE(inst->exited)) {
		err = -EBUSY;
		goto out;
	}
	if (inst->destroyed)
		goto out;

	/* The console and the log: what is in the rings goes out first. */
	flush_work(&inst->out_work);
	klet_out_free(&inst->console);
	klet_out_free(&inst->log);
	if (inst->grant_pages)
		__free_pages(inst->grant_pages, inst->grant_order);
	inst->grant_pages = NULL;
	klet_image_stop(inst);
	for (i = 0; i < inst->nr_vcpus; i++)
		if (inst->vslot[i].exec_file) {
			fput(inst->vslot[i].exec_file);
			WRITE_ONCE(inst->vslot[i].exec_file, NULL);
		}
	klet_fold_counters(inst);
	klet_spaces_free(inst);
	kfree(inst->mem_stats);
	inst->mem_stats = NULL;
	cgroup_put(inst->cgrp);
	inst->cgrp = NULL;
	if (inst->owner) {	/* klet_spaces_destroy() put it, if the sandbox ever started */
		put_task_struct(inst->owner);
		inst->owner = NULL;
	}

	mutex_lock(&klet_table_lock);
	klet_sandboxes[inst->slot] = NULL;
	inst->kind->nr_sandboxes--;
	mutex_unlock(&klet_table_lock);
	inst->destroyed = true;
	pr_info("kernelet: sandbox %d destroyed\n", inst->slot);
out:
	mutex_unlock(&inst->mutex);
	return err;
}

static void klet_destroy_work(struct work_struct *work)
{
	struct klet_inst *inst = container_of(work, struct klet_inst, destroy_work);

	klet_sandbox_destroy(inst);
	klet_inst_put(inst);
}

/* ------------------------------------------------------------------ */
/* The sandbox descriptor                                             */
/* ------------------------------------------------------------------ */

static u32 klet_sandbox_state(struct klet_inst *inst)
{
	if (inst->exited)
		return KLET_STATE_EXITED;
	if (inst->dying)
		return KLET_STATE_DYING;
	if (inst->arrived == inst->nr_vcpus)
		return KLET_STATE_RUNNING;
	if (inst->started)
		return KLET_STATE_STARTING;
	return KLET_STATE_CREATED;
}

static long klet_get_stats(struct klet_inst *inst, struct klet_stats __user *uarg)
{
	struct klet_stats st;
	struct klet_vcpu_stat __user *rows;
	unsigned int i, n;

	if (copy_from_user(&st, uarg, sizeof(st)))
		return -EFAULT;
	rows = u64_to_user_ptr(st.vcpus);
	n = min(st.capacity, inst->nr_vcpus);
	for (i = 0; i < n; i++) {
		struct klet_vcpu_stat row = { };
		struct klet_carrier *c;

		spin_lock(&inst->lock);
		row.pid = inst->vslot[i].pid;
		row.state = inst->vslot[i].state;
		row.run_ns = inst->vslot[i].run_ns;
		row.stolen_ns = inst->vslot[i].stolen_ns;
		c = inst->vcpus[i];
		if (c && c->task) {
			row.run_ns = READ_ONCE(c->task->se.sum_exec_runtime);
#ifdef CONFIG_SCHED_INFO
			row.stolen_ns = READ_ONCE(c->task->sched_info.run_delay);
#endif
		}
		spin_unlock(&inst->lock);
		if (copy_to_user(&rows[i], &row, sizeof(row)))
			return -EFAULT;
	}
	spin_lock(&inst->lock);
	st.state = klet_sandbox_state(inst);
	memset(&st.exit, 0, sizeof(st.exit));
	if (inst->exited)
		st.exit = inst->exit;
	spin_unlock(&inst->lock);
	st.nr_vcpus = inst->nr_vcpus;
	st.memory_granted = inst->grant_pages ? PAGE_SIZE << inst->grant_order : 0;
	st.service_calls = atomic64_read(&inst->service_calls);
	st.virqs_raised = atomic_read(&inst->n.virqs);
	st.log_dropped = atomic64_read(&inst->log_dropped);
	st.slot = inst->slot;
	if (copy_to_user(uarg, &st, sizeof(st)))
		return -EFAULT;
	return 0;
}

static long klet_sandbox_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct klet_inst *inst = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case KLET_VSOCK_GET_CID:
		return put_user(inst->vsock_cid, (u64 __user *)uarg);
	case KLET_VSOCK_PERMIT: {
		struct file *peer;
		s32 fd;
		long ret;

		if (get_user(fd, (s32 __user *)uarg))
			return -EFAULT;
		if (fd == -1)
			return klet_vsock_permit(inst, NULL);
		peer = fget(fd);
		if (!peer)
			return -EBADF;
		ret = peer->f_op == &klet_sandbox_fops ?
			klet_vsock_permit(inst, peer->private_data) : -EBADF;
		fput(peer);
		return ret;
	}
	case KLET_GET_VCPU_EXEC_FD:
		return klet_get_vcpu_exec_fd(inst, uarg);
	case KLET_ATTACH_DEVICE:
		return -EOPNOTSUPP;	// No devices in this prototype.
	case KLET_SET_CONSOLE:
		return klet_set_out(inst, &inst->console, uarg);
	case KLET_SET_LOG:
		return klet_set_out(inst, &inst->log, uarg);
	case KLET_KILL: {
		u32 reason;

		if (get_user(reason, (u32 __user *)uarg))
			return -EFAULT;
		if (reason & 0x80000000u)
			return -EINVAL;
		klet_end(inst, KLET_END_KILLED, 0, reason);
		return 0;
	}
	case KLET_GET_STATS:
		return klet_get_stats(inst, uarg);
	default:
		return -ENOTTY;
	}
}

// Wait for exit and return the immutable record without consuming it.
static ssize_t klet_sandbox_read(struct file *file, char __user *buf,
				size_t count, loff_t *pos)
{
	struct klet_inst *inst = file->private_data;
	struct klet_exit exit;
	int err;

	if (!count)
		return 0;
	if (count < sizeof(exit))
		return -EINVAL;
	if ((file->f_flags & O_NONBLOCK) && !READ_ONCE(inst->exited))
		return -EAGAIN;
	err = wait_event_interruptible(inst->exit_wq, READ_ONCE(inst->exited));
	if (err)
		return err;
	spin_lock(&inst->lock);
	exit = inst->exit;
	spin_unlock(&inst->lock);
	if (copy_to_user(buf, &exit, sizeof(exit)))
		return -EFAULT;
	return sizeof(exit);
}

static __poll_t klet_sandbox_poll(struct file *file, struct poll_table_struct *pt)
{
	struct klet_inst *inst = file->private_data;

	poll_wait(file, &inst->exit_wq, pt);
	return READ_ONCE(inst->exited) ? EPOLLIN | EPOLLRDNORM : 0;
}

/*
 * The last copy of the descriptor is closed: kill the sandbox if it has not
 * exited; destroy it now if it has, or have its exit destroy it later. Never
 * waits for a carrier.
 */
static int klet_sandbox_release(struct inode *inode, struct file *file)
{
	struct klet_inst *inst = file->private_data;
	bool exited;

	/*
	 * The kill first, and `released` after it: a sandbox with no carrier
	 * exits inside klet_end(), and is then destroyed here, at once, rather
	 * than by the work its exit would queue for a released sandbox.
	 * Exactly one of the two destroys: klet_sandbox_exited() queues the
	 * work only if it finds `released` set, and this destroys only if it
	 * finds `exited` set, both under the lock.
	 */
	klet_end(inst, KLET_END_KILLED, 0, KLET_KILLED_REQUESTED);
	spin_lock(&inst->lock);
	inst->released = true;
	exited = inst->exited;
	spin_unlock(&inst->lock);
	if (exited)
		klet_sandbox_destroy(inst);
	klet_inst_put(inst);
	return 0;
}

static const struct file_operations klet_sandbox_fops = {
	.owner = THIS_MODULE,
	.read = klet_sandbox_read,
	.unlocked_ioctl = klet_sandbox_ioctl,
	.poll = klet_sandbox_poll,
	.release = klet_sandbox_release,
};
