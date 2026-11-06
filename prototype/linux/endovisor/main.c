// SPDX-License-Identifier: GPL-2.0
/*
 * Module init and exit; the device, /dev/endovisor, and its system operations
 * (the kinds: KLET_KIND_REGISTER, KLET_KIND_LIST, KLET_KIND_UNREGISTER;
 * KLET_CREATE is sandbox.c's); the carrier lifeline; the four host offsets the
 * kernelet needs in order to find its own vCPU record and Linux's preemption
 * count without a call; and the binary-format handler that turns a process
 * executing an exec descriptor into the carrier of its virtual CPU, with every
 * signal it can ignore ignored.
 *
 * Also here: the module parameters (klet_mirror; klet_test_hooks and
 * klet_test_sleep_ms, the test hook; klet_watch is in watch.c), the refusal
 * to load on a kernel that emulates the vsyscall page, and the counters
 * printed at unload, the totals of every sandbox, which end with "endovisor
 * unloaded" and which the tests read: the watch timer's ticks and upcalls,
 * the kicks, STATS, MEMSTATS, the kills, the heap, the kernelet stacks and
 * the carrier records.
 */

#include <linux/anon_inodes.h>
#include <linux/binfmts.h>
#include <linux/bitops.h>
#include <linux/capability.h>
#include <linux/cgroup.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kernel_read_file.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/personality.h>
#include <linux/preempt.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/coredump.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/security.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thread_info.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <asm/current.h>
#include <asm/io.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/tsc.h>

#include "kernelet.h"
#include "uapi/klet-vsock.h"

struct klet_totals klet_totals;
struct workqueue_struct *klet_wq;

// The kind and sandbox tables support `KLET_CREATE`, cleanup, and exec lookup.
DEFINE_MUTEX(klet_table_lock);
struct klet_kind klet_kinds[KLET_MAX_KINDS];
struct klet_inst *klet_sandboxes[KLET_MAX_SANDBOXES];

static struct linux_binfmt klet_format;

/*
 * Whether the kernelet mirrors its guard depth into Linux's per-processor
 * preemption count. On is the design; off is the control for the experiment that
 * shows what it buys.
 */
bool klet_mirror = true;
module_param(klet_mirror, bool, 0444);
MODULE_PARM_DESC(klet_mirror,
		 "mirror the kernelet's guard depth into Linux's preemption count (default on)");

/*
 * The test hook, off by default and no part of the design. With
 * klet_test_hooks=1, a vm_map whose protection carries KLET_PROT_TEST_SLEEP
 * sleeps klet_test_sleep_ms in Linux, standing in for direct reclaim inside a
 * page-table allocation, so that the contended-cursor test has a holder that
 * sleeps inside vm_map and a second cursor has to take the range lock's slow
 * path.
 */
bool klet_test_hooks;
module_param(klet_test_hooks, bool, 0444);
MODULE_PARM_DESC(klet_test_hooks, "enable the test hook: test-flagged vm_map calls sleep (default off)");
unsigned int klet_test_sleep_ms = 20;
module_param(klet_test_sleep_ms, uint, 0444);
MODULE_PARM_DESC(klet_test_sleep_ms, "how long a test-flagged vm_map sleeps (default 20 ms)");

/* ------------------------------------------------------------------ */
/* The carrier lifeline                                               */
/* ------------------------------------------------------------------ */

static int klet_lifeline_release(struct inode *inode, struct file *file)
{
	struct klet_carrier *c = file->private_data;

	file->private_data = NULL;
	if (c) {
		klet_carrier_lifeline_closed(c);
		/* The file's own reference (klet_lifeline_create). */
		klet_carrier_put(c);
	}
	return 0;
}

static const struct file_operations klet_lifeline_fops = {
	/* The last fput() may be the last module reference to go. */
	.owner = THIS_MODULE,
	.release = klet_lifeline_release,
	.llseek = noop_llseek,
};

struct file *klet_lifeline_create(struct klet_carrier *c)
{
	struct file *file;

