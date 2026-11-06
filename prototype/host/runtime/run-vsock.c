// SPDX-License-Identifier: GPL-2.0
// Exercises the experimental sockets and compares tenant echo with Linux pipes.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sandbox.h"
#include "klet-vsock.h"
#include "vsock-bench.h"

#define WARMUP 32
#define ROUNDS 1024

struct instance {
	int fd;
	pid_t pid;
	unsigned long long cid;
	char console[80];
};

static int system_fd, kind;
static unsigned int sequence;
static int comparison, profiling;
static unsigned int bench_rounds = ROUNDS, bench_warmup = WARMUP;

static int comparison_control(int transport, unsigned int size, unsigned int repeat)
{
	struct sockaddr_vm address = { .svm_family = AF_VSOCK, .svm_cid = VMADDR_CID_HOST,
		.svm_port = VB_PORT };
	struct vb_header header = {
		.magic = VB_MAGIC, .transport = transport, .size = size,
		.rounds = bench_rounds, .warmup = bench_warmup, .repeat = repeat,
	};
	uint32_t token;
	int fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0 || vb_buffers(fd) || connect(fd, (void *)&address, sizeof(address)) ||
	    vb_full(fd, &header, sizeof(header), 1) || vb_full(fd, &token, sizeof(token), 0) || token != VB_MAGIC) {
		perror("comparison control");
		exit(1);
	}
	return fd;
}

static void check(int condition, const char *what)
{
	if (!condition) {
		sandbox_say("FAIL %s: errno=%d (%s)", what, errno, strerror(errno));
		exit(1);
	}
}

