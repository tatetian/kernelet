// SPDX-License-Identifier: GPL-2.0
// Runs the real host peer and samples outer-host CPU without guest double counting.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "vsock-bench.h"

static int qemu_pid;
static int vhost_tids[16], nr_vhost;

static int is_vhost(int tid)
{
	for (int i = 0; i < nr_vhost; i++)
		if (vhost_tids[i] == tid)
			return 1;
	return 0;
}

static void require(int ok, const char *operation)
{
	if (!ok) {
		perror(operation);
		exit(1);
	}
}

static uint64_t clock_ns(clockid_t clock)
{
	struct timespec ts;

	require(clock_gettime(clock, &ts) == 0, "clock");
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static uint64_t task_runtime(const char *path)
{
	unsigned long long value;
	FILE *file = fopen(path, "r");

	require(file && fscanf(file, "%llu", &value) == 1, "read schedstat");
	fclose(file);
	return value;
}

static uint64_t qemu_runtime(void)
{
	char directory[128], path[256];
	DIR *tasks;
	struct dirent *entry;
	uint64_t total = 0;

	snprintf(directory, sizeof(directory), "/proc/%d/task", qemu_pid);
	tasks = opendir(directory);
	require(tasks != NULL, "QEMU tasks");
	while ((entry = readdir(tasks))) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
			continue;
		if (is_vhost(atoi(entry->d_name)))
			continue;
		snprintf(path, sizeof(path), "/proc/%d/task/%d/schedstat", qemu_pid, atoi(entry->d_name));
		total += task_runtime(path);
	}
	closedir(tasks);
	return total;
}

static uint64_t vhost_runtime(uint64_t *count)
{
    char path[128];
    uint64_t total = 0;

    *count = nr_vhost;
    for (int i = 0; i < nr_vhost; i++) {
        snprintf(path, sizeof(path), "/proc/%d/schedstat", vhost_tids[i]);
        total += task_runtime(path);
    }
    return total;
}

static void session(int fd)
{
	struct vb_header header;
	struct vb_result result = {0};
	unsigned char bytes[VB_CAPACITY];
	uint32_t token, i;
	uint64_t before_cpu, before_qemu, before_vhost, before_wall, workers;

	require(vb_buffers(fd) == 0, "accepted vsock buffer");
	require(vb_full(fd, &header, sizeof(header), 0) == 0, "session header");
	require(header.magic == VB_MAGIC && header.size >= 4096 && header.size <= VB_CAPACITY &&
		header.rounds > 0 && header.rounds <= 65536 && header.warmup <= 1024 && header.transport <= 3,
		"valid session");
	token = VB_MAGIC;
	require(vb_full(fd, &token, sizeof(token), 1) == 0, "header acknowledgement");
	if (header.transport == 3)
		for (i = 0; i < header.warmup; i++) {
			require(vb_full(fd, bytes, header.size, 0) == 0, "warmup read");
			require(vb_full(fd, bytes, header.size, 1) == 0, "warmup write");
		}
	require(vb_full(fd, &token, sizeof(token), 0) == 0 && token == VB_BEGIN, "begin token");
	before_vhost = vhost_runtime(&workers);
	require(workers > 0, "identified vhost worker");
	before_qemu = qemu_runtime();
	before_cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	before_wall = clock_ns(CLOCK_MONOTONIC);
	require(vb_full(fd, &token, sizeof(token), 1) == 0, "begin acknowledgement");
	if (header.transport == 3)
		for (i = 0; i < header.rounds; i++) {
			require(vb_full(fd, bytes, header.size, 0) == 0, "echo read");
			require(vb_full(fd, bytes, header.size, 1) == 0, "echo write");
		}
	require(vb_full(fd, &token, sizeof(token), 0) == 0 && token == VB_END, "end token");
	result.outer_wall_ns = clock_ns(CLOCK_MONOTONIC) - before_wall;
	result.peer_cpu_ns = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - before_cpu;
	result.qemu_cpu_ns = qemu_runtime() - before_qemu;
	result.vhost_cpu_ns = vhost_runtime(&result.vhost_tasks) - before_vhost;
	result.buffer_bytes = VB_CAPACITY;
	require(result.vhost_tasks == workers, "stable vhost worker count");
	require(vb_full(fd, &result, sizeof(result), 1) == 0, "result");
	printf("OUTER transport=%u size=%u repeat=%u qemu_ns=%llu vhost_ns=%llu peer_ns=%llu wall_ns=%llu workers=%llu\n",
	       header.transport, header.size, header.repeat,
	       (unsigned long long)result.qemu_cpu_ns, (unsigned long long)result.vhost_cpu_ns,
	       (unsigned long long)result.peer_cpu_ns, (unsigned long long)result.outer_wall_ns,
	       (unsigned long long)result.vhost_tasks);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	struct sockaddr_vm address = { .svm_family = AF_VSOCK, .svm_cid = VMADDR_CID_HOST,
		.svm_port = VB_PORT };
	cpu_set_t set;
	int listener, fd, cpu;

	require(argc == 4, "usage: vsock-peer QEMU_PID CPU VHOST_TIDS");
    {
        char *save, *tid = strtok_r(argv[3], ",", &save);

        while (tid && nr_vhost < 16) {
            vhost_tids[nr_vhost++] = atoi(tid);
            tid = strtok_r(NULL, ",", &save);
        }
        require(nr_vhost > 0 && !tid, "bounded vhost task list");
    }
	qemu_pid = atoi(argv[1]);
	cpu = atoi(argv[2]);
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	require(sched_setaffinity(0, sizeof(set), &set) == 0, "pin host peer");
	signal(SIGPIPE, SIG_IGN);
	listener = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
	require(listener >= 0 && vb_buffers(listener) == 0 &&
		bind(listener, (void *)&address, sizeof(address)) == 0 && listen(listener, 4) == 0,
		"host vsock listener");
	puts("VSOCK-PEER ready");
	fflush(stdout);
	for (;;) {
		fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
		require(fd >= 0, "accept");
		session(fd);
		close(fd);
	}
}