	/* The file's own reference, given back by its release, or here. */
	file = anon_inode_getfile("[kernelet-carrier]", &klet_lifeline_fops,
				  klet_carrier_get(c), O_RDWR);
	if (IS_ERR(file))
		klet_carrier_put(c);
	return file;
}

/* ------------------------------------------------------------------ */
/* The four host offsets vOSTD needs without a service call           */
/* ------------------------------------------------------------------ */

/*
 * `CpuId::current()` is on the hottest path the kernelet has, so it cannot be a
 * service call. What vOSTD does instead is two dependent loads: the host's
 * per-processor current-task pointer through `gs:`, the hooks pointer inside that
 * task, and then the vCPU record, which is embedded in the carrier record the hooks
 * belong to. The mirror is a third: one `inc`/`dec` of the host's preemption count
 * through `gs:`.
 *
 * The `gs:`-relative offsets are derived at run time from the segment base rather
 * than from the linkage value of the per-cpu symbol, and then checked: a wrong
 * offset here would have the kernelet reading and writing arbitrary host memory, so
 * the sandbox does not start if the check fails.
 */
int klet_publish_host_offsets(struct klet_inst *inst)
{
	unsigned long gsbase, hot, off_cur, off_pc;
	struct task_struct *seen;

	preempt_disable();
	rdmsrl(MSR_GS_BASE, gsbase);
	hot = (unsigned long)this_cpu_ptr(&pcpu_hot);
	off_cur = hot - gsbase + offsetof(struct pcpu_hot, current_task);
	off_pc = hot - gsbase + offsetof(struct pcpu_hot, preempt_count);
	seen = *(struct task_struct **)(gsbase + off_cur);
	preempt_enable();

	if (seen != current) {
		pr_err("kernelet: the gs: offset of the current-task pointer looks wrong (%#lx gives %p, expected %p)\n",
		       off_cur, seen, current);
		return -ELIBBAD;
	}

	inst->boot.gs_current_task = off_cur;
	inst->boot.gs_preempt_count = off_pc;
	inst->boot.task_hooks_off = offsetof(struct task_struct, carrier_hooks);
	inst->boot.hooks_vrec_off = offsetof(struct klet_carrier, vrec) -
				   offsetof(struct klet_carrier, hooks);

	pr_info("kernelet: sandbox %d: host offsets: gs:current %#llx, gs:preempt_count %#llx, task->carrier_hooks +%llu, hooks->vrec +%llu\n",
		inst->slot, inst->boot.gs_current_task, inst->boot.gs_preempt_count,
		inst->boot.task_hooks_off, inst->boot.hooks_vrec_off);
	return 0;
}

/* How big a floating-point save area the enabled features need. */
u64 klet_fpu_area_size(void)
{
	u32 eax, ebx, ecx, edx;

	if (!boot_cpu_has(X86_FEATURE_XSAVE))
		return 512;
	cpuid_count(0xd, 0, &eax, &ebx, &ecx, &edx);
	if (ebx < 512)
		return 512;
	if (ebx > 4096)
		return 4096;
	return ebx;
}

/* ------------------------------------------------------------------ */
/* load_binary                                                        */
/* ------------------------------------------------------------------ */

/*
 * Unmaps everything the exec machinery left in the fresh address space: one
 * one-page VMA at STACK_TOP_MAX holding the argument strings. The kernelet is the
 * loader here, so nothing else belongs in the mm, not even a vDSO.
 */
static void klet_empty_mm(void)
{
	struct mm_struct *mm = current->mm;
	unsigned long start[8], len[8];
	struct vm_area_struct *vma;
	int n = 0, i;

	VMA_ITERATOR(vmi, mm, 0);

	mmap_read_lock(mm);
	for_each_vma(vmi, vma) {
		if (n == ARRAY_SIZE(start))
			break;
		start[n] = vma->vm_start;
		len[n] = vma->vm_end - vma->vm_start;
		n++;
	}
	mmap_read_unlock(mm);

	for (i = 0; i < n; i++)
		vm_munmap(start[i], len[i]);
}