static uint64_t ns(clockid_t clock)
{
	struct timespec ts;

	check(clock_gettime(clock, &ts) == 0, "clock_gettime");
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void pin(unsigned int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	check(sched_setaffinity(0, sizeof(set), &set) == 0, "pin process");
}

// Requests delivery of real virtio interrupts on the common caller's guest CPU.
static void configure_virtio(void)
{
	FILE *interrupts = fopen("/proc/interrupts", "r");
	char line[512], path[128], affinity[80], effective[80];
	unsigned int irq, count = 0, cid = 0;
	int fd = open("/dev/vsock", O_RDONLY | O_CLOEXEC);

	check(fd >= 0 && ioctl(fd, IOCTL_VM_SOCKETS_GET_LOCAL_CID, &cid) == 0 && cid > 2,
	      "real guest vsock identity");
	close(fd);
	check(interrupts != NULL, "open guest IRQ placement");
	while (fgets(line, sizeof(line), interrupts)) {
		FILE *verify;

		if (!strstr(line, "virtio") || sscanf(line, " %u:", &irq) != 1)
			continue;
		snprintf(path, sizeof(path), "/proc/irq/%u/smp_affinity_list", irq);
		fd = open(path, O_WRONLY | O_CLOEXEC);
		check(fd >= 0 && write(fd, "1\n", 2) == 2, "route virtio IRQ to guest caller CPU");
		close(fd);
		verify = fopen(path, "r");
		check(verify && fgets(affinity, sizeof(affinity), verify) && !strcmp(affinity, "1\n"),
		      "verify virtio IRQ placement");
		fclose(verify);
		snprintf(path, sizeof(path), "/proc/irq/%u/effective_affinity_list", irq);
		verify = fopen(path, "r");
		check(verify && fgets(effective, sizeof(effective), verify), "effective virtio IRQ affinity");
		fclose(verify);
		effective[strcspn(effective, "\n")] = 0;
		line[strcspn(line, "\n")] = 0;
		check(strstr(line, "PCI-MSI") != NULL, "MSI/MSI-X virtio device interrupts");
		sandbox_say("VIRTIO-IRQ irq=%u requested=1 effective=%s line=%s", irq, effective, line);
		count++;
	}
	fclose(interrupts);
	check(count > 0, "found virtio device interrupts");
	sandbox_say("VIRTIO-CONFIG local_cid=%u peer_cid=2 buffer_bytes=%u irq_cpu=1", cid, VB_CAPACITY);
}

static void report_virtio_delivery(void)
{
	FILE *interrupts = fopen("/proc/interrupts", "r");
	char line[512], path[128], effective[80];
	unsigned int irq;

	check(interrupts != NULL, "final virtio interrupt counters");
	while (fgets(line, sizeof(line), interrupts)) {
		FILE *file;

		if (!strstr(line, "virtio") || sscanf(line, " %u:", &irq) != 1)
			continue;
		snprintf(path, sizeof(path), "/proc/irq/%u/effective_affinity_list", irq);
		file = fopen(path, "r");
		check(file && fgets(effective, sizeof(effective), file), "final effective IRQ affinity");
		fclose(file);
		effective[strcspn(effective, "\n")] = 0;
		line[strcspn(line, "\n")] = 0;
		sandbox_say("VIRTIO-IRQ-AFTER irq=%u effective=%s line=%s", irq, effective, line);
	}
	fclose(interrupts);
}

// Reads Linux scheduler CPU time for either a carrier or the pipe child.
static uint64_t cpu_ns(pid_t pid)
{
	char path[80];
	unsigned long long runtime = 0;
	FILE *file;

	snprintf(path, sizeof(path), "/proc/%d/schedstat", pid);
	file = fopen(path, "r");
	check(file && fscanf(file, "%llu", &runtime) == 1, "read peer CPU time");
	fclose(file);
	return runtime;
}

static int contains(struct instance *instance, const char *marker)
{
	char data[8192];
	int fd = open(instance->console, O_RDONLY | O_CLOEXEC);
	ssize_t count;

	if (fd < 0)
		return 0;
	count = read(fd, data, sizeof(data) - 1);
	close(fd);
	if (count < 0)
		return 0;
	data[count] = 0;
	check(!strstr(data, "panicked") && !strstr(data, "exit=1"), "tenant did not fail");
	return strstr(data, marker) != NULL;
}

static void ready(struct instance *instance)
{
	long long until = now_ms() + 5000;

	while (now_ms() < until) {
		if (contains(instance, "VSOCK-TENANT listening"))
			return;
		msleep(5);
	}
	check(0, "tenant listener became ready");
}

static struct instance create_with_permission(const char *command, int permit_host)
{
	struct instance instance;
	struct klet_create create = {
		.kind = kind, .nr_vcpus = 1,
		.memory_initial = 4 << 20, .memory_max = 4 << 20, .max_tasks = 16,
		.cmdline = (uintptr_t)command, .cmdline_len = strlen(command),
	};
	int output;
	__s32 host = -1;

	memset(&instance, 0, sizeof(instance));
	instance.fd = ioctl(system_fd, KLET_CREATE, &create);
	check(instance.fd >= 0, "create sandbox");
	check(ioctl(instance.fd, KLET_VSOCK_GET_CID, &instance.cid) == 0, "get CID");
	if (permit_host)
		check(ioctl(instance.fd, KLET_VSOCK_PERMIT, &host) == 0, "permit host capability");
	snprintf(instance.console, sizeof(instance.console), "/vsock-%u.console", ++sequence);
	output = open(instance.console, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	check(output >= 0 && ioctl(instance.fd, KLET_SET_CONSOLE, &output) == 0, "set console");
	close(output);
	return instance;
}

static struct instance create(const char *command)
{
	return create_with_permission(command, 1);
}

static void boot(struct instance *instance)
{
	unsigned int zero = 0;
	int exec = ioctl(instance->fd, KLET_GET_VCPU_EXEC_FD, &zero);
	struct sandbox_opts options;

	check(exec >= 0, "get exec descriptor");
	sandbox_opts_init(&options);
	options.pin = 1;
	instance->pid = fork();
	check(instance->pid >= 0, "fork carrier");
	if (!instance->pid)
		start_carrier(exec, 0, &options);
	close(exec);
}

static void finish(struct instance *instance, int killed)
{
	struct pollfd p = { .fd = instance->fd, .events = POLLIN };
	struct klet_exit result;
	int status;

	check(poll(&p, 1, 5000) == 1, "sandbox exits before timeout");
	check(read(instance->fd, &result, sizeof(result)) == sizeof(result), "read exit record");
	check(waitpid(instance->pid, &status, 0) == instance->pid, "reap carrier");
	check(killed ? result.how == KLET_END_KILLED :
	      result.how == KLET_END_EXITED && result.code == 0, "expected sandbox exit");
	if (!killed)
		check(contains(instance, "VSOCK-TENANT exit=0"), "tenant completed checks");
	if (profiling) {
		char bytes[8192], *line, *save;
		int fd = open(instance->console, O_RDONLY);
		ssize_t count;

		check(fd >= 0, "open profiling console");
		count = read(fd, bytes, sizeof(bytes) - 1);
		check(count >= 0, "read profiling console");
		close(fd);
		bytes[count] = 0;
		for (line = strtok_r(bytes, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
			if (strstr(line, "VSOCK-COPY"))
				sandbox_say("%s", line);
	}
	close(instance->fd);
}

static void terminate(struct instance *instance)
{
	__u32 reason = 17;

	check(ioctl(instance->fd, KLET_KILL, &reason) == 0, "kill sandbox");
}

static int connect_instance(struct instance *instance, unsigned int port)
{
	int fd = kv_socket(system_fd);

	check(fd >= 0 && kv_connect(fd, instance->cid, port) == 0, "connect to tenant listener");
	return fd;
}

static void transfer(int fd, unsigned char *buffer, size_t length, int write_it)
{
	size_t done = 0;

	while (done < length) {
		ssize_t count = write_it ? write(fd, buffer + done, length - done) :
			read(fd, buffer + done, length - done);

		check(count > 0, "complete payload transfer");
		done += count;
	}
}

static void functional(void)
{
	struct instance server = create("role=0 size=4093 loops=40 port=1234");
	unsigned char sent[4093], received[4093];
	int fd, i, j;

	boot(&server);
	ready(&server);
	fd = connect_instance(&server, 1234);
	for (i = 0; i < 40; i++) {
		for (j = 0; j < 4093; j++)
			sent[j] = (i * 31 + j * 17) % 251;
		transfer(fd, sent, sizeof(sent), 1);
		transfer(fd, received, sizeof(received), 0);
		check(!memcmp(sent, received, sizeof(sent)), "ordered bytes across ring wraparound");
	}
	check(read(fd, received, sizeof(received)) == 0, "drain before write-half EOF");
	close(fd);
	finish(&server, 0);
	sandbox_say("ok socket lifecycle, partial transfers, wraparound, ordered payload, half-close EOF");
}

static void peers(void)
{
	struct instance server = create("role=0 size=4096 loops=32 port=1234"), client;
	char command[128];

	boot(&server);
	ready(&server);
	snprintf(command, sizeof(command), "role=1 peer=%llu size=4096 loops=32 port=1234", server.cid);
	client = create(command);
	check(ioctl(client.fd, KLET_VSOCK_PERMIT, &server.fd) == 0, "permit pair with both descriptors");
	boot(&client);
	finish(&client, 0);
	finish(&server, 0);
	sandbox_say("ok independent kernelet peers verified echoed payloads through shared rings");
}

static void authorization(void)
{
	struct instance server = create_with_permission("role=3 port=1234", 0);
	int fd = kv_socket(system_fd);
	__s32 host = -1;

	boot(&server);
	ready(&server);
	check(fd >= 0 && kv_connect(fd, server.cid, 1234) == -1 && errno == EACCES,
	      "host reachability denied by default");
	check(ioctl(server.fd, KLET_VSOCK_PERMIT, &host) == 0 &&
	      kv_connect(fd, server.cid, 1234) == 0, "explicit capability permits host");
	close(fd);
	terminate(&server);
	finish(&server, 1);
	fd = kv_socket(system_fd);
	check(fd >= 0 && kv_connect(fd, server.cid, 1234) == -1 && errno == ECONNREFUSED,
	      "dead CID cannot reach a replacement");
	close(fd);
	sandbox_say("ok default denial, explicit permission, and dead CID refusal");
}

static void partial_faults(void)
{
	struct instance server = create("role=5 port=1234");
	unsigned char bytes[4096];
	int fd;
	unsigned int i;

	boot(&server);
	ready(&server);
	fd = connect_instance(&server, 1234);
	transfer(fd, bytes, 2048, 0);
	for (i = 0; i < 2048; i++)
		check(bytes[i] == ((KLET_VSOCK_CAPACITY - 2048 + i) * 17 + 3) % 251,
		      "partial write fault preserves mapped prefix");
	memset(bytes, 0xb4, sizeof(bytes));
	transfer(fd, bytes, sizeof(bytes), 1);
	memset(bytes, 0, sizeof(bytes));
	transfer(fd, bytes, 2048, 0);
	for (i = 0; i < 2048; i++)
		check(bytes[i] == 0xb4, "cancelled read lease preserved bytes");
	close(fd);
	finish(&server, 0);
	sandbox_say("ok partial user faults publish or consume only copied prefixes and cancel empty leases");
}

static void survivor(void)
{
	struct instance victim = create("role=2 port=1234"), client;
	char command[128];
	unsigned char bytes[4096];
	int listener = kv_socket(system_fd), stream;

	check(listener >= 0 && kv_bind(listener, 4321) == 0 && kv_listen(listener, 2) == 0,
	      "prepare recovery listener");
	boot(&victim);
	ready(&victim);
	snprintf(command, sizeof(command), "role=4 peer=%llu size=4096 loops=1 port=1234", victim.cid);
	client = create(command);
	check(ioctl(client.fd, KLET_VSOCK_PERMIT, &victim.fd) == 0, "authorize survivor and victim");
	boot(&client);
	{
		long long deadline = now_ms() + 5000;

		while (!contains(&client, "VSOCK-TENANT recovery connected") && now_ms() < deadline)
			msleep(5);
		check(contains(&client, "VSOCK-TENANT recovery connected"), "survivor connected to victim");
	}
	terminate(&victim);
	finish(&victim, 1);
	stream = kv_accept(listener);
	check(stream >= 0, "surviving kernelet connects after peer destruction");
	transfer(stream, bytes, sizeof(bytes), 0);
	transfer(stream, bytes, sizeof(bytes), 1);
	close(stream);
	close(listener);
	finish(&client, 0);
	sandbox_say("ok surviving kernelet observes reset and exchanges verified data after peer destruction");
}

static void host_listener(void)
{
	int listener = kv_socket(system_fd), stream;
	struct instance client = create("role=1 peer=0 size=4096 loops=8 port=4321");
	unsigned char bytes[4096];
	int i;

	check(listener >= 0 && kv_bind(listener, 4321) == 0 && kv_listen(listener, 2) == 0,
	      "host bind and listen");
	boot(&client);
	stream = kv_accept(listener);
	check(stream >= 0 && stream != listener, "accept a distinct host stream descriptor");
	for (i = 0; i < 8; i++) {
		transfer(stream, bytes, sizeof(bytes), 0);
		transfer(stream, bytes, sizeof(bytes), 1);
	}
	close(stream);
	close(listener);
	finish(&client, 0);
	sandbox_say("ok host listener accepts a tenant-initiated connection");
}

struct blocked {
	int fd, operation, result, error;
	int fill, verify;
	unsigned int length, offset;
	unsigned long long cid;
	volatile int entered;
};

static void *block_operation(void *arg)
{
	struct blocked *b = arg;
	unsigned char bytes[4096] = {0};
	size_t length = b->length ? b->length : sizeof(bytes);

	memset(bytes, b->fill, sizeof(bytes));
	__atomic_store_n(&b->entered, 1, __ATOMIC_RELEASE);
	if (b->operation == 0)
		b->result = read(b->fd, bytes, length);
	else if (b->operation == 1)
		b->result = write(b->fd, bytes, length);
	else if (b->operation == 3)
		b->result = syscall(SYS_write, b->fd, (void *)1, length);
	else if (b->operation == 4)
		b->result = syscall(SYS_read, b->fd, (void *)1, length);
	else
		b->result = kv_connect(b->fd, b->cid, 1234);
	b->error = errno;
	if (b->verify && b->result > 0)
		for (int i = 0; i < b->result; i++)
			if (bytes[i] != ((b->offset + i) * 17 + 3) % 251) {
				b->result = -1;
				b->error = EILSEQ;
				break;
			}
	return NULL;
}

static int join_thread(pthread_t thread)
{
	struct timespec deadline;

	check(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "join deadline");
	deadline.tv_sec += 5;
	return pthread_timedjoin_np(thread, NULL, &deadline);
}

static void wait_demand(int fd, unsigned int ring, int producer)
{
	long long deadline = now_ms() + 5000;
	struct klet_vsock_waiters state;

	while (now_ms() < deadline) {
		check(ioctl(fd, KLET_VSOCK_TEST_WAITERS, &state) == 0, "inspect registered demand");
		if (producer ? state.producer[ring] : state.consumer[ring])
			return;
		msleep(1);
	}
	check(0, "operation registered before releasing its condition");
}

static void no_demand(int fd)
{
	struct klet_vsock_waiters state;

	check(ioctl(fd, KLET_VSOCK_TEST_WAITERS, &state) == 0 &&
	      !state.consumer[0] && !state.consumer[1] && !state.producer[0] && !state.producer[1],
	      "registered demand balances after interruption or kill");
}

static void killed_waiter(int operation)
{
	struct instance server = create(operation == 2 ? "role=3 port=1234" : "role=2 port=1234");
	struct blocked b = { .operation = operation };
	pthread_t thread;
	unsigned char bytes[4096] = {0};
	int extra[8], flags, i;

	boot(&server);
	ready(&server);
	if (operation == 2) {
		for (i = 0; i < 8; i++)
			extra[i] = connect_instance(&server, 1234);
		b.fd = kv_socket(system_fd);
		b.cid = server.cid;
	} else {
		b.fd = connect_instance(&server, 1234);
	}
	check(b.fd >= 0, "create blocked endpoint");
	if (operation == 1) {
		flags = fcntl(b.fd, F_GETFL);
		check(fcntl(b.fd, F_SETFL, flags | O_NONBLOCK) == 0, "nonblocking fill");
		for (i = 0; i < (int)(KLET_VSOCK_CAPACITY / sizeof(bytes)); i++)
			check(write(b.fd, bytes, sizeof(bytes)) == sizeof(bytes), "fill bounded ring");
		check(write(b.fd, bytes, sizeof(bytes)) == -1 && errno == EAGAIN, "full ring backpressure");
		check(fcntl(b.fd, F_SETFL, flags) == 0, "restore blocking write");
	}
	check(pthread_create(&thread, NULL, block_operation, &b) == 0, "start blocked operation");
	while (!__atomic_load_n(&b.entered, __ATOMIC_ACQUIRE))
		msleep(1);
	msleep(30);
	check(pthread_tryjoin_np(thread, NULL) == EBUSY, "operation remains blocked before kill");
	terminate(&server);
	check(join_thread(thread) == 0, "peer kill wakes blocked operation");
	check(b.result == -1 && (b.error == ECONNRESET || b.error == EPIPE ||
	      (operation == 2 && b.error == ECONNREFUSED)), "blocked operation returns peer failure");
	close(b.fd);
	if (operation == 2)
		for (i = 0; i < 8; i++)
			close(extra[i]);
	finish(&server, 1);
	sandbox_say("ok kill wakes blocked %s and releases peer-owned resources",
		operation == 0 ? "reader" : operation == 1 ? "writer" : "connect with full backlog");
}

// A delayed admitted write must resolve before EOF, and its ring must survive death.
static void reservation_race(int kill_peer)
{
	struct instance server = create(kill_peer ? "role=2 port=1234" :
				       "role=6 size=4096 loops=1 port=1234");
	struct blocked b = { .operation = 1 };
	struct pollfd state;
	pthread_t thread;
	unsigned char bytes[4096];
	__u32 delay = 300;
	long long deadline;

	boot(&server);
	ready(&server);
	b.fd = connect_instance(&server, 1234);
	check(ioctl(b.fd, KLET_VSOCK_TEST_DELAY, &delay) == 0, "delay an admitted host write");
	check(pthread_create(&thread, NULL, block_operation, &b) == 0, "start delayed producer");
	state = (struct pollfd){ .fd = b.fd, .events = POLLOUT };
	deadline = now_ms() + 5000;
	while (poll(&state, 1, 0) == 1 && now_ms() < deadline)
		msleep(1);
	check(now_ms() < deadline && pthread_tryjoin_np(thread, NULL) == EBUSY,
	      "producer holds an unpublished reservation");
	if (kill_peer) {
		terminate(&server);
		finish(&server, 1);
	} else {
		check(kv_shutdown(b.fd, 1) == 0, "write shutdown races with admitted producer");
		state.events = POLLIN;
		check(poll(&state, 1, 20) == 0, "no premature echo or EOF during producer delay");
	}
	check(join_thread(thread) == 0 && b.result == sizeof(bytes),
	      "admitted write completes with retained storage");
	if (kill_peer) {
		check(read(b.fd, bytes, sizeof(bytes)) == -1 && errno == ECONNRESET,
		      "surviving endpoint reports destroyed peer");
	} else {
		transfer(b.fd, bytes, sizeof(bytes), 0);
		for (unsigned int i = 0; i < sizeof(bytes); i++)
			check(bytes[i] == 0, "all admitted bytes arrive before EOF");
		check(read(b.fd, bytes, sizeof(bytes)) == 0, "EOF follows admitted payload");
		finish(&server, 0);
	}
	close(b.fd);
	sandbox_say("ok reservation survives %s", kill_peer ? "peer destruction" : "concurrent shutdown until publication");
}

static void consume_wakes_writer(void)
{
	struct instance server = create("role=8 size=4096 loops=17 port=1234");
	struct blocked b = { .operation = 1 };
	pthread_t thread;
	unsigned char bytes[4096] = {0};
	int control, i;
	long long deadline;

	boot(&server);
	ready(&server);
	b.fd = connect_instance(&server, 1234);
	for (i = 0; i < 16; i++)
		transfer(b.fd, bytes, sizeof(bytes), 1);
	check(pthread_create(&thread, NULL, block_operation, &b) == 0, "start full-ring writer");
	while (!__atomic_load_n(&b.entered, __ATOMIC_ACQUIRE))
		msleep(1);
	msleep(20);
	check(pthread_tryjoin_np(thread, NULL) == EBUSY, "writer waits before controlled consumption");
	control = kv_socket(system_fd);
	check(control >= 0, "open consumption gate endpoint");
	deadline = now_ms() + 5000;
	while (kv_connect(control, server.cid, 4322) < 0 && errno == ECONNREFUSED && now_ms() < deadline)
		msleep(1);
	check(now_ms() < deadline, "connect consumption gate");
	check(write(control, bytes, 1) == 1, "enable peer consumption");
	close(control);
	for (i = 0; i < 17; i++)
		transfer(b.fd, bytes, sizeof(bytes), 0);
	check(join_thread(thread) == 0 && b.result == sizeof(bytes),
	      "ordinary consumption wakes blocked writer");
	close(b.fd);
	finish(&server, 0);
	sandbox_say("ok controlled consumption wakes full-ring writer without peer death");
}

static int compare_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

static int open_gate(struct instance *server)
{
	int fd = kv_socket(system_fd), ret;
	long long deadline = now_ms() + 5000;

	check(fd >= 0, "create control gate socket");
	do {
		ret = kv_connect(fd, server->cid, 4322);
		if (!ret)
			return fd;
		check(errno == ECONNREFUSED, "gate connection error");
		msleep(1);
	} while (now_ms() < deadline);
	check(0, "control gate listens before timeout");
	return -1;
}

// Registered same-direction demand must survive both ordinary and zero commits.
static void lease_contention(int tx, int cancel)
{
	char command[128];
	struct instance server;
	struct blocked first = { .fill = 0x11 }, second = { .fill = 0x22 };
	pthread_t a, b;
	struct pollfd state;
	unsigned char bytes[4096];
	__u32 delay = 300;
	long long deadline;

	snprintf(command, sizeof(command), "role=%u size=4096 loops=%u port=1234",
		 tx ? 0 : 9, cancel ? 1 : 2);
	server = create(command);
	boot(&server);
	ready(&server);
	first.fd = second.fd = connect_instance(&server, 1234);
	first.operation = tx ? (cancel ? 3 : 1) : (cancel ? 4 : 0);
	second.operation = tx ? 1 : 0;
	if (!tx) {
		first.length = second.length = cancel ? 4096 : 2048;
		first.verify = second.verify = 1;
		second.offset = cancel ? 0 : 2048;
		state = (struct pollfd){ .fd = first.fd, .events = POLLIN };
		check(poll(&state, 1, 5000) == 1, "producer populated RX ring");
	}
	check(ioctl(first.fd, tx ? KLET_VSOCK_TEST_DELAY : KLET_VSOCK_TEST_RX_DELAY, &delay) == 0,
	      "delay first same-direction reservation");
	check(pthread_create(&a, NULL, block_operation, &first) == 0, "start first lease owner");
	state = (struct pollfd){ .fd = first.fd, .events = tx ? POLLOUT : POLLIN };
	deadline = now_ms() + 5000;
	while (poll(&state, 1, 0) == 1 && now_ms() < deadline)
		msleep(1);
	check(now_ms() < deadline, "first operation owns its reservation");
	check(pthread_create(&b, NULL, block_operation, &second) == 0, "start same-direction contender");
	wait_demand(first.fd, tx ? 0 : 1, tx);
	check(join_thread(a) == 0, "first lease resolves");
	check(cancel ? first.result == -1 && first.error == EFAULT :
	      first.result == (int)(first.length ? first.length : sizeof(bytes)), "first copy outcome");
	check(join_thread(b) == 0 && second.result == (int)(second.length ? second.length : sizeof(bytes)),
	      "lease release wakes same-direction contender");
	if (tx) {
		for (int i = 0; i < (cancel ? 1 : 2); i++) {
			transfer(first.fd, bytes, sizeof(bytes), 0);
			for (unsigned int j = 0; j < sizeof(bytes); j++)
				check(bytes[j] == (i == 0 && !cancel ? 0x11 : 0x22), "serialized producer bytes");
		}
		finish(&server, 0);
	} else {
		no_demand(first.fd);
		terminate(&server);
		finish(&server, 1);
	}
	close(first.fd);
	sandbox_say("ok same-endpoint %s lease %s wakes registered contender",
		 tx ? "TX" : "RX", cancel ? "zero cancellation" : "commit");
}

static void zero_commit_fin(void)
{
	struct instance server = create("role=10 port=1234");
	struct blocked writer = { .operation = 3 };
	pthread_t thread;
	struct pollfd state;
	__u32 delay = 300;
	unsigned char byte;
	long long deadline;

	boot(&server);
	ready(&server);
	writer.fd = connect_instance(&server, 1234);
	check(ioctl(writer.fd, KLET_VSOCK_TEST_DELAY, &delay) == 0, "delay invalid admitted TX");
	check(pthread_create(&thread, NULL, block_operation, &writer) == 0, "start zero-prefix producer");
	state = (struct pollfd){ .fd = writer.fd, .events = POLLOUT };
	deadline = now_ms() + 5000;
	while (poll(&state, 1, 0) == 1 && now_ms() < deadline)
		msleep(1);
	check(now_ms() < deadline, "zero-prefix producer admitted");
	check(kv_shutdown(writer.fd, 1) == 0, "shutdown with admitted zero-prefix producer");
	wait_demand(writer.fd, 0, 0);
	check(join_thread(thread) == 0 && writer.result == -1 && writer.error == EFAULT,
	      "zero-prefix producer cancels after shutdown");
	state.events = POLLIN;
	check(poll(&state, 1, 5000) == 1 && read(writer.fd, &byte, 1) == 0,
	      "zero-prefix commit wakes reader to EOF");
	finish(&server, 0);
	close(writer.fd);
	sandbox_say("ok SHUT_WR plus zero TX cancellation enables peer EOF");
}

static void poll_cancel_lease(int tx)
{
	struct instance server = create(tx ? "role=2 port=1234" : "role=9 port=1234");
	struct blocked owner = { .operation = tx ? 3 : 4 };
	pthread_t thread;
	struct pollfd state;
	struct epoll_event event = { .events = (tx ? EPOLLOUT : EPOLLIN) | EPOLLET };
	struct klet_vsock_waiters demand;
	int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	__u32 delay = 300;
	long long deadline;

	boot(&server);
	ready(&server);
	owner.fd = connect_instance(&server, 1234);
	state = (struct pollfd){ .fd = owner.fd, .events = tx ? POLLOUT : POLLIN };
	check(poll(&state, 1, 5000) == 1, "pre-reservation event is ready");
	check(ioctl(owner.fd, tx ? KLET_VSOCK_TEST_DELAY : KLET_VSOCK_TEST_RX_DELAY, &delay) == 0,
	      "delay zero-prefix reservation for event-only waiter");
	check(pthread_create(&thread, NULL, block_operation, &owner) == 0, "start cancelling lease");
	deadline = now_ms() + 5000;
	while (poll(&state, 1, 0) == 1 && now_ms() < deadline)
		msleep(1);
	check(now_ms() < deadline, "reserved lease removes event readiness");
	check(epoll_fd >= 0 && epoll_ctl(epoll_fd, EPOLL_CTL_ADD, owner.fd, &event) == 0,
	      "register epoll while lease removes readiness");
	check(ioctl(owner.fd, KLET_VSOCK_TEST_WAITERS, &demand) == 0 &&
	      !demand.producer[tx ? 0 : 1] && !demand.consumer[tx ? 0 : 1],
	      "cancelled ring has event interest but no blocking operation demand");
	check(epoll_wait(epoll_fd, &event, 1, 5000) == 1 &&
	      (event.events & (tx ? EPOLLOUT : EPOLLIN)), "zero-prefix cancellation restores event readiness");
	check(join_thread(thread) == 0 && owner.result == -1 && owner.error == EFAULT,
	      "event-only cancelling copy faulted before first byte");
	terminate(&server);
	finish(&server, 1);
	close(epoll_fd);
	close(owner.fd);
	sandbox_say("ok epoll-only zero %s lease cancellation restores readiness", tx ? "TX" : "RX");
}

static void noop_signal(int sig)
{
	(void)sig;
}

static void interrupted_wait(int tx)
{
	struct instance server = create(tx ? "role=8 size=4096 loops=17 port=1234" : "role=2 port=1234");
	struct blocked waiter = { .operation = tx ? 1 : 0 };
	struct sigaction action = { .sa_handler = noop_signal };
	pthread_t thread;
	unsigned char bytes[4096] = {0};
	int control;

	sigemptyset(&action.sa_mask);
	check(sigaction(SIGUSR1, &action, NULL) == 0, "non-restarting signal handler");
	boot(&server);
	ready(&server);
	waiter.fd = connect_instance(&server, 1234);
	if (tx)
		for (int i = 0; i < 16; i++)
			transfer(waiter.fd, bytes, sizeof(bytes), 1);
	check(pthread_create(&thread, NULL, block_operation, &waiter) == 0, "start interruptible waiter");
	wait_demand(waiter.fd, tx ? 0 : 1, tx);
	check(pthread_kill(thread, SIGUSR1) == 0 && join_thread(thread) == 0 &&
	      waiter.result == -1 && waiter.error == EINTR, "signal unwinds registered wait");
	no_demand(waiter.fd);
	if (tx) {
		control = open_gate(&server);
		check(write(control, bytes, 1) == 1, "release consumer after interrupted writer");
		close(control);
		transfer(waiter.fd, bytes, sizeof(bytes), 1);
		for (int i = 0; i < 17; i++)
			transfer(waiter.fd, bytes, sizeof(bytes), 0);
		finish(&server, 0);
	} else {
		terminate(&server);
		finish(&server, 1);
		no_demand(waiter.fd);
	}
	close(waiter.fd);
	sandbox_say("ok interrupted %s balances demand and preserves later progress", tx ? "writer" : "reader");
}

static void poll_publication(int edge)
{
	struct instance server = create("role=0 size=4096 loops=2 port=1234");
	struct blocked writer = { .operation = 1, .fill = 0x31 };
	pthread_t thread;
	struct epoll_event event = { .events = EPOLLIN | (edge ? EPOLLET : 0) };
	unsigned char bytes[4096];
	__u32 delay = 100;
	int epoll_fd = edge >= 0 ? epoll_create1(EPOLL_CLOEXEC) : -1;

	boot(&server);
	ready(&server);
	writer.fd = connect_instance(&server, 1234);
	check(ioctl(writer.fd, KLET_VSOCK_TEST_DELAY, &delay) == 0, "delay poll publication");
	if (edge >= 0) {
		check(epoll_fd >= 0 && epoll_ctl(epoll_fd, EPOLL_CTL_ADD, writer.fd, &event) == 0,
		      "register epoll-only receive interest");
	}
	for (int round = 0; round < 2; round++) {
		struct pollfd state = { .fd = writer.fd, .events = POLLIN };
		struct klet_vsock_waiters demand;

		check(ioctl(writer.fd, KLET_VSOCK_TEST_WAITERS, &demand) == 0 && !demand.consumer[1],
		      "RX event subscriber has no blocking read demand");
		check(pthread_create(&thread, NULL, block_operation, &writer) == 0, "delayed event producer");
		check(edge >= 0 ? epoll_wait(epoll_fd, &event, 1, 5000) == 1 :
		      poll(&state, 1, 5000) == 1, "publication wakes poll/epoll-only subscriber");
		check(join_thread(thread) == 0 && writer.result == sizeof(bytes), "event producer completes");
		transfer(writer.fd, bytes, sizeof(bytes), 0);
		for (unsigned int i = 0; i < sizeof(bytes); i++)
			check(bytes[i] == 0x31, "event payload");
		if (round == 0) {
			int flags = fcntl(writer.fd, F_GETFL);

			check(fcntl(writer.fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
			      read(writer.fd, bytes, sizeof(bytes)) == -1 && errno == EAGAIN &&
			      fcntl(writer.fd, F_SETFL, flags) == 0, "edge receive drains to EAGAIN before next event");
		}
	}
	if (epoll_fd >= 0)
		close(epoll_fd);
	close(writer.fd);
	finish(&server, 0);
	sandbox_say("ok %s-only RX publication and repeated readiness", edge < 0 ? "poll" : edge ? "edge epoll" : "level epoll");
}

static void poll_capacity(void)
{
	struct instance server = create("role=8 size=4096 loops=17 port=1234");
	struct epoll_event event = { .events = EPOLLOUT | EPOLLET };
	struct klet_vsock_waiters demand;
	unsigned char bytes[4096] = {0};
	int fd, control, epoll_fd = epoll_create1(EPOLL_CLOEXEC);

	boot(&server);
	ready(&server);
	fd = connect_instance(&server, 1234);
	for (int i = 0; i < 16; i++)
		transfer(fd, bytes, sizeof(bytes), 1);
	check(epoll_fd >= 0 && epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0 &&
	      epoll_wait(epoll_fd, &event, 1, 20) == 0, "full ring has no output readiness");
	check(ioctl(fd, KLET_VSOCK_TEST_WAITERS, &demand) == 0 && !demand.producer[0],
	      "TX event subscriber has no blocking producer demand");
	control = open_gate(&server);
	check(write(control, bytes, 1) == 1, "enable consumption for output event");
	close(control);
	check(epoll_wait(epoll_fd, &event, 1, 5000) == 1 && (event.events & EPOLLOUT),
	      "consumption wakes epoll-only producer");
	transfer(fd, bytes, sizeof(bytes), 1);
	for (int i = 0; i < 17; i++)
		transfer(fd, bytes, sizeof(bytes), 0);
	close(epoll_fd);
	close(fd);
	finish(&server, 0);
	sandbox_say("ok epoll-only full-to-writable consumption");
}

static void kill_waiting_image(void)
{
	struct instance server = create("role=0 size=4096 loops=1 port=1234");
	struct epoll_event event = { .events = EPOLLIN | EPOLLET };
	unsigned char byte;
	int fd, epoll_fd = epoll_create1(EPOLL_CLOEXEC);

	boot(&server);
	ready(&server);
	fd = connect_instance(&server, 1234);
	wait_demand(fd, 0, 0);
	check(epoll_fd >= 0 && epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0,
	      "register peer-death event interest");
	terminate(&server);
	check(epoll_wait(epoll_fd, &event, 1, 5000) == 1, "peer death wakes epoll-only subscriber");
	finish(&server, 1);
	no_demand(fd);
	check(read(fd, &byte, 1) == -1 && errno == ECONNRESET, "dead registered image reports reset");
	close(epoll_fd);
	close(fd);
	sandbox_say("ok killed waiting image unregisters demand and wakes epoll peer");
}

static void backlog_progress(void)
{
	struct instance server = create("role=11 port=1234");
	struct blocked connector = { .operation = 2 };
	pthread_t thread;
	int first, control;
	unsigned char byte = 0;

	boot(&server);
	ready(&server);
	first = connect_instance(&server, 1234);
	connector.fd = kv_socket(system_fd);
	connector.cid = server.cid;
	check(connector.fd >= 0 && pthread_create(&thread, NULL, block_operation, &connector) == 0,
	      "start backlog-limited connector");
	while (!__atomic_load_n(&connector.entered, __ATOMIC_ACQUIRE))
		msleep(1);
	msleep(20);
	check(pthread_tryjoin_np(thread, NULL) == EBUSY, "connector waits for accept to free backlog");
	control = open_gate(&server);
	check(write(control, &byte, 1) == 1, "enable controlled accepts");
	close(control);
	check(join_thread(thread) == 0 && connector.result == 0, "accept releases connector backlog wait");
	close(first);
	close(connector.fd);
	finish(&server, 0);
	sandbox_say("ok accept wakes connector after bounded backlog release");
}

struct stress_peer {
	int fd, tag;
};

static void *stress_roundtrips(void *arg)
{
	struct stress_peer *peer = arg;
	unsigned char sent[4096], received[4096];

	for (int round = 0; round < 256; round++) {
		for (unsigned int i = 0; i < sizeof(sent); i++)
			sent[i] = (round * 31 + i * 17 + peer->tag) % 251;
		transfer(peer->fd, sent, sizeof(sent), 1);
		transfer(peer->fd, received, sizeof(received), 0);
		check(!memcmp(sent, received, sizeof(sent)), "concurrent independent ring ordering");
	}
	return NULL;
}

static void registration_stress(void)
{
	struct instance server[2];
	struct stress_peer peer[2];
	pthread_t thread[2];

	for (int i = 0; i < 2; i++) {
		server[i] = create("role=0 size=4096 loops=256 port=1234");
		boot(&server[i]);
		ready(&server[i]);
		peer[i] = (struct stress_peer){ .fd = connect_instance(&server[i], 1234), .tag = i + 1 };
	}
	for (int i = 0; i < 2; i++)
		check(pthread_create(&thread[i], NULL, stress_roundtrips, &peer[i]) == 0, "start concurrent rings");
	for (int i = 0; i < 2; i++) {
		check(join_thread(thread[i]) == 0, "concurrent ring registration completes before timeout");
		close(peer[i].fd);
		finish(&server[i], 0);
	}
	sandbox_say("ok concurrent independent rings stress check-register-commit retries");
}

static void benchmark(int pipes, unsigned int size, int repeat)
{
	struct instance instance = {0};
	int rx, tx, to_peer[2], from_peer[2], finish_pipe[2], control = -1;
	pid_t peer;
	struct vb_result outer = {0};
	uint32_t token;
	char command[128];
	unsigned char sent[KLET_VSOCK_CAPACITY], received[KLET_VSOCK_CAPACITY];
	uint64_t samples[bench_rounds], wall = 0, cpu = 0, other_cpu = 0, end;
	unsigned int i;

	memset(sent, 0x69, size);
	if (profiling) {
		sandbox_say("PROFILE-BEGIN transport=%s size=%u rounds=%u warmup=%u",
			    pipes == 2 ? "api" : "klet", size, bench_rounds, bench_warmup);
		check(ioctl(system_fd, KLET_VSOCK_PROFILE_RESET) == 0, "reset isolated diagnostic case");
	}
	if (pipes == 1) {
		check(pipe(to_peer) == 0 && pipe(from_peer) == 0 && pipe(finish_pipe) == 0, "create pipes");
		check(fcntl(to_peer[1], F_SETPIPE_SZ, KLET_VSOCK_CAPACITY) == KLET_VSOCK_CAPACITY &&
		      fcntl(from_peer[1], F_SETPIPE_SZ, KLET_VSOCK_CAPACITY) == KLET_VSOCK_CAPACITY,
		      "match each pipe capacity to one ring");
		peer = fork();
		check(peer >= 0, "fork pipe peer");
		if (!peer) {
			pin(0);
			close(to_peer[1]);
			close(from_peer[0]);
			close(finish_pipe[1]);
			for (i = 0; i < bench_rounds + bench_warmup; i++) {
				transfer(to_peer[0], received, size, 0);
				transfer(from_peer[1], received, size, 1);
			}
			check(read(finish_pipe[0], received, 1) == 0, "pipe peer released after sampling");
			_exit(0);
		}
		close(to_peer[0]);
		close(from_peer[1]);
		close(finish_pipe[0]);
		tx = to_peer[1];
		rx = from_peer[0];
	} else if (pipes == 3) {
		rx = tx = control = comparison_control(pipes, size, repeat);
		peer = 0;
	} else {
		snprintf(command, sizeof(command), "role=%u size=%u loops=%u port=1234 profile=%d",
			 pipes == 2 ? 7 : 0, size, bench_rounds + bench_warmup, profiling);
		instance = create(command);
		boot(&instance);
		ready(&instance);
		rx = tx = connect_instance(&instance, 1234);
		peer = instance.pid;
	}
	if (comparison && control < 0)
		control = comparison_control(pipes, size, repeat);
	for (i = 0; i < bench_rounds + bench_warmup; i++) {
		uint64_t begin;

		if (i == bench_warmup) {
			if (comparison) {
				token = VB_BEGIN;
				check(vb_full(control, &token, sizeof(token), 1) == 0 &&
				      vb_full(control, &token, sizeof(token), 0) == 0 && token == VB_BEGIN,
				      "outer CPU window begins");
			}
			other_cpu = peer ? cpu_ns(peer) : 0;
			cpu = ns(CLOCK_PROCESS_CPUTIME_ID);
			wall = ns(CLOCK_MONOTONIC);
		}
		memcpy(sent, &i, sizeof(i));
		begin = ns(CLOCK_MONOTONIC);
		transfer(tx, sent, size, 1);
		transfer(rx, received, size, 0);
		check(!memcmp(sent, received, size), "benchmark payload integrity");
		if (i >= bench_warmup)
			samples[i - bench_warmup] = ns(CLOCK_MONOTONIC) - begin;
	}
	end = ns(CLOCK_MONOTONIC);
	cpu = ns(CLOCK_PROCESS_CPUTIME_ID) - cpu;
	other_cpu = peer ? cpu_ns(peer) - other_cpu : 0;
	wall = end - wall;
	if (comparison) {
		token = VB_END;
		check(vb_full(control, &token, sizeof(token), 1) == 0 &&
		      vb_full(control, &outer, sizeof(outer), 0) == 0 && outer.buffer_bytes == VB_CAPACITY,
		      "outer CPU window ends with matched credit buffer");
		if (!peer)
			other_cpu = outer.peer_cpu_ns;
	}
	qsort(samples, bench_rounds, sizeof(*samples), compare_u64);
	sandbox_say("BENCH transport=%s size=%u repeat=%d rounds=%u bytes=%llu wall_ns=%llu cpu_ns=%llu peer_cpu_ns=%llu p50_ns=%llu p95_ns=%llu outer_qemu_ns=%llu outer_vhost_ns=%llu outer_peer_ns=%llu outer_wall_ns=%llu",
		pipes == 1 ? "pipe" : pipes == 2 ? "api" : pipes == 3 ? "virtio" : "klet", size, repeat, bench_rounds,
		(unsigned long long)size * bench_rounds * 2, (unsigned long long)wall,
		(unsigned long long)cpu, (unsigned long long)other_cpu,
		(unsigned long long)samples[bench_rounds / 2], (unsigned long long)samples[bench_rounds * 95 / 100],
		(unsigned long long)outer.qemu_cpu_ns, (unsigned long long)outer.vhost_cpu_ns,
		(unsigned long long)outer.peer_cpu_ns, (unsigned long long)outer.outer_wall_ns);
	if (control >= 0 && control != tx)
		close(control);
	close(tx);
	if (pipes == 1) {
		int status;

		close(rx);
		close(finish_pipe[1]);
		check(waitpid(peer, &status, 0) == peer && WIFEXITED(status) && !WEXITSTATUS(status), "pipe peer exits");
	} else if (pipes != 3) {
		finish(&instance, 0);
	}
	if (profiling)
		check(ioctl(system_fd, KLET_VSOCK_PROFILE_REPORT) == 0, "report isolated diagnostic case");
}

int main(int argc, char **argv)
{
	unsigned int sizes[] = {4096, 16384, 65536};
	unsigned int size;
	int repeat;

	sandbox_who = "VSOCK-TEST";
	check(argc == 2 || argc == 3, "usage: run-vsock IMAGE [compare|profile]");
	comparison = argc == 3 && !strcmp(argv[2], "compare");
	profiling = argc == 3 && !strcmp(argv[2], "profile");
	check(argc == 2 || comparison || profiling, "known experiment mode");
	if (comparison) {
		check(!strcmp(argv[2], "compare"), "comparison mode");
		bench_rounds = 8192;
		bench_warmup = 64;
	}
	if (profiling) {
		bench_rounds = 4096;
		bench_warmup = 64;
	}
	signal(SIGPIPE, SIG_IGN);
	pin(1);
	system_fd = open_system();
	kind = register_kind(system_fd, "vsocktest", argv[1]);
	join_cgroup("/sys/fs/cgroup/vsock-test");
	if (comparison)
		configure_virtio();
	if (!profiling) {
		functional();
		authorization();
		partial_faults();
		peers();
		survivor();
		host_listener();
		killed_waiter(0);
		killed_waiter(1);
		killed_waiter(2);
		reservation_race(0);
		reservation_race(1);
		consume_wakes_writer();
		lease_contention(1, 0);
		lease_contention(1, 1);
		lease_contention(0, 0);
		lease_contention(0, 1);
		zero_commit_fin();
		poll_cancel_lease(1);
		poll_cancel_lease(0);
		interrupted_wait(0);
		interrupted_wait(1);
		poll_publication(-1);
		poll_publication(0);
		poll_publication(1);
		poll_capacity();
		kill_waiting_image();
		backlog_progress();
		registration_stress();
		// Re-establish useful communication after all three peer-failure cases.
		functional();
	}
	for (size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++)
		for (repeat = 1; repeat <= (comparison ? 7 : profiling ? 1 : 3); repeat++) {
			if (profiling) {
				benchmark(0, sizes[size], repeat);
				benchmark(2, sizes[size], repeat);
			} else if (comparison) {
				for (unsigned int position = 0; position < 4; position++)
					benchmark((position + repeat + size) % 4, sizes[size], repeat);
			} else {
				benchmark(repeat % 2, sizes[size], repeat);
				benchmark(!(repeat % 2), sizes[size], repeat);
				benchmark(2, sizes[size], repeat);
			}
		}
	if (comparison)
		report_virtio_delivery();
	check(ioctl(system_fd, KLET_KIND_UNREGISTER, &kind) == 0, "unregister test image");
	close(system_fd);
	check(module_references("endovisor") == 0, "module references return to zero");
	sandbox_say("PASS lifecycle, rings, backpressure, peer death, survivors, end-to-end measurements");
	return 0;
}
