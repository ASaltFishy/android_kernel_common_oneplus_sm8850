// SPDX-License-Identifier: GPL-2.0
/*
 * Standalone shared-ring v3 final-state PTE and VMA-fault regression.
 * Run in a fresh QEMU guest, with debugfs switch statistics enabled and no
 * existing delegation broker/arena. An arena cannot change its pinned backing
 * during one boot. Argument 1 asserts PTE retention; 0 tests the old baseline.
 * Build: aarch64-linux-gnu-gcc -O2 -static -pthread final_state_sync_test.c -o final-state-test
 */
#define _GNU_SOURCE
#include "shared_log_v2.h"
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
	do {                                                                                       \
		if (!(x)) {                                                                        \
			fprintf(stderr, "FAIL(final): line=%d errno=%d %s\n", __LINE__, errno,     \
				#x);                                                               \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)
#define PAGES 8192U
#define CAP 1024U
struct control {
	uint64_t next_id;
};
struct message {
	uint32_t op, arg;
};
static struct control *ctl;
static struct md_shared_log *logs;
static unsigned char *base;
static size_t pg, log_bytes;
static uint64_t session;
static sigjmp_buf jb;
static volatile sig_atomic_t expecting;
static void handler(int sig)
{
	if (expecting) {
		expecting = 0;
		siglongjmp(jb, 1);
	}
	_exit(90 + sig);
}
static void pin(void)
{
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(0, &s);
	CHECK(!sched_setaffinity(0, sizeof(s), &s));
}
static void reg(void)
{
	struct md_shared_registration r = {.size = sizeof(r),
					   .version = 3,
					   .log_address = (uintptr_t)logs,
					   .log_bytes = log_bytes};
	CHECK(!prctl(74, &r, (1UL << 2) | (1UL << 4), 0UL, base));
	session = r.session;
}
static uint64_t post(unsigned op, unsigned start, unsigned n, uint64_t id)
{
	struct md_shared_record r = {.session = session,
				     .block_id = start + 1,
				     .generation = id,
				     .allocation_id = id,
				     .start_page = start,
				     .nr_pages = n,
				     .cpu = 0,
				     .op = op};
	uint64_t ticket;
	CHECK(!md_v2_normal_publish(logs, &r, &ticket));
	return ticket;
}
static uint64_t alloc(unsigned start, unsigned n)
{
	uint64_t id = __atomic_add_fetch(&ctl->next_id, 1, __ATOMIC_RELAXED);
	post(MD_LOG_ALLOC, start, n, id);
	return id;
}
static void flush(void)
{
	uint64_t target = __atomic_load_n(&logs->normal.reserved, __ATOMIC_ACQUIRE);
	for (unsigned n = 0; __atomic_load_n(&logs->normal.head, __ATOMIC_ACQUIRE) < target; n++) {
		CHECK(n < 10000);
		CHECK(logs->state == MD_SHARED_LOG_RUNNING);
		sched_yield();
		usleep(10);
	}
	usleep(100);
	CHECK(logs->state == MD_SHARED_LOG_RUNNING);
}
static void sendm(int fd, struct message m)
{
	CHECK(write(fd, &m, sizeof(m)) == sizeof(m));
}
static struct message recvm(int fd)
{
	struct message m;
	CHECK(read(fd, &m, sizeof(m)) == sizeof(m));
	return m;
}
static void command(int wr, int rd, unsigned op, unsigned arg)
{
	sendm(wr, (struct message){op, arg});
	struct message m = recvm(rd);
	CHECK(m.op == op);
}
static unsigned char readp(unsigned p)
{
	return *(volatile unsigned char *)(base + p * pg);
}
static void writep(unsigned p, unsigned char v)
{
	*(volatile unsigned char *)(base + p * pg) = v;
}
static atomic_int stop_readers;
static void *read_worker(void *arg)
{
	cpu_set_t cpus;
	CPU_ZERO(&cpus);
	CPU_SET((uintptr_t)arg, &cpus);
	CHECK(!sched_setaffinity(0, sizeof(cpus), &cpus));
	while (!atomic_load_explicit(&stop_readers, memory_order_relaxed))
		CHECK(readp(65) == 0x44);
	return NULL;
}
static void denied(unsigned p)
{
	expecting = 1;
	if (sigsetjmp(jb, 1) == 0) {
		(void)readp(p);
		expecting = 0;
		CHECK(!"unauthorized PTE survived");
	}
	CHECK(!expecting);
}
static long minflt(void)
{
	struct rusage r;
	CHECK(!getrusage(RUSAGE_SELF, &r));
	return r.ru_minflt;
}
static uint64_t lost_fallbacks(void)
{
	FILE *f = fopen("/sys/kernel/debug/memory_delegation/switch_cycle_stats", "r");
	char line[4096];
	uint64_t result = 0;
	int found = 0;
	CHECK(f);
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "pte_detail_total ", 17)) {
			char *save = NULL, *word = strtok_r(line, " ", &save);
			for (unsigned i = 0; i < 11; i++) {
				word = strtok_r(NULL, " ", &save);
				CHECK(word);
			}
			result = strtoull(word, NULL, 10);
			found = 1;
			break;
		}
	}
	fclose(f);
	CHECK(found);
	return result;
}
static void child(int rd, int wr)
{
	pin();
	reg();
	uint64_t left = 0, right = 0, large = 0;
	sendm(wr, (struct message){0, 0});
	for (;;) {
		struct message m = recvm(rd);
		switch (m.op) {
		case 1:
			for (unsigned i = 0; i < m.arg; i++) {
				uint64_t id = alloc(0, 32);
				post(MD_LOG_FREE, 0, 32, id);
				if ((i % 64) == 63)
					flush();
			}
			flush();
			break;
		case 2:
			left = alloc(64, 1);
			right = alloc(66, 1);
			flush();
			writep(64, 0x51);
			writep(66, 0x52);
			break;
		case 3:
			post(MD_LOG_FREE, 64, 1, left);
			post(MD_LOG_FREE, 66, 1, right);
			flush();
			break;
		case 4:
			large = alloc(512, 512);
			flush();
			writep(512, 0x61);
			writep(1023, 0x62);
			break;
		case 5:
			post(MD_LOG_FREE, 512, 512, large);
			flush();
			break;
		case 6:
			for (unsigned i = 0; i < 260; i++)
				alloc(2048 + 2 * i, 1);
			flush();
			break;
		case 7:
			for (unsigned i = 0; i < 4600; i++) {
				uint64_t id = alloc(128, 1);
				post(MD_LOG_FREE, 128, 1, id);
				if ((i % 64) == 63)
					flush();
			}
			flush();
			left = alloc(64, 1);
			right = alloc(66, 1);
			flush();
			break;
		case 8:
			large = alloc(4096, 256);
			flush();
			writep(4096, 0x7b);
			break;
		case 9:
			_exit(0);
		default:
			CHECK(!"command");
		}
		sendm(wr, m);
	}
}
int main(int argc, char **argv)
{
	setbuf(stdout, NULL);
	alarm(90);
	pin();
	int expect_final = argc > 1 ? atoi(argv[1]) : 0;
	pg = sysconf(_SC_PAGESIZE);
	log_bytes = ((128 + 2 * CAP * 64 + pg - 1) / pg) * pg;
	struct sigaction sa = {};
	sa.sa_handler = handler;
	CHECK(!sigaction(SIGSEGV, &sa, NULL));
	int fd = memfd_create("final-payload", 0), lf = memfd_create("final-log", 0);
	CHECK(fd >= 0 && lf >= 0);
	CHECK(!ftruncate(fd, PAGES * pg) && !ftruncate(lf, log_bytes));
	base = mmap((void *)0x3000000000UL, PAGES * pg, 3, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
	CHECK(base == (void *)0x3000000000UL);
	logs = mmap(NULL, log_bytes, 3, MAP_SHARED, lf, 0);
	CHECK(logs != MAP_FAILED);
	logs->magic = MD_SHARED_LOG_MAGIC;
	logs->version = 3;
	logs->capacity = CAP;
	logs->arena_nr_pages = PAGES;
	logs->log_bytes = log_bytes;
	ctl = mmap(NULL, pg, 3, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(ctl != MAP_FAILED);
	if (argc > 2 && atoi(argv[2])) {
		/* Build a real large cached folio through an unwrapped alias, then
		 * exercise page-granular read mapping from a NOHUGEPAGE arena.
		 */
		unsigned char *alias = mmap((void *)0x5000000000UL, 2UL << 20,
			3, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
		CHECK(alias == (void *)0x5000000000UL);
		CHECK(!madvise(alias, 2UL << 20, MADV_HUGEPAGE));
		*(volatile unsigned char *)alias = 0;
		FILE *f = fopen("/proc/meminfo", "r");
		char line[256];
		unsigned long huge_kb = 0;
		CHECK(f);
		while (fgets(line, sizeof(line), f))
			if (sscanf(line, "ShmemHugePages: %lu kB", &huge_kb) == 1)
				break;
		fclose(f);
		CHECK(huge_kb >= 2048);
		CHECK(!munmap(alias, 2UL << 20));
		CHECK(!madvise(base, PAGES * pg, MADV_NOHUGEPAGE));
		printf("LARGE_CACHED_FOLIO verified_shmem_huge_kb=%lu\n", huge_kb);
	}
	reg();
	int a[2], b[2];
	CHECK(!pipe(a) && !pipe(b));
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid)
		child(a[0], b[1]);
	CHECK(recvm(b[0]).op == 0);
	for (unsigned n = 1; n <= 64; n *= 8) {
		for (unsigned p = 0; p < 32; p++)
			writep(p, 0x31);
		command(a[1], b[0], 1, n);
		long before = minflt();
		for (unsigned p = 0; p < 32; p++)
			CHECK(readp(p) == 0x31);
		long faults = minflt() - before;
		printf("RETENTION repeats=%u faults=%ld expect_final=%d\n", n, faults,
		       expect_final);
		if (expect_final)
			CHECK(faults == 0);
	}
	puts("PASS(final): repeated FREE access; requested retention assertion passed");
	uint64_t middle = alloc(65, 1);
	flush();
	writep(65, 0x44);
	writep(64, 1);
	writep(66, 2);
	command(a[1], b[0], 2, 0);
	denied(64);
	denied(66);
	long before = minflt();
	CHECK(readp(65) == 0x44);
	CHECK(minflt() == before);
	puts("PASS(final): mixed chunk revokes only foreign ranges, hole PTE retained");
	/* Force a read fault beside two resident pages belonging to the child.
	 * A generic filemap fault-around must never populate either neighbour.
	 * mprotect splits the wrapped VMA and exercises its shared lifetime.
	 */
	CHECK(!mprotect(base + 64 * pg, 3 * pg, PROT_READ));
	CHECK(!madvise(base + 63 * pg, 5 * pg, MADV_DONTNEED));
	CHECK(readp(63) == 0);
	CHECK(readp(65) == 0x44);
	denied(64);
	denied(66);
	CHECK(!mprotect(base + 64 * pg, 3 * pg, PROT_READ | PROT_WRITE));
	writep(65, 0x44);
	puts("PASS(final): read fault neighbours denied after VMA split and PTE discard");
	pthread_t readers[2];
	for (uintptr_t i = 0; i < 2; i++)
		CHECK(!pthread_create(&readers[i], NULL, read_worker, (void *)(i + 1)));
	for (unsigned i = 0; i < 100; i++) {
		CHECK(!mprotect(base + 64 * pg, 3 * pg, PROT_READ));
		CHECK(!madvise(base + 65 * pg, pg, MADV_DONTNEED));
		command(a[1], b[0], 3, 0);
		command(a[1], b[0], 2, 0);
		CHECK(!mprotect(base + 64 * pg, 3 * pg, PROT_READ | PROT_WRITE));
	}
	atomic_store_explicit(&stop_readers, 1, memory_order_relaxed);
	for (unsigned i = 0; i < 2; i++)
		CHECK(!pthread_join(readers[i], NULL));
	denied(64);
	denied(66);
	puts("PASS(final): concurrent read faults, VMA split and adjacent ownership handoffs");
	command(a[1], b[0], 3, 0);
	CHECK(readp(64) == 0x51 && readp(66) == 0x52);
	uint64_t mine = alloc(512, 512);
	flush();
	writep(512, 0x11);
	writep(1023, 0x12);
	post(MD_LOG_FREE, 512, 512, mine);
	flush();
	command(a[1], b[0], 4, 0);
	denied(512);
	denied(1023);
	command(a[1], b[0], 5, 0);
	mine = alloc(512, 512);
	flush();
	writep(512, 0x33);
	CHECK(readp(512) == 0x33);
	post(MD_LOG_FREE, 512, 512, mine);
	flush();
	puts("PASS(final): A-B-A transfer and slot generation reuse");
	for (unsigned i = 0; i < 260; i++) {
		writep(2048 + 2 * i, 0x21);
		alloc(2049 + 2 * i, 1);
		writep(2049 + 2 * i, 0x48);
	}
	flush();
	command(a[1], b[0], 6, 0);
	denied(2048);
	denied(2566);
	before = minflt();
	for (unsigned i = 0; i < 260; i++)
		CHECK(readp(2049 + 2 * i) == 0x48);
	CHECK(minflt() == before);
	puts("PASS(final): fragmented fallback preserves intervening live PTEs");
	writep(64, 1);
	writep(66, 2);
	uint64_t lost_before = lost_fallbacks();
	command(a[1], b[0], 7, 0);
	CHECK(lost_fallbacks() > lost_before);
	denied(64);
	denied(66);
	before = minflt();
	CHECK(readp(65) == 0x44);
	CHECK(minflt() == before);
	puts("PASS(final): overwritten delta fallback and mixed hole");
	writep(4096, 1);
	command(a[1], b[0], 8, 0);
	denied(4096);
	sendm(a[1], (struct message){9, 0});
	int status;
	CHECK(waitpid(pid, &status, 0) == pid && status == 0);
	usleep(1000);
	denied(4096);
	puts("PASS(final): exited owner remains inaccessible");
	post(MD_LOG_FREE, 65, 1, middle);
	flush();
	puts("FINAL_STATE_ALL_PASS");
	return 0;
}