/*
 * set_personality_64bit() is not exported, so the part of it that matters to a
 * carrier is open-coded. The vsyscall bit (MM_CONTEXT_HAS_VSYSCALL) is
 * deliberately left clear. That is no protection, though: emulate_vsyscall(),
 * Linux's page-fault path for a jump into the vsyscall page, does not look at
 * it. What keeps a tenant from having Linux run a system call for it there is
 * that the endovisor does not load on a kernel that emulates the page at all
 * (klet_init).
 */
static void klet_personality_64bit(void)
{
	clear_thread_flag(TIF_ADDR32);
	current_thread_info()->status &= ~TS_COMPAT;
	current->personality &= ~READ_IMPLIES_EXEC;
}

/*
 * Every signal that can be ignored is ignored, and the carrier keeps it so for
 * its whole life; the mask is cleared too, because a blocked signal is
 * queued even when it is ignored. SIGKILL and SIGSTOP cannot be ignored, and
 * keep their meaning.
 *
 * Draining is not enough. The resume hook drains the queue and returns, but in
 * the same pass of the exit loop Linux goes on to deliver signals if the work
 * flags it read before the hook said one was pending, and a signal that
 * arrives between the drain and that delivery would be Linux's to act on: a
 * SIGQUIT or SIGABRT would dump core and end the carrier, and with it the
 * sandbox, a SIGTSTP, SIGTTIN or SIGTTOU would stop it. And a signal whose
 * default action is to terminate without a core dump (SIGTERM, SIGPIPE, ...)
 * never reaches the queue at all: it is fatal at the moment it is sent
 * (complete_signal() turns it into SIGKILL). An ignored signal is discarded
 * when it is sent, so none of them ever becomes pending.
 *
 * The fault signals are ignored as well, and still arrive: a fault of the
 * tenant's is sent with force_sig_info_to_task(), which first sets an ignored
 * or blocked signal back to SIG_DFL, so it is queued, and the hook drains it
 * before Linux can deliver it, because it was queued before the exit loop
 * began. The drain then ignores it again (klet_drain_signals), so that the
 * next one is forced, and drained, in the same way, and the same signal sent
 * by a process in the meantime is discarded.
 *
 * kernel_sigaction() is documented for kernel threads only, and not for a task
 * that shares its sighand (CLONE_SIGHAND), because it skips do_sigaction()'s
 * checks and would change the action of a whole thread group. The carrier is
 * single-threaded: begin_new_exec() has killed every other thread of the
 * process and given it a sighand of its own (unshare_sighand()).
 */
static void klet_ignore_signals(void)
{
	sigset_t none;
	int sig;

	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	for (sig = 1; sig <= _NSIG; sig++)
		if (!sig_kernel_only(sig))
			kernel_sigaction(sig, SIG_IGN);
}

/*
 * The binary-format handler: an exec of one of the endovisor's exec
 * descriptors (sandbox.c) makes the executing process the carrier of that
 * virtual CPU. The first bytes of the file only turn other files away quickly;
 * the file is recognized by its inode (klet_exec_lookup), because the exec
 * opened the file afresh.
 *
 * Before the point of no return, the checks of KLET_GET_VCPU_EXEC_FD, in this
 * order: the sandbox has not been killed (ESRCH), this virtual CPU has not
 * been started (EBUSY), and the process is in the sandbox's control group with
 * no-new-privileges set (EPERM). Then what every loader does, with an address
 * space left empty, because only the kernelet may put anything in it. The
 * first arrival starts the sandbox (klet_sandbox_start); on the way out to
 * user mode the resume hook takes the carrier, waits for the others, and the
 * carrier never reaches the user mode the exec was heading for.
 */
