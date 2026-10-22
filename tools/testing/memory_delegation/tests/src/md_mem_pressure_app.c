// SPDX-License-Identifier: GPL-2.0
/*
 * Scudo allocation pressure helper for phone-side camera experiments.
 *
 * This intentionally uses malloc/free instead of direct mmap so the test can
 * exercise either baseline Scudo secondary cache or SharedArena.
 */
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "shared_arena.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
	(void)signo;
	g_stop = 1;
}

static long env_long(const char *name, long fallback)
{
	const char *value = getenv(name);
	char *end = NULL;
	long parsed;

	if (!value || !*value)
		return fallback;
	errno = 0;
	parsed = strtol(value, &end, 10);
	if (errno || !end || *end)
		return fallback;
	return parsed;
}

static void sleep_ms(long ms)
{
	struct timespec ts;

	if (ms <= 0)
		return;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) && errno == EINTR && !g_stop) {
	}
}

static uintptr_t arena_base_for_cpu(int cpu)
{
	return (uintptr_t)scudo::kSharedArenaBaseAddr +
	       (uintptr_t)cpu * scudo::kArenaCapacityPerCore;
}

static int is_in_any_arena(void *ptr)
{
	uintptr_t addr = (uintptr_t)ptr;
	unsigned cpus = sysconf(_SC_NPROCESSORS_CONF);

	if (cpus == 0 || cpus > scudo::kArenaMaxCores)
		cpus = scudo::kArenaMaxCores;

	for (unsigned cpu = 0; cpu < cpus; ++cpu) {
		uintptr_t base = arena_base_for_cpu((int)cpu);
		if (addr >= base && addr < base + scudo::kArenaCapacityPerCore)
			return 1;
	}
	return 0;
}

static void read_self_rollup_kb(uint64_t *pss_kb, uint64_t *rss_kb)
{
	FILE *fp;
	char line[256];
	unsigned long value;

	*pss_kb = 0;
	*rss_kb = 0;
	fp = fopen("/proc/self/smaps_rollup", "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "Pss: %lu kB", &value) == 1)
			*pss_kb = value;
		else if (sscanf(line, "Rss: %lu kB", &value) == 1)
			*rss_kb = value;
	}
	fclose(fp);
}

int main(int argc, char **argv)
{
	long mb = env_long("MD_PRESSURE_MB", 512);
	long seconds = env_long("MD_PRESSURE_SECONDS", 120);
	long touch_ms = env_long("MD_PRESSURE_TOUCH_MS", 1000);
	long stride_kb = env_long("MD_PRESSURE_STRIDE_KB", 4);
	long chunk_kb = env_long("MD_PRESSURE_CHUNK_KB", 1024);
	long free_after_touch = env_long("MD_PRESSURE_FREE_AFTER_TOUCH", 0);
	const char *label = getenv("MD_PRESSURE_LABEL");
	size_t len;
	size_t stride;
	size_t chunk;
	size_t chunks;
	unsigned char **ptrs;
	uint64_t checksum = 0;
	uint64_t self_pss_kb = 0;
	uint64_t self_rss_kb = 0;
	int arena_allocs = 0;
	time_t start;
	int i;

	for (i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--mb"))
			mb = strtol(argv[i + 1], NULL, 10);
		else if (!strcmp(argv[i], "--seconds"))
			seconds = strtol(argv[i + 1], NULL, 10);
		else if (!strcmp(argv[i], "--touch-ms"))
			touch_ms = strtol(argv[i + 1], NULL, 10);
		else if (!strcmp(argv[i], "--stride-kb"))
			stride_kb = strtol(argv[i + 1], NULL, 10);
		else if (!strcmp(argv[i], "--chunk-kb"))
			chunk_kb = strtol(argv[i + 1], NULL, 10);
	}

	if (mb <= 0 || seconds < 0 || touch_ms <= 0 || stride_kb <= 0 ||
	    chunk_kb <= 0) {
		fprintf(stderr,
			"usage: %s [--mb N] [--seconds N] [--touch-ms N] [--stride-kb N] [--chunk-kb N]\n",
			argv[0]);
		return 2;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	len = (size_t)mb * 1024u * 1024u;
	stride = (size_t)stride_kb * 1024u;
	chunk = (size_t)chunk_kb * 1024u;
	chunks = (len + chunk - 1) / chunk;
	ptrs = (unsigned char **)calloc(chunks, sizeof(*ptrs));
	if (!ptrs) {
		fprintf(stderr, "MD_PRESSURE_READY label=%s status=calloc_failed mb=%ld errno=%d\n",
			label ? label : "", mb, errno);
		return 1;
	}

	for (size_t idx = 0; idx < chunks; ++idx) {
		size_t this_len = chunk;

		if ((idx + 1) * chunk > len)
			this_len = len - idx * chunk;
		ptrs[idx] = (unsigned char *)malloc(this_len);
		if (!ptrs[idx]) {
			fprintf(stderr,
				"MD_PRESSURE_READY label=%s status=malloc_failed mb=%ld chunk=%zu errno=%d\n",
				label ? label : "", mb, idx, errno);
			return 1;
		}
		if (is_in_any_arena(ptrs[idx]))
			arena_allocs++;
		for (size_t off = 0; off < this_len; off += stride) {
			ptrs[idx][off] = (unsigned char)((idx * chunk + off) >> 12);
			checksum += ptrs[idx][off];
		}
	}

	if (free_after_touch) {
		for (size_t idx = 0; idx < chunks; ++idx) {
			free(ptrs[idx]);
			ptrs[idx] = NULL;
		}
	}
	read_self_rollup_kb(&self_pss_kb, &self_rss_kb);

	fprintf(stderr,
		"MD_PRESSURE_READY label=%s pid=%ld mb=%ld seconds=%ld chunk_kb=%ld chunks=%zu stride_kb=%ld arena_allocs=%d free_after_touch=%ld checksum=%" PRIu64 "\n",
		label ? label : "", (long)getpid(), mb, seconds, chunk_kb, chunks,
		stride_kb, arena_allocs, free_after_touch, checksum);
	fprintf(stderr,
		"MD_PRESSURE_MEM label=%s pid=%ld pss_kb=%" PRIu64 " rss_kb=%" PRIu64 "\n",
		label ? label : "", (long)getpid(), self_pss_kb, self_rss_kb);
	fflush(stderr);

	start = time(NULL);
	while (!g_stop && (seconds == 0 || time(NULL) - start < seconds)) {
		if (!free_after_touch) {
			for (size_t idx = 0; idx < chunks && !g_stop; ++idx) {
				size_t this_len = chunk;

				if ((idx + 1) * chunk > len)
					this_len = len - idx * chunk;
				for (size_t off = 0; off < this_len && !g_stop; off += stride) {
					ptrs[idx][off]++;
					checksum += ptrs[idx][off];
				}
			}
		}
		sleep_ms(touch_ms);
	}

	fprintf(stderr, "MD_PRESSURE_EXIT label=%s pid=%ld checksum=%" PRIu64 "\n",
		label ? label : "", (long)getpid(), checksum);
	for (size_t idx = 0; idx < chunks; ++idx)
		free(ptrs[idx]);
	free(ptrs);
	return 0;
}
