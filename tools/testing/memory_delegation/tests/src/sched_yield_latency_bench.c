#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t read_cntvct(void)
{
#if defined(__aarch64__)
	uint64_t v;

	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static void die(const char *msg)
{
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

static void pin_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0)
		die("sched_setaffinity");
}

static int env_int(const char *name, int fallback)
{
	const char *s = getenv(name);
	char *end = NULL;
	long v;

	if (!s || !*s)
		return fallback;
	errno = 0;
	v = strtol(s, &end, 0);
	if (errno || !end || *end)
		return fallback;
	return (int)v;
}

static int cmp_u64(const void *a, const void *b)
{
	const uint64_t va = *(const uint64_t *)a;
	const uint64_t vb = *(const uint64_t *)b;

	return (va > vb) - (va < vb);
}

struct switch_state {
	volatile uint32_t turn;
	volatile uint32_t stop;
	volatile uint32_t record;
	volatile uint32_t miss;
	volatile uint32_t index;
	volatile uint64_t start;
	volatile uint64_t rejected;
	uint64_t samples[];
};

static uint32_t load_u32(volatile uint32_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_u32(volatile uint32_t *p, uint32_t v)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static bool cmpxchg_u32(volatile uint32_t *p, uint32_t *expected,
			uint32_t desired)
{
	return __atomic_compare_exchange_n(p, expected, desired, false,
					   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static uint64_t load_u64(volatile uint64_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_u64(volatile uint64_t *p, uint64_t v)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void child_loop(int cpu, int ready_fd, struct switch_state *state)
{
	char c = 'r';

	pin_cpu(cpu);
	if (write(ready_fd, &c, 1) != 1)
		_exit(2);

	for (;;) {
		uint32_t turn;

		while ((turn = load_u32(&state->turn)) == 0) {
			if (load_u32(&state->stop))
				_exit(0);
			sched_yield();
		}

	if (turn == 1) {
			uint32_t expected = 1;
			uint64_t delta;

			if (!cmpxchg_u32(&state->turn, &expected, 3))
				continue;
			delta = read_cntvct() - load_u64(&state->start);
			if (load_u32(&state->record)) {
				uint32_t index = load_u32(&state->index);

				state->samples[index] = delta;
			}
			store_u32(&state->turn, 0);
			sched_yield();
			continue;
		}

		if (turn == 2) {
			store_u32(&state->turn, 0);
			sched_yield();
			continue;
		}

		if (load_u32(&state->record)) {
			uint32_t index = load_u32(&state->index);
			state->samples[index] = read_cntvct() -
				load_u64(&state->start);
		}
		store_u32(&state->turn, 0);
		sched_yield();
	}
}

int main(void)
{
	const int cpu = env_int("YIELD_BENCH_CPU", 0);
	const int warmup = env_int("YIELD_BENCH_WARMUP", 1000);
	const int iterations = env_int("YIELD_BENCH_ITERS", 20000);
	const int strict_next = env_int("YIELD_BENCH_STRICT_NEXT", 1);
	uint64_t *samples;
	struct switch_state *state;
	pid_t pid;
	int pipefd[2];
	size_t state_size;
	uint64_t sum = 0;
	uint64_t min = UINT64_MAX;
	uint64_t max = 0;

	if (iterations < 1)
		return 2;

	state_size = sizeof(*state) + (size_t)iterations * sizeof(uint64_t);
	state = mmap(NULL, state_size, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (state == MAP_FAILED)
		die("mmap");
	memset(state, 0, state_size);
	samples = state->samples;

	pin_cpu(cpu);
	if (pipe(pipefd) != 0)
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		close(pipefd[0]);
		child_loop(cpu, pipefd[1], state);
	}
	close(pipefd[1]);

	{
		char c;

		if (read(pipefd[0], &c, 1) != 1)
			die("read ready");
		close(pipefd[0]);
	}

	for (int i = 0; i < warmup; i++) {
		while (load_u32(&state->turn) != 0)
			sched_yield();
		store_u32(&state->record, 0);
		store_u32(&state->miss, 0);
		store_u64(&state->start, read_cntvct());
		store_u32(&state->turn, 1);
		sched_yield();
	}

	for (int i = 0; i < iterations; i++) {
		for (;;) {
			while (load_u32(&state->turn) != 0)
				sched_yield();
			store_u32(&state->index, (uint32_t)i);
			store_u32(&state->record, 1);
			store_u32(&state->miss, 0);
			store_u64(&state->start, read_cntvct());
			store_u32(&state->turn, 1);
			sched_yield();
			if (!strict_next || load_u32(&state->turn) == 0)
				break;
			{
				uint32_t expected = 1;

				if (!cmpxchg_u32(&state->turn, &expected, 2))
					break;
			}
			__atomic_add_fetch(&state->rejected, 1,
					   __ATOMIC_RELAXED);
		}
	}

	while (load_u32(&state->turn) != 0)
		sched_yield();
	store_u32(&state->stop, 1);
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);

	for (int i = 0; i < iterations; i++) {
		uint64_t delta = samples[i];

		sum += delta;
		if (delta < min)
			min = delta;
		if (delta > max)
			max = delta;
	}

	qsort(samples, (size_t)iterations, sizeof(*samples), cmp_u64);
	printf("YIELD_SWITCH unit=%s cpu=%d warmup=%d iterations=%d direction=A_to_B\n",
#if defined(__aarch64__)
	       "cntvct_tick",
#else
	       "ns",
#endif
	       cpu, warmup, iterations);
	printf("YIELD_SWITCH avg=%" PRIu64 " min=%" PRIu64 " p50=%" PRIu64
	       " p90=%" PRIu64 " p99=%" PRIu64 " max=%" PRIu64 "\n",
	       sum / (uint64_t)iterations, min, samples[iterations / 2],
	       samples[(iterations * 90) / 100], samples[(iterations * 99) / 100],
	       max);
	printf("YIELD_SWITCH_STRICT strict_next=%d rejected=%" PRIu64 "\n",
	       strict_next, load_u64(&state->rejected));
	return 0;
}