static int klet_load_binary(struct linux_binprm *bprm)
{
	struct klet_inst *inst;
	struct klet_carrier *c;
	char comm[TASK_COMM_LEN];
	unsigned int vcpu;
	int err, dumpable;
	bool first;

	if (memcmp(bprm->buf, KLET_EXEC_MAGIC, KLET_EXEC_MAGIC_LEN) != 0)
		return -ENOEXEC;
	inst = klet_exec_lookup(bprm, &vcpu);
	if (!inst)
		return -ENOEXEC;	/* the bytes, but not one of our files */
	err = klet_exec_claim(inst, vcpu);
	if (err)
		goto out;

	err = begin_new_exec(bprm);
	if (err) {
		klet_exec_unclaim(inst, vcpu);
		goto out;
	}
	/* The point of no return: from here on, a failure kills the process. */

	klet_personality_64bit();
	setup_new_exec(bprm);
	klet_empty_mm();
	set_binfmt(&klet_format);
	finalize_exec(bprm);
	start_thread(current_pt_regs(), 0, 0);
	klet_ignore_signals();

	/*
	 * Not dumpable. begin_new_exec() makes a process that executed a file
	 * it cannot read non-dumpable (would_dump(), BINPRM_FLAGS_ENFORCE_NONDUMP,
	 * then set_dumpable(suid_dumpable)), and the exec file is execute-only;
	 * but a holder with CAP_DAC_OVERRIDE, root, *can* read it, so for root
	 * the rule leaves the carrier dumpable, and for anyone it depends on
	 * fs.suid_dumpable. set_dumpable() is not exported: its one line here.
	 */
	dumpable = get_dumpable(current->mm);
	set_mask_bits(&current->mm->flags, MMF_DUMPABLE_MASK, SUID_DUMP_DISABLE);

	/* __set_task_comm() is not exported (and so no perf comm event is sent). */
	snprintf(comm, sizeof(comm), "kernelet/%d/%u", inst->slot, vcpu);
	task_lock(current);
	strscpy_pad(current->comm, comm, sizeof(current->comm));
	task_unlock(current);

	c = klet_carrier_new(inst, vcpu);
	if (!c) {
		err = READ_ONCE(inst->dying) ? -ESRCH : -ENOMEM;
		goto kill;
	}
	err = klet_carrier_attach(c);
	if (err) {
		klet_carrier_lifeline_closed(c);
		goto kill;
	}
	/* The carrier's own, empty address space, to fall back to. */
	mmget(current->mm);
	c->blank_mm = current->mm;
	c->cur_mm = current->mm;

	spin_lock(&inst->lock);
	inst->vcpus[vcpu] = c;
	inst->vslot[vcpu].pid = current->pid;
	first = !inst->started;
	inst->started = true;
	spin_unlock(&inst->lock);

	klet_sched_attach(c);
	pr_info("kernelet: sandbox %d: vcpu %u carrier (pid %d) arrived as %s: address space emptied, every signal ignored, dumpable %d -> 0\n",
		inst->slot, vcpu, current->pid, comm, dumpable);

	/* The first arrival starts the sandbox: its data, its grant, its offsets. */
	if (first) {
		err = klet_sandbox_start(inst);
		if (err) {
			pr_err("kernelet: sandbox %d cannot start: %d\n", inst->slot, err);
			goto kill;	/* its death ends the sandbox */
		}
	}
	spin_lock(&inst->lock);
	inst->arrived++;
	inst->vslot[vcpu].state = KLET_VCPU_WAITING;
	spin_unlock(&inst->lock);
	wake_up_all(&inst->arrive_wq);

	/*
	 * On the way out to user mode the resume hook runs, which waits until
	 * this virtual CPU may enter the image and then enters it.
	 */
	set_thread_flag(TIF_NOTIFY_RESUME);
	klet_inst_put(inst);
	return 0;

kill:
	force_sig(SIGKILL);
out:
	klet_inst_put(inst);
	return err;
}

static struct linux_binfmt klet_format = {
	.module = THIS_MODULE,
	.load_binary = klet_load_binary,
};

/* ------------------------------------------------------------------ */
/* The system descriptor: /dev/endovisor                              */
/* ------------------------------------------------------------------ */

/* An image file is at most this big. */
#define KLET_IMAGE_MAX (64 << 20)

/*
 * What loading a module asks (init_module, finit_module): CAP_SYS_MODULE in the
 * initial user namespace, module loading not disabled, and, because an image
 * carries no signature, neither signature enforcement nor lockdown in force.
 * modules_disabled is not exported; the patch exports it.
 */
static int klet_may_load_code(void)
{
	if (!capable(CAP_SYS_MODULE) || READ_ONCE(modules_disabled))
		return -EPERM;
	if (is_module_sig_enforced())
		return -EPERM;
	return security_locked_down(LOCKDOWN_MODULE_SIGNATURE);
}

