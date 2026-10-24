// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "shared_log_v2.h"

_Static_assert(sizeof(struct md_shared_slot) == 64, "v3 slot ABI");
_Static_assert(sizeof(struct md_shared_log) == 128, "v3 header ABI");

#define MD_PRCTL 74
#define MD_FLAGS ((1UL << 2) | (1UL << 4))
#define CAPACITY 16U
#define ARENA_PAGES 32U
#define BASE ((void *)0x3000000000UL)
#define CHECK(x)                                                           \
	do {                                                               \
		if (!(x)) {                                                \
			fprintf(stderr, "FAIL(v3): line=%d %s errno=%d\n", \
				__LINE__, #x, errno);                      \
			exit(1);                                           \
		}                                                          \
	} while (0)

static struct md_shared_log *log_area;
static size_t page_size, log_bytes;
static int payload_fd, log_fd;
static unsigned int owner_cpu, other_cpu;
static unsigned int remote_cpus[CPU_SETSIZE], remote_count;
static uint64_t session;
static pthread_mutex_t *arena_mutex;

static void pin_cpu(unsigned int cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	CHECK(sched_setaffinity(0, sizeof(set), &set) == 0);
}

static uint64_t register_session(void *base, struct md_shared_log *logs)
{
	struct md_shared_registration req = {
		.size = sizeof(req),
		.version = MD_SHARED_LOG_VERSION,
		.log_address = (uintptr_t)logs,
		.log_bytes = log_bytes,
	};
	CHECK(prctl(MD_PRCTL, &req, MD_FLAGS, owner_cpu, base) == 0);
	CHECK(req.session != 0);
	return req.session;
}

static struct md_shared_record record(uint64_t id, unsigned int start)
{
	struct md_shared_record r = {
		.session = session,
		.block_id = start + 1,
		.generation = id,
		.allocation_id = id,
		.start_page = start,
		.nr_pages = 2,
		.cpu = owner_cpu,
		.op = MD_LOG_ALLOC,
	};
	return r;
}

static uint64_t normal_publish(struct md_shared_record r)
{
	uint64_t ticket;
	CHECK(pthread_mutex_lock(arena_mutex) == 0);
	CHECK(sched_getcpu() == (int)owner_cpu);
	CHECK(md_v2_normal_publish(log_area, &r, &ticket) == 0);
	CHECK(pthread_mutex_unlock(arena_mutex) == 0);
	return ticket;
}

static void wait_completed(int remote, uint64_t target)
{
	struct timespec begin, now;
	clock_gettime(CLOCK_MONOTONIC, &begin);
	while (__atomic_load_n(remote ? &log_area->remote.completed :
				       &log_area->normal.head, __ATOMIC_ACQUIRE) < target) {
		CHECK(__atomic_load_n(&log_area->state, __ATOMIC_ACQUIRE) == MD_SHARED_LOG_RUNNING);
		sched_yield();
		usleep(100);
		clock_gettime(CLOCK_MONOTONIC, &now);
		CHECK(now.tv_sec - begin.tv_sec < 10);
	}
}

static int result(int remote, uint64_t ticket)
{
	wait_completed(remote, ticket + 1);
	if (!remote) return 0; /* head is an all-success prefix; slots may be reused */
	struct md_shared_slot *s = &log_area->slots[CAPACITY + ticket % CAPACITY];
	CHECK(s->ready == ticket + 1);
	return s->result;
}

static void reclaim_all(int remote)
{
	if (!remote) return; /* normal slot recycling is kernel-owned */
	CHECK(pthread_mutex_lock(arena_mutex) == 0);
	CHECK(sched_getcpu() == (int)owner_cpu);
	__atomic_store_n(&log_area->remote.reclaimed,
			 __atomic_load_n(&log_area->remote.completed, __ATOMIC_ACQUIRE),
			 __ATOMIC_RELEASE);
	CHECK(pthread_mutex_unlock(arena_mutex) == 0);
}

struct producer {
	struct md_shared_record r;
	uint64_t ticket;
	int publish;
	unsigned int cpu_index;
};
static void *remote_producer(void *arg)
{
	struct producer *p = arg;
	pin_cpu(remote_cpus[p->cpu_index % remote_count]);
	CHECK(md_v2_remote_reserve(log_area, &p->ticket) == 0);
	if (p->publish)
		md_v2_remote_publish(log_area, p->ticket, &p->r);
	return NULL;
}

static uint64_t remote_publish(struct md_shared_record r)
{
	pthread_t thread;
	struct producer p = { .r = r, .publish = 1 };
	CHECK(pthread_create(&thread, NULL, remote_producer, &p) == 0);
	CHECK(pthread_join(thread, NULL) == 0);
	return p.ticket;
}

static void expect_child_segv(void *addr)
{
	pid_t pid = fork();
	int status;
	CHECK(pid >= 0);
	if (!pid) {
		pin_cpu(owner_cpu);
		register_session(BASE, log_area);
		/* Registration queues sync via a subsequent context switch. */
		usleep(10000);
		(void)*(volatile unsigned char *)addr;
		_exit(99);
	}
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}

int main(int argc, char **argv)
{
	cpu_set_t allowed;
	unsigned int cpu;
	pthread_mutexattr_t attr;
	pthread_t thread;
	struct producer hole;
	struct md_shared_record a, b, stale;
	uint64_t ticket, tail, held_ready;
	int status, pipes[2];
	pid_t pid;

	if (argc == 3 && !strcmp(argv[1], "--pump")) {
		pin_cpu(strtoul(argv[2], NULL, 10));
		for (;;) {
			sched_yield();
			usleep(100);
		}
	}
	setbuf(stdout, NULL);
	CHECK(prctl(MD_PRCTL, 0UL, 1UL << 4, 0UL, MD_SHARED_LOG_QUERY) == 3);
	page_size = sysconf(_SC_PAGESIZE);
	CHECK(sched_getaffinity(0, sizeof(allowed), &allowed) == 0);
	owner_cpu = other_cpu = UINT_MAX;
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &allowed)) {
			if (owner_cpu == UINT_MAX)
				owner_cpu = cpu;
			else {
				remote_cpus[remote_count++] = cpu;
				if (other_cpu == UINT_MAX)
					other_cpu = cpu;
			}
		}
	CHECK(other_cpu != UINT_MAX);
	pin_cpu(owner_cpu);
	log_bytes = ((sizeof(*log_area) +
		      2 * CAPACITY * sizeof(struct md_shared_slot) + page_size -
		      1) /
		     page_size) *
		    page_size;
	payload_fd = memfd_create("md-v2-payload", 0);
	log_fd = memfd_create("md-v2-logs", 0);
	CHECK(payload_fd >= 0 && log_fd >= 0);
	CHECK(ftruncate(payload_fd, ARENA_PAGES * page_size) == 0);
	CHECK(ftruncate(log_fd, log_bytes) == 0);
	CHECK(mmap(BASE, ARENA_PAGES * page_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_FIXED_NOREPLACE, payload_fd, 0) == BASE);
	log_area = mmap(NULL, log_bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
			log_fd, 0);
	CHECK(log_area != MAP_FAILED);
	log_area->magic = MD_SHARED_LOG_MAGIC;
	log_area->version = MD_SHARED_LOG_VERSION;
	log_area->capacity = CAPACITY;
	log_area->arena_nr_pages = ARENA_PAGES;
	log_area->log_bytes = log_bytes;
	arena_mutex = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(arena_mutex != MAP_FAILED);
	CHECK(pthread_mutexattr_init(&attr) == 0);
	CHECK(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0);
	CHECK(pthread_mutex_init(arena_mutex, &attr) == 0);
	errno = 0;
	CHECK(prctl(MD_PRCTL, log_area, 1UL << 2, owner_cpu, BASE) == -1 &&
	      errno == EPROTONOSUPPORT);
	CHECK(prctl(MD_PRCTL, 0, 1UL << 1, owner_cpu, BASE) == -1 &&
	      errno == EOPNOTSUPP);
	session = register_session(BASE, log_area);
	CHECK(register_session(BASE, log_area) == session);

	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		int fd = memfd_create("wrong-arena-logs", 0);
		struct md_shared_log *wrong;
		struct md_shared_registration req = {
			.size = sizeof(req),
			.version = MD_SHARED_LOG_VERSION,
			.log_bytes = log_bytes,
		};
		CHECK(fd >= 0 && ftruncate(fd, log_bytes) == 0);
		wrong = mmap(NULL, log_bytes, PROT_READ | PROT_WRITE,
			     MAP_SHARED, fd, 0);
		CHECK(wrong != MAP_FAILED);
		memcpy(wrong, log_area, log_bytes);
		req.log_address = (uintptr_t)wrong;
		CHECK(prctl(MD_PRCTL, &req, MD_FLAGS, owner_cpu, BASE) == -1 &&
		      errno == EXDEV);
		_exit(0);
	}
	CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	      WEXITSTATUS(status) == 0);
	puts("PASS(v3): explicit capabilities, stable session, incompatible shared backing rejected");

	a = record(1, 0);
	b = record(2, 2);
	ticket = normal_publish(a);
	CHECK(result(0, ticket) == 0);
	ticket = normal_publish(b);
	CHECK(result(0, ticket) == 0);
	reclaim_all(0);
	memset(BASE, 0x5a, 4 * page_size);
	expect_child_segv(BASE);
	puts("PASS(v3): registered nonowner faults with SIGSEGV after sync");

	/* A paused reservation must not expose its record or allow ticket 1 past. */
	a.op = b.op = MD_LOG_FREE;
	hole = (struct producer){ .r = a };
	CHECK(pthread_create(&thread, NULL, remote_producer, &hole) == 0);
	CHECK(pthread_join(thread, NULL) == 0);
	ticket = remote_publish(b);
	for (int n = 0; n < 30; n++) {
		sched_yield();
		usleep(100);
	}
	CHECK(__atomic_load_n(&log_area->remote.completed, __ATOMIC_ACQUIRE) ==
	      hole.ticket);
	md_v2_remote_publish(log_area, hole.ticket, &a);
	CHECK(result(1, hole.ticket) == 0 && result(1, ticket) == 0);
	reclaim_all(1);
	puts("PASS(v3): reserved hole stops without waiting or skipping; remote-only producers");

	/* PRECEDING ALLOC can remain pending until the remote FREE is published. */
	a = record(3, 0);
	ticket = normal_publish(a);
	a.op = MD_LOG_FREE;
	tail = remote_publish(a);
	CHECK(result(0, ticket) == 0 && result(1, tail) == 0);
	reclaim_all(0);
	reclaim_all(1);
	puts("PASS(v3): normal ALLOC commits before dependent remote FREE");

	stale = a;
	a = record(4, 0);
	ticket = normal_publish(a);
	CHECK(result(0, ticket) == 0);
	reclaim_all(0);
	CHECK(result(1, remote_publish(stale)) == -ESTALE);
	reclaim_all(1);
	b = a;
	b.op = MD_LOG_FREE;
	b.session = UINT64_MAX;
	CHECK(result(1, remote_publish(b)) == -ESTALE);
	reclaim_all(1);
	b = a;
	b.op = MD_LOG_FREE;
	b.nr_pages = 1;
	CHECK(result(1, remote_publish(b)) == -ESTALE);
	reclaim_all(1);
	b = a;
	b.op = MD_LOG_FREE;
	CHECK(result(1, remote_publish(b)) == 0);
	reclaim_all(1);
	CHECK(result(1, remote_publish(b)) != 0);
	reclaim_all(1);
	b.cpu = other_cpu;
	CHECK(result(1, remote_publish(b)) == -EINVAL);
	reclaim_all(1);
	b.cpu = owner_cpu;
	puts("PASS(v3): stale, foreign-session, partial, mixed-arena and duplicate FREE rejected individually");

	/* Wrap/reuse more than one ring without ANY user normal reclaim. */
	for (unsigned int n = 0; n < CAPACITY * 4; n++) {
		a = record(100 + n, 0);
		CHECK(result(0, normal_publish(a)) == 0);
		a.op = MD_LOG_FREE;
		CHECK(result(0, normal_publish(a)) == 0);
	}
	CHECK(log_area->normal.head == log_area->normal.reserved);
	puts("PASS(v3): kernel directly recycles normal success slots across wrap");

	/* All consumed results remain intact until user reclaim, including wrap. */
	for (int round = 0; round < 32; round++) {
		tail = log_area->remote.reserved;
		for (unsigned int n = 0; n < CAPACITY; n++)
			remote_publish(b);
		wait_completed(1, tail + CAPACITY);
		held_ready = log_area->slots[CAPACITY + tail % CAPACITY].ready;
		CHECK(md_v2_remote_reserve(log_area, &ticket) == -ENOSPC);
		CHECK(log_area->slots[CAPACITY + tail % CAPACITY].ready ==
		      held_ready);
		for (unsigned int n = 0; n < CAPACITY; n++)
			CHECK(result(1, tail + n) != 0);
		reclaim_all(1);
	}
	puts("PASS(v3): wraparound retains per-ticket errors until explicit user reclaim");

	/* Several producers compete for unique reservations; no descriptor writes. */
	{
		pthread_t threads[CAPACITY];
		struct producer producers[CAPACITY];
		tail = log_area->remote.reserved;
		for (unsigned int n = 0; n < CAPACITY; n++) {
			producers[n] = (struct producer){ .r = b,
							  .publish = 1,
							  .cpu_index = n };
			CHECK(pthread_create(&threads[n], NULL, remote_producer,
					     &producers[n]) == 0);
		}
		for (unsigned int n = 0; n < CAPACITY; n++) {
			CHECK(pthread_join(threads[n], NULL) == 0);
			CHECK(producers[n].ticket >= tail &&
			      producers[n].ticket < tail + CAPACITY);
			for (unsigned int j = 0; j < n; j++)
				CHECK(producers[n].ticket !=
				      producers[j].ticket);
			CHECK(result(1, producers[n].ticket) != 0);
		}
		reclaim_all(1);
	}
	printf("PASS(v3): concurrent producers on %u remote CPUs reserve unique slots\n",
	       remote_count);

	/* A dead mm's session cannot allocate, and shared storage survives exit. */
	CHECK(pipe(pipes) == 0);
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		uint64_t child_session = register_session(BASE, log_area);
		close(pipes[0]);
		CHECK(write(pipes[1], &child_session, sizeof(child_session)) ==
		      sizeof(child_session));
		_exit(0);
	}
	close(pipes[1]);
	CHECK(read(pipes[0], &tail, sizeof(tail)) == sizeof(tail));
	close(pipes[0]);
	CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	      WEXITSTATUS(status) == 0);
	a = record(200, 0);
	a.session = tail;
	a.op = MD_LOG_FREE;
	CHECK(result(1, remote_publish(a)) == -ESTALE);
	reclaim_all(1);
	a.op = MD_LOG_ALLOC;
	a.session = session;
	CHECK(result(0, normal_publish(a)) == 0);
	reclaim_all(0);
	a.op = MD_LOG_FREE;
	CHECK(result(1, remote_publish(a)) == 0);
	reclaim_all(1);
	puts("PASS(v3): mm exit expires session without destroying Arena logs");

	/* Normal FIFO contains different mm identities, independent of prev_mm. */
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		session = register_session(BASE, log_area);
		a = record(201, 8);
		CHECK(result(0, normal_publish(a)) == 0);
		reclaim_all(0);
		memset((char *)BASE + 8 * page_size, 0x77, 2 * page_size);
		a.op = MD_LOG_FREE;
		CHECK(result(0, normal_publish(a)) == 0);
		reclaim_all(0);
		_exit(0);
	}
	CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	      WEXITSTATUS(status) == 0);
	a = record(202, 8);
	CHECK(result(0, normal_publish(a)) == 0);
	reclaim_all(0);
	memset((char *)BASE + 8 * page_size, 0x88, 2 * page_size);
	a.op = MD_LOG_FREE;
	CHECK(result(1, remote_publish(a)) == 0);
	reclaim_all(1);
	puts("PASS(v3): shared normal FIFO resolves actual mm; cross-process run reuse");

	/* Owner CPU can consume while only unregistered mm tasks run there. */
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		char cpu_arg[24];
		snprintf(cpu_arg, sizeof(cpu_arg), "%u", owner_cpu);
		execl("/proc/self/exe", "shared_log_v2_test", "--pump", cpu_arg,
		      NULL);
		_exit(98);
	}
	a = record(203, 8);
	CHECK(result(0, normal_publish(a)) == 0);
	reclaim_all(0);
	pin_cpu(other_cpu);
	a.op = MD_LOG_FREE;
	CHECK(result(1, remote_publish(a)) == 0);
	pin_cpu(owner_cpu);
	reclaim_all(1);
	CHECK(kill(pid, SIGTERM) == 0 && waitpid(pid, &status, 0) == pid);
	puts("PASS(v3): owner CPU consumes with unregistered prev/next mm");
	/* A fatal normal rejection stops BOTH publication and success head. */
	stale = record(204, 8);
	stale.session = UINT64_MAX;
	tail = log_area->normal.head;
	ticket = normal_publish(stale);
	for (unsigned int n = 0; n < 10000 &&
	     __atomic_load_n(&log_area->state, __ATOMIC_ACQUIRE) == MD_SHARED_LOG_RUNNING; n++)
		usleep(100);
	CHECK(log_area->state == MD_SHARED_LOG_STOPPED);
	CHECK(log_area->normal.head == tail);
	CHECK(log_area->normal_error.ready == ticket + 1);
	CHECK(log_area->normal_error.result == -ESTALE);
	CHECK(log_area->normal_error.record.session == UINT64_MAX);
	CHECK(md_v2_normal_publish(log_area, &stale, &ticket) == -ESHUTDOWN);
	CHECK(md_v2_remote_reserve(log_area, &ticket) == -ESHUTDOWN);
	puts("PASS(v3): normal first error retained; head never passes failed request");
	puts("PASS(v3): ALL shared Arena protocol tests");
	return 0;
}
