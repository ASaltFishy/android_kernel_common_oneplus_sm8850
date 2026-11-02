// SPDX-License-Identifier: GPL-2.0
/* Run in a fresh 4-CPU guest. CPU 1 arms a timer waking CPU 0 while
 * CPU 1/2 repeatedly register and exit; CPU 0 continuously schedules an
 * active delegated mm. This exercises rq -> ctx-table versus exit + IRQ. */
#define _GNU_SOURCE
#include "shared_log_v2.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                    \
	do {                        \
		if (!(x)) {         \
			perror(#x); \
			exit(1);    \
		}                   \
	} while (0)
#define CPUS 4
#define CAP 32
static struct md_shared_log *logs[CPUS];
static void *payload[CPUS];
static size_t log_bytes;
static atomic_int stop;
static atomic_ulong wakes;
static int tfd;
static void pin(unsigned cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	CHECK(!sched_setaffinity(0, sizeof(set), &set));
}
static void reg_all(void)
{
	for (unsigned cpu = 0; cpu < CPUS; cpu++) {
		struct md_shared_registration r = {
			.size = sizeof(r),
			.version = 3,
			.log_address = (uintptr_t)logs[cpu],
			.log_bytes = log_bytes
		};
		CHECK(!prctl(74, &r, (1UL << 2) | (1UL << 4),
			     (unsigned long)cpu, payload[cpu]));
		CHECK(r.session);
	}
}
static void *reader(void *unused)
{
	(void)unused;
	pin(0);
	while (!atomic_load(&stop)) {
		uint64_t n;
		ssize_t r = read(tfd, &n, sizeof(n));
		if (r < 0 && errno == EINTR)
			continue;
		CHECK(r == sizeof(n));
		atomic_fetch_add(&wakes, 1);
		sched_yield();
	}
	return NULL;
}
static void exits(unsigned cpu, unsigned count)
{
	pin(cpu);
	for (unsigned n = 0; n < count; n++) {
		pid_t p = fork();
		CHECK(p >= 0);
		if (!p) {
			reg_all();
			_exit(0);
		}
		int status;
		CHECK(waitpid(p, &status, 0) == p);
		CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
	}
	_exit(0);
}
int main(int argc, char **argv)
{
	unsigned count = argc > 1 ? strtoul(argv[1], NULL, 10) : 200;
	unsigned mb = argc > 2 ? strtoul(argv[2], NULL, 10) : 2048;
	size_t pg = sysconf(_SC_PAGESIZE), bytes = (size_t)mb << 20;
	setbuf(stdout, NULL);
	alarm(110);
	pin(1);
	log_bytes = (sizeof(struct md_shared_log) +
		     2 * CAP * sizeof(struct md_shared_slot) + pg - 1) &
		    ~(pg - 1);
	for (unsigned cpu = 0; cpu < CPUS; cpu++) {
		int p = memfd_create("irq-exit-payload", 0),
		    l = memfd_create("irq-exit-log", 0);
		CHECK(p >= 0 && l >= 0);
		CHECK(!ftruncate(p, bytes) && !ftruncate(l, log_bytes));
		payload[cpu] =
			mmap((void *)(0x3000000000UL + cpu * (bytes + pg)),
			     bytes, 3, MAP_SHARED | MAP_FIXED_NOREPLACE, p, 0);
		CHECK(payload[cpu] != MAP_FAILED);
		logs[cpu] = mmap(NULL, log_bytes, 3, MAP_SHARED, l, 0);
		CHECK(logs[cpu] != MAP_FAILED);
		close(p);
		close(l);
		logs[cpu]->magic = MD_SHARED_LOG_MAGIC;
		logs[cpu]->version = 3;
		logs[cpu]->capacity = CAP;
		logs[cpu]->arena_nr_pages = bytes / pg;
		logs[cpu]->log_bytes = log_bytes;
	}
	reg_all();
	tfd = timerfd_create(CLOCK_MONOTONIC, 0);
	CHECK(tfd >= 0);
	struct itimerspec timer = { .it_interval = { .tv_nsec = 50000 },
				    .it_value = { .tv_nsec = 50000 } };
	CHECK(!timerfd_settime(tfd, 0, &timer, NULL));
	/* Fork before creating reader; avoid libc fork synchronization in children. */
	pid_t children[2];
	for (unsigned i = 0; i < 2; i++) {
		children[i] = fork();
		CHECK(children[i] >= 0);
		if (!children[i])
			exits(i + 1, count);
	}
	pthread_t thread;
	CHECK(!pthread_create(&thread, NULL, reader, NULL));
	pin(0);
	for (unsigned i = 0; i < 2; i++) {
		int s;
		CHECK(waitpid(children[i], &s, 0) == children[i]);
		CHECK(WIFEXITED(s) && !WEXITSTATUS(s));
	}
	atomic_store(&stop, 1);
	CHECK(!pthread_join(thread, NULL));
	CHECK(atomic_load(&wakes) > 0);
	printf("PASS(ctx_irq_exit): exits=%u arenas=%u arena_mb=%u timer_wakes=%lu\n",
	       2 * count + 2, CPUS, mb, atomic_load(&wakes));
	return 0;
}