static long klet_kind_register(struct klet_kind_register __user *uarg)
{
	struct klet_kind_register reg;
	struct klet_kind *kind = NULL;
	void *buf = NULL;
	ssize_t len;
	int i, err, id = -1;

	if (copy_from_user(&reg, uarg, sizeof(reg)))
		return -EFAULT;
	if (reg.flags || !reg.name[0] || strnlen(reg.name, KLET_NAME_LEN) == KLET_NAME_LEN)
		return -EINVAL;
	err = klet_may_load_code();
	if (err)
		return err;
	/* Read as a module file is read, so that the host's security modules see it. */
	len = kernel_read_file_from_fd(reg.image_fd, 0, &buf, KLET_IMAGE_MAX, NULL,
				       READING_MODULE);
	if (len < 0)
		return len;

	mutex_lock(&klet_table_lock);
	err = 0;
	for (i = 0; i < KLET_MAX_KINDS; i++) {
		if (klet_kinds[i].used && strcmp(klet_kinds[i].name, reg.name) == 0)
			err = -EEXIST;
		else if (!klet_kinds[i].used && !kind) {
			kind = &klet_kinds[i];
			id = i;
		}
	}
	if (!err && !kind)
		err = -ENOSPC;
	if (!err)
		err = klet_image_load(kind, buf, len);
	if (!err) {
		strscpy(kind->name, reg.name, sizeof(kind->name));
		kind->nr_sandboxes = 0;
		kind->used = true;
		pr_info("kernelet: kind %d \"%s\" registered: an image of %zd bytes, PIE text offsets [%#lx, %#lx), %zu bytes shared\n",
			id, kind->name, len, kind->text_lo, kind->text_hi, kind->text_size);
	}
	mutex_unlock(&klet_table_lock);
	vfree(buf);
	return err ? err : id;
}

static long klet_kind_list(struct klet_kind_list __user *uarg)
{
	struct klet_kind_list list;
	struct klet_kind_info __user *out;
	u32 n = 0;
	int i, err = 0;

	if (copy_from_user(&list, uarg, sizeof(list)))
		return -EFAULT;
	out = u64_to_user_ptr(list.kinds);
	mutex_lock(&klet_table_lock);
	for (i = 0; i < KLET_MAX_KINDS; i++) {
		struct klet_kind *kind = &klet_kinds[i];
		struct klet_kind_info info = { .id = i };

		if (!kind->used)
			continue;
		if (n < list.capacity) {
			info.nr_sandboxes = kind->nr_sandboxes;
			info.text_bytes = kind->text_size;
			strscpy_pad(info.name, kind->name, sizeof(info.name));
			if (copy_to_user(&out[n], &info, sizeof(info))) {
				err = -EFAULT;
				break;
			}
		}
		n++;
	}
	mutex_unlock(&klet_table_lock);
	if (err)
		return err;
	list.count = n;
	if (copy_to_user(uarg, &list, sizeof(list)))
		return -EFAULT;
	return 0;
}

static long klet_kind_unregister(u32 __user *uarg)
{
	struct klet_kind *kind;
	u32 id;
	int err;

	if (get_user(id, uarg))
		return -EFAULT;
	err = klet_may_load_code();
	if (err)
		return err;
	mutex_lock(&klet_table_lock);
	kind = id < KLET_MAX_KINDS ? &klet_kinds[id] : NULL;
	if (!kind || !kind->used)
		err = -ENOENT;
	else if (kind->nr_sandboxes)
		err = -EBUSY;
	else {
		pr_info("kernelet: kind %u \"%s\" unregistered\n", id, kind->name);
		klet_image_free(kind);
		kind->used = false;
	}
	mutex_unlock(&klet_table_lock);
	return err;
}

static long klet_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case KLET_VSOCK_SOCKET:
		return klet_vsock_socket();
	case KLET_VSOCK_PROFILE_RESET:
		return klet_vsock_profile_report(true);
	case KLET_VSOCK_PROFILE_REPORT:
		return klet_vsock_profile_report(false);
	case KLET_GET_API_VERSION:
		return KLET_API_VERSION;
	case KLET_KIND_REGISTER:
		return klet_kind_register(uarg);
	case KLET_KIND_LIST:
		return klet_kind_list(uarg);
	case KLET_KIND_UNREGISTER:
		return klet_kind_unregister(uarg);
	case KLET_CREATE:
		return klet_sandbox_create(uarg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations klet_dev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = klet_dev_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice klet_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "endovisor",
	.fops = &klet_dev_fops,
	.mode = 0600,
};

/* ------------------------------------------------------------------ */
/* Module init and exit                                               */
/* ------------------------------------------------------------------ */

static int __init klet_init(void)
{
	int err;

	/*
	 * With vsyscall emulation, a tenant that jumps to the vsyscall page
	 * faults, and Linux's fault handler runs gettimeofday, time or getcpu
	 * for it (emulate_vsyscall()) and returns to user mode: a system call
	 * the syscall hook never sees, which no other path in the endovisor can
	 * catch. Whether `vsyscall=none` was given at boot is held in a static
	 * of Linux's that no module can read, so a kernel built with emulation
	 * is refused outright.
	 */
#ifdef CONFIG_X86_VSYSCALL_EMULATION
	pr_err("kernelet: refusing to load: this kernel emulates the vsyscall page (CONFIG_X86_VSYSCALL_EMULATION=y), through which a tenant could have Linux run a system call without passing the syscall hook; build Linux with it off\n");
	return -EINVAL;
#endif

	/* The console and log drains, and the destroys release leaves behind. */
	klet_wq = alloc_workqueue("endovisor", WQ_UNBOUND, 0);
	if (!klet_wq)
		return -ENOMEM;

	klet_evict_init();
	klet_watch_init();
	klet_sched_init();
	register_binfmt(&klet_format);
	err = misc_register(&klet_misc);
	if (err) {
		unregister_binfmt(&klet_format);
		klet_sched_exit();
		klet_watch_exit();
		klet_evict_exit();
		destroy_workqueue(klet_wq);
		return err;
	}

	pr_info("kernelet: endovisor loaded: /dev/endovisor, API %d, image ABI %llu, watch %s, mirror %s, fpu area %llu B, tsc %u kHz%s\n",
		KLET_API_VERSION, KLET_ABI_VERSION, klet_watch ? "on" : "off",
		klet_mirror ? "on" : "off", klet_fpu_area_size(), tsc_khz,
		klet_test_hooks ? ", TEST HOOK on" : "");
	return 0;
}

static void __exit klet_exit(void)
{
	struct klet_totals *t = &klet_totals;
	int i;

	klet_vsock_report();
	misc_deregister(&klet_misc);
	unregister_binfmt(&klet_format);
	/* Waits for a destroy that a release left to the exit path, and the drains. */
	destroy_workqueue(klet_wq);
	klet_watch_exit();
	klet_evict_exit();
	klet_sched_exit();

	for (i = 0; i < KLET_MAX_SANDBOXES; i++)
		if (klet_sandboxes[i])
			pr_warn("kernelet: at unload: sandbox %d was never destroyed\n", i);
	for (i = 0; i < KLET_MAX_KINDS; i++)
		if (klet_kinds[i].used) {
			klet_image_free(&klet_kinds[i]);
			klet_kinds[i].used = false;
		}

	if (atomic_read(&t->sweeps))
		pr_info("kernelet: eviction: %d sweep(s), %d eviction(s)\n",
			atomic_read(&t->sweeps), atomic_read(&t->n.evictions));
	{
		int n = min(atomic_read(&t->forced_recorded), KLET_FORCED_RECORDS);

		for (i = 0; i < n; i++) {
			struct klet_forced_rec *r = &t->forced_rec[i];

			pr_info("kernelet: forced yield of vcpu %d (pid %d) on cpu %d at ip %#lx, guards %u, irq_off %u\n",
				r->vcpu, r->pid, r->cpu, r->ip, r->guards, r->irq_off);
		}
	}
	pr_info("kernelet: watch timer: %d tick(s), %d upcall(s), %d voluntary yield(s)\n",
		atomic_read(&t->n.ticks), atomic_read(&t->n.virqs),
		atomic_read(&t->n.yields));
	pr_info("kernelet: kicks: %d IPI(s), %d requested firing(s): %d delivered an upcall, %d found user mode or a service, %d a critical section\n",
		atomic_read(&t->n.kick_ipis), atomic_read(&t->n.watch_fires),
		atomic_read(&t->n.fires_redirected),
		atomic_read(&t->n.fires_elsewhere),
		atomic_read(&t->n.fires_masked));
	pr_info("kernelet: STATS sandboxes=%d deferrals=%d longest_defer_us=%d longest_tick_gap_us=%d forced_yields=%d bad_preempt=%d on_spin=%d yield_to=%d mm_switch=%d fpu_swap=%d fpu_reject=%d mirror_fault=%d stolen_ms=%d kstacks=%d\n",
		atomic_read(&t->sandboxes),
		atomic_read(&t->n.deferrals),
		atomic_read(&t->longest_defer_us),
		atomic_read(&t->longest_tick_gap_us),
		atomic_read(&t->n.forced_yields),
		atomic_read(&t->n.bad_preempt),
		atomic_read(&t->n.on_spins),
		atomic_read(&t->n.yield_tos),
		atomic_read(&t->n.mm_switches),
		atomic_read(&t->n.fpu_swaps),
		atomic_read(&t->n.fpu_rejects),
		atomic_read(&t->n.mirror_faults),
		atomic_read(&t->n.stolen_ms),
		atomic_read(&t->kstack_total));
	{
		struct klet_mem_stats *m = &t->mem;

		pr_info("kernelet: MEMSTATS faults=%lld mkwrites=%lld reported=%lld kernel_faults=%lld maps=%lld unmaps=%lld unmapped=%lld protects=%lld protected=%lld flushes=%lld flush_alls=%lld waits=%lld wait_sleeps=%lld wakes=%lld spaces=%lld\n",
			atomic64_read(&m->faults), atomic64_read(&m->mkwrites),
			atomic64_read(&m->reported), atomic64_read(&m->kernel_faults),
			atomic64_read(&m->maps),
			atomic64_read(&m->unmaps), atomic64_read(&m->unmapped),
			atomic64_read(&m->protects), atomic64_read(&m->protected_),
			atomic64_read(&m->flushes), atomic64_read(&m->flush_alls),
			atomic64_read(&m->waits), atomic64_read(&m->wait_sleeps),
			atomic64_read(&m->wakes), atomic64_read(&m->spaces));
	}
	pr_info("kernelet: kills: %d found in a service, %d in kernelet code by the watch timer\n",
		atomic_read(&t->n.kills_in_service),
		atomic_read(&t->n.kills_in_kernelet));
	{
		s64 allocs = atomic64_read(&t->heap_allocs);
		s64 frees = atomic64_read(&t->heap_frees);
		s64 reaped = atomic64_read(&t->heap_reaped);
		s64 bad = atomic64_read(&t->heap_bad_frees);

		pr_info("kernelet: heap: %lld blocks allocated, %lld freed by the kernelet, %lld freed at teardown, %lld live\n",
			allocs, frees, reaped, allocs - frees - reaped);
		if (bad)
			pr_warn("kernelet: heap: %lld mem_free call(s) refused: not a block mem_alloc handed out\n",
				bad);
		pr_info("kernelet: kernelet stacks: %d made, %d returned, %d live\n",
			atomic_read(&t->kstack_total), atomic_read(&t->kstack_returned),
			atomic_read(&t->kstack_total) - atomic_read(&t->kstack_returned));
	}
	pr_info("kernelet: carrier records: %d made, %d freed, %d live\n",
		atomic_read(&t->carriers_made), atomic_read(&t->carriers_freed),
		atomic_read(&t->carriers_made) - atomic_read(&t->carriers_freed));
	pr_info("kernelet: endovisor unloaded\n");
}

module_init(klet_init);
module_exit(klet_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Endovisor for the kernelet-in-Linux prototype");
