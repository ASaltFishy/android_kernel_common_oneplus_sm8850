// SPDX-License-Identifier: GPL-2.0
/*
 * Scudo SharedArena <-> kernel memory delegation integration test (QEMU-friendly).
 *
 * Requirements (by design):
 * - Test code must be "normal userspace C": only uses malloc/free + fork/pipe/signal.
 * - Scudo is linked into this binary (standalone allocator + C wrappers), so
 *   malloc/free are provided by Scudo and can hit SharedArena.
 * - Enable forced SharedArena path via SCUDO_SHARED_ARENA_FORCE=1.
 *
 * Test shape:
 * - Warm-up: single process alloc/free loop to populate SharedArena free list.
 * - Multi-process: several processes pinned to the same CPU. For each round:
 *   - One owner allocates and touches.
 *   - Other processes try to touch the same VA and must SIGSEGV (revoked access).
 *   - Owner frees; next owner allocates again (ideally reusing addresses).
 *
 * The kernel side commits ownership on context switch, so we explicitly yield to
 * force frequent switches on the same CPU.
 */

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "shared_arena.h"

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "FAIL(scudo): ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  _exit(1);
}

static void alarm_handler(int signo) {
  (void)signo;
  die("timeout");
}

static void install_alarm(int seconds) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = alarm_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGALRM, &sa, NULL) != 0)
    die("sigaction(SIGALRM) failed errno=%d", errno);
  alarm((unsigned)seconds);
}

static long env_long(const char *name, long fallback) {
  const char *value = getenv(name);
  if (!value || value[0] == '\0')
    return fallback;

  char *end = nullptr;
  long parsed = strtol(value, &end, 10);
  if (!end || *end != '\0')
    die("invalid %s=%s", name, value);

  return parsed;
}

enum YieldTag {
  YIELD_GENERIC = 0,
  YIELD_WARMUP,
  YIELD_AFTER_ALLOC_LOG,
  YIELD_AFTER_TOUCH,
  YIELD_AFTER_FREE_LOG,
  YIELD_BEFORE_SEGV_TOUCH,
  YIELD_PARENT_AFTER_FREE,
  YIELD_MIGRATE_FREE,
  YIELD_EMPTY_SWITCH,
  YIELD_TAG_COUNT,
};

struct PendingYieldState {
  volatile uint32_t turn;
  volatile uint32_t miss;
  volatile uint32_t index;
  volatile uint64_t start;
  volatile uint64_t rejected_alloc;
  volatile uint64_t rejected_free;
};

struct YieldTagStats {
  uint64_t Count;
  uint64_t Sum;
  uint64_t Min;
  uint64_t Max;
  uint64_t FirstCount;
  uint64_t FirstSum;
  uint64_t FirstMin;
  uint64_t FirstMax;
};

static bool g_yield_stats_enabled = false;
static YieldTagStats g_yield_stats[YIELD_TAG_COUNT];

static const char *yield_tag_name(YieldTag tag) {
  switch (tag) {
  case YIELD_GENERIC:
    return "generic";
  case YIELD_WARMUP:
    return "warmup";
  case YIELD_AFTER_ALLOC_LOG:
    return "after_alloc_log";
  case YIELD_AFTER_TOUCH:
    return "after_touch";
  case YIELD_AFTER_FREE_LOG:
    return "after_free_log";
  case YIELD_BEFORE_SEGV_TOUCH:
    return "before_segv_touch";
  case YIELD_PARENT_AFTER_FREE:
    return "parent_after_free";
  case YIELD_MIGRATE_FREE:
    return "migrate_free";
  case YIELD_EMPTY_SWITCH:
    return "empty_switch";
  default:
    return "unknown";
  }
}

static void reset_yield_stats(void) {
  memset(g_yield_stats, 0, sizeof(g_yield_stats));
}

static void write_text_file_best_effort(const char *path, const char *text) {
  FILE *fp = fopen(path, "w");
  if (!fp)
    return;
  fputs(text, fp);
  fclose(fp);
}

static void reset_kernel_switch_stats_if_requested(void) {
  if (env_long("SCUDO_SHARED_ARENA_TEST_RESET_KERNEL_STATS_BEFORE_START", 0) == 0)
    return;

  write_text_file_best_effort(
      "/sys/kernel/debug/memory_delegation/switch_cycle_stats", "reset\n");
  write_text_file_best_effort(
      "/sys/kernel/debug/memory_delegation/switch_cycle_stats_enabled", "1\n");
}

static inline uint64_t read_test_cycles(void) {
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

static int cmp_u64_sample(const void *a, const void *b) {
  const uint64_t va = *(const uint64_t *)a;
  const uint64_t vb = *(const uint64_t *)b;
  return (va > vb) - (va < vb);
}

static void print_sample_stats(const char *prefix, const char *name,
                               uint64_t *samples, int count) {
  uint64_t sum = 0;
  uint64_t min = UINT64_MAX;
  uint64_t max = 0;

  if (count <= 0)
    return;

  for (int i = 0; i < count; i++) {
    const uint64_t v = samples[i];
    sum += v;
    if (v < min)
      min = v;
    if (v > max)
      max = v;
  }
  qsort(samples, (size_t)count, sizeof(*samples), cmp_u64_sample);
  fprintf(stderr,
          "%s name=%s count=%d avg=%" PRIu64 " min=%" PRIu64
          " p50=%" PRIu64 " p90=%" PRIu64 " p99=%" PRIu64
          " max=%" PRIu64 "\n",
          prefix, name, count, sum / (uint64_t)count, min,
          samples[count / 2], samples[(count * 90) / 100],
          samples[(count * 99) / 100], max);
}

static void record_yield_sample(YieldTag tag, uint64_t delta, bool first) {
  YieldTagStats *s = &g_yield_stats[tag];

  s->Count++;
  s->Sum += delta;
  if (s->Count == 1 || delta < s->Min)
    s->Min = delta;
  if (delta > s->Max)
    s->Max = delta;

  if (first) {
    s->FirstCount++;
    s->FirstSum += delta;
    if (s->FirstCount == 1 || delta < s->FirstMin)
      s->FirstMin = delta;
    if (delta > s->FirstMax)
      s->FirstMax = delta;
  }
}

static void print_yield_stats(const char *role, int child_idx) {
  if (!g_yield_stats_enabled)
    return;

  fprintf(stderr,
          "YIELD_STATS_BEGIN role=%s child=%d pid=%d unit=%s\n",
          role, child_idx, getpid(),
#if defined(__aarch64__)
          "cntvct_cycles"
#else
          "ns"
#endif
  );
  for (int i = 0; i < YIELD_TAG_COUNT; i++) {
    const YieldTagStats *s = &g_yield_stats[i];
    if (!s->Count)
      continue;
    const uint64_t Avg = s->Sum / s->Count;
    const uint64_t FirstAvg = s->FirstCount ? s->FirstSum / s->FirstCount : 0;
    fprintf(stderr,
            "YIELD_STATS tag=%s count=%" PRIu64 " avg=%" PRIu64
            " min=%" PRIu64 " max=%" PRIu64
            " first_count=%" PRIu64 " first_avg=%" PRIu64
            " first_min=%" PRIu64 " first_max=%" PRIu64 "\n",
            yield_tag_name((YieldTag)i), s->Count, Avg, s->Min, s->Max,
            s->FirstCount, FirstAvg, s->FirstMin, s->FirstMax);
  }
  fprintf(stderr, "YIELD_STATS_END role=%s child=%d pid=%d\n",
          role, child_idx, getpid());
}

static void yield_many_tag(int n, YieldTag tag) {
  for (int i = 0; i < n; i++) {
    uint64_t start = 0;
    if (g_yield_stats_enabled)
      start = read_test_cycles();
    sched_yield();
    if (g_yield_stats_enabled)
      record_yield_sample(tag, read_test_cycles() - start, i == 0);
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1 * 1000 * 1000; // 1ms
    nanosleep(&ts, NULL);
  }
}

static void yield_many(int n) {
  yield_many_tag(n, YIELD_GENERIC);
}

struct EmptyYieldSwitchState {
  volatile uint32_t turn;
  volatile uint32_t miss;
  volatile uint32_t index;
  volatile uint64_t start;
  volatile uint64_t rejected;
};

static uint32_t load_shared_u32(volatile uint32_t *p) {
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_shared_u32(volatile uint32_t *p, uint32_t v) {
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static bool cmpxchg_shared_u32(volatile uint32_t *p, uint32_t *expected,
                               uint32_t desired) {
  return __atomic_compare_exchange_n(p, expected, desired, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static uint64_t load_shared_u64(volatile uint64_t *p) {
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_shared_u64(volatile uint64_t *p, uint64_t v) {
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void add_shared_u64(volatile uint64_t *p, uint64_t v) {
  __atomic_add_fetch(p, v, __ATOMIC_RELAXED);
}

static void write_full(int fd, const void *buf, size_t n);
static void read_full(int fd, void *buf, size_t n);

static int g_test_cpu = 0;
static int g_free_cpu = -1;

static void init_test_cpu(void) {
  const char *cpu = getenv("SCUDO_SHARED_ARENA_TEST_CPU");
  const char *free_cpu = getenv("SCUDO_SHARED_ARENA_TEST_FREE_CPU");

  if (cpu && cpu[0] != '\0') {
    char *end = nullptr;
    long v = strtol(cpu, &end, 10);
    if (!end || *end != '\0' || v < 0 || v >= CPU_SETSIZE)
      die("invalid SCUDO_SHARED_ARENA_TEST_CPU=%s", cpu);

    g_test_cpu = (int)v;
  }

  if (free_cpu && free_cpu[0] != '\0') {
    char *end = nullptr;
    long v = strtol(free_cpu, &end, 10);
    if (!end || *end != '\0' || v < 0 || v >= CPU_SETSIZE)
      die("invalid SCUDO_SHARED_ARENA_TEST_FREE_CPU=%s", free_cpu);

    g_free_cpu = (int)v;
  }
}

static void pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0)
    die("sched_setaffinity(cpu%d) failed errno=%d", cpu, errno);
}

static void pin_to_test_cpu(void) {
  pin_to_cpu(g_test_cpu);
}

static uintptr_t arena_base(void) {
  return (uintptr_t)scudo::kSharedArenaBaseAddr;
}

static uintptr_t arena_cap_per_core(void) {
  return (uintptr_t)scudo::kArenaCapacityPerCore;
}

static bool is_in_test_arena(void *p) {
  const uintptr_t v = (uintptr_t)p;
  const uintptr_t base = arena_base() +
                         (uintptr_t)g_test_cpu * arena_cap_per_core();
  const uintptr_t cap = arena_cap_per_core();
  return v >= base && v < (base + cap);
}

static void touch_rw(void *p, size_t n);

static unsigned long meminfo_kb(const char *field) {
  FILE *fp = fopen("/proc/meminfo", "r");
  if (!fp)
    die("fopen(/proc/meminfo) failed errno=%d", errno);

  char key[64];
  unsigned long value = 0;
  char unit[32];
  while (fscanf(fp, "%63[^:]: %lu %31s\n", key, &value, unit) == 3) {
    if (strcmp(key, field) == 0) {
      fclose(fp);
      return value;
    }
  }
  fclose(fp);
  die("missing /proc/meminfo field %s", field);
  return 0;
}

static unsigned long memory_usage_percent(void) {
  const unsigned long total = meminfo_kb("MemTotal");
  const unsigned long available = meminfo_kb("MemAvailable");
  if (total == 0)
    die("MemTotal is zero");
  return (total - available) * 100 / total;
}

static void expect_alloc_path(const char *label, bool want_arena) {
  const size_t alloc_sz = 32U << 20;
  void *p = malloc(alloc_sz);
  if (!p)
    die("%s: malloc(%zu) failed", label, alloc_sz);
  const bool got_arena = is_in_test_arena(p);
  fprintf(stderr, "pressure_toggle: %s ptr=%p got_arena=%d want_arena=%d usage=%lu\n",
          label, p, got_arena ? 1 : 0, want_arena ? 1 : 0,
          memory_usage_percent());
  touch_rw(p, alloc_sz);
  free(p);
  if (got_arena != want_arena)
    die("%s: expected %s path but got %s path", label,
        want_arena ? "arena" : "fallback",
        got_arena ? "arena" : "fallback");
}

static void pressure_child_main(int cmd_fd, int ready_fd, size_t bytes) {
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  unsigned char *buf = (unsigned char *)malloc(bytes);
  if (!buf)
    die("pressure child malloc(%zu) failed", bytes);
  for (size_t off = 0; off < bytes; off += page)
    buf[off] = (unsigned char)(off >> 12);

  char ready = 'R';
  if (write(ready_fd, &ready, 1) != 1)
    die("pressure child ready write failed errno=%d", errno);

  char cmd = 0;
  (void)read(cmd_fd, &cmd, 1);
  free(buf);
  _exit(0);
}

static void set_env_u32(const char *name, unsigned long value) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%lu", value);
  if (setenv(name, buf, 1) != 0)
    die("setenv(%s) failed errno=%d", name, errno);
}

static void run_pressure_toggle_smoke(void) {
  pin_to_test_cpu();

  if (setenv("SCUDO_SHARED_ARENA_FORCE", "0", 1) != 0)
    die("setenv(SCUDO_SHARED_ARENA_FORCE) failed errno=%d", errno);
  set_env_u32("SCUDO_SHARED_ARENA_PRESSURE_CHECK_INTERVAL", 0);

  const unsigned long low_usage = memory_usage_percent();
  fprintf(stderr, "pressure_toggle: initial usage=%lu\n", low_usage);
  set_env_u32("SCUDO_SHARED_ARENA_PRESSURE_THRESHOLD", 101);
  expect_alloc_path("low-water", false);

  int cmd_pipe[2];
  int ready_pipe[2];
  if (pipe(cmd_pipe) != 0 || pipe(ready_pipe) != 0)
    die("pressure pipe failed errno=%d", errno);

  const unsigned long total_kb = meminfo_kb("MemTotal");
  size_t pressure_bytes =
      (size_t)env_long("SCUDO_SHARED_ARENA_TEST_PRESSURE_MB", 512) << 20;
  const size_t max_pressure_bytes = (size_t)(total_kb / 2) << 10;
  if (pressure_bytes > max_pressure_bytes)
    pressure_bytes = max_pressure_bytes;
  if (pressure_bytes < (64U << 20))
    pressure_bytes = 64U << 20;

  pid_t pid = fork();
  if (pid < 0)
    die("pressure fork failed errno=%d", errno);
  if (pid == 0) {
    close(cmd_pipe[1]);
    close(ready_pipe[0]);
    pressure_child_main(cmd_pipe[0], ready_pipe[1], pressure_bytes);
  }
  close(cmd_pipe[0]);
  close(ready_pipe[1]);

  char ready = 0;
  if (read(ready_pipe[0], &ready, 1) != 1)
    die("pressure child ready read failed errno=%d", errno);
  close(ready_pipe[0]);
  if (ready != 'R')
    die("pressure child sent unexpected ready byte");

  const unsigned long high_usage = memory_usage_percent();
  fprintf(stderr, "pressure_toggle: pressured usage=%lu bytes=%zu\n",
          high_usage, pressure_bytes);
  if (high_usage <= low_usage)
    die("pressure child did not raise memory usage: low=%lu high=%lu",
        low_usage, high_usage);
  set_env_u32("SCUDO_SHARED_ARENA_PRESSURE_THRESHOLD", high_usage);
  expect_alloc_path("high-water", true);

  char stop = 'S';
  if (write(cmd_pipe[1], &stop, 1) != 1)
    die("pressure child stop write failed errno=%d", errno);
  close(cmd_pipe[1]);
  int st = 0;
  if (waitpid(pid, &st, 0) < 0)
    die("pressure waitpid failed errno=%d", errno);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    die("pressure child failed status=%d", st);

  yield_many(40);
  const unsigned long final_usage = memory_usage_percent();
  fprintf(stderr, "pressure_toggle: final usage=%lu threshold=%lu\n",
          final_usage, high_usage);
  set_env_u32("SCUDO_SHARED_ARENA_PRESSURE_THRESHOLD", high_usage);
  expect_alloc_path("water-cleared", false);

  fprintf(stderr, "PASS(scudo): shared arena pressure toggle ok\n");
}

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_saw_segv = 0;
static volatile sig_atomic_t g_expect_segv_touch = 0;

static void on_segv(int signo, siginfo_t *info, void *ucontext) {
  (void)ucontext;
  (void)signo;
  const void *fault_addr = info ? info->si_addr : nullptr;
  if (!g_expect_segv_touch) {
    // SIGSEGV happened outside of expect_segv_on_touch()'s sigsetjmp scope.
    // Don't siglongjmp into an uninitialized/irrelevant jmp buffer.
    char buf[256];
    const int n = snprintf(buf, sizeof(buf),
                           "FAIL(scudo): unexpected SIGSEGV outside expect_segv_on_touch fault_addr=%p\n",
                           fault_addr);
    if (n > 0)
      (void)write(STDERR_FILENO, buf,
                   static_cast<size_t>(n) < sizeof(buf) ? (size_t)n
                                                     : sizeof(buf));
    _exit(1);
  }

  g_saw_segv = 1;
  siglongjmp(g_jmp, 1);
}

static void install_segv_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_segv;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NODEFER | SA_SIGINFO;
  if (sigaction(SIGSEGV, &sa, NULL) != 0)
    die("sigaction(SIGSEGV) failed errno=%d", errno);
}

static void touch_rw(void *p, size_t n) {
  volatile unsigned char *b = (volatile unsigned char *)p;
  if (n == 0)
    return;
  b[0] ^= 0x1;
  b[n - 1] ^= 0x2;
}

static void run_single_process_malloc_free_smoke(void) {
  pin_to_test_cpu();

  const size_t alloc_sz = 32U << 20; // 32MB, above Scudo primary classes.
  fprintf(stderr,
          "single_process: malloc/free smoke cpu=%d size=%zu\n",
          g_test_cpu, alloc_sz);

  void *p = malloc(alloc_sz);
  if (!p)
    die("single_process: malloc(%zu) failed", alloc_sz);
  if (!is_in_test_arena(p)) {
    const uintptr_t base = arena_base() +
                           (uintptr_t)g_test_cpu * arena_cap_per_core();
    const uintptr_t cap = arena_cap_per_core();
    die("single_process: malloc VA %p not in expected cpu%d shared arena range "
        "[0x%016" PRIxPTR ", 0x%016" PRIxPTR ")",
        p, g_test_cpu, base, base + cap);
  }

  // malloc() appends ownership to the ring; let the kernel consume it before
  // first touch, then fault in pages through arena_vma_ops.
  yield_many(80);
  touch_rw(p, alloc_sz);
  yield_many(20);
  free(p);
  yield_many(80);

  fprintf(stderr,
          "PASS(scudo): shared arena single-process malloc/free ok\n");
}

struct thread_smoke_arg {
  int thread_idx;
  int iterations;
};

static void *thread_malloc_free_worker(void *argp) {
  thread_smoke_arg *arg = (thread_smoke_arg *)argp;
  pin_to_test_cpu();

  for (int iter = 0; iter < arg->iterations; iter++) {
    const size_t alloc_sz =
        (32U << 20) + (size_t)((arg->thread_idx + iter) % 4) * (1U << 20);
    void *p = malloc(alloc_sz);
    if (!p)
      die("thread=%d iter=%d malloc(%zu) failed", arg->thread_idx, iter,
          alloc_sz);
    if (!is_in_test_arena(p))
      die("thread=%d iter=%d malloc VA %p not in expected cpu%d arena",
          arg->thread_idx, iter, p, g_test_cpu);
    touch_rw(p, alloc_sz);
    yield_many(4);
    free(p);
    yield_many(4);
  }
  return nullptr;
}

static void run_multi_thread_malloc_free_smoke(void) {
  pin_to_test_cpu();

  int threads = (int)env_long("SCUDO_SHARED_ARENA_TEST_THREADS", 4);
  int iterations = (int)env_long("SCUDO_SHARED_ARENA_TEST_THREAD_ITERS", 4);
  if (threads <= 0 || threads > 16)
    die("invalid SCUDO_SHARED_ARENA_TEST_THREADS=%d", threads);
  if (iterations <= 0 || iterations > 64)
    die("invalid SCUDO_SHARED_ARENA_TEST_THREAD_ITERS=%d", iterations);

  pthread_t tids[16];
  thread_smoke_arg args[16];
  for (int i = 0; i < threads; i++) {
    args[i].thread_idx = i;
    args[i].iterations = iterations;
    if (pthread_create(&tids[i], nullptr, thread_malloc_free_worker,
                       &args[i]) != 0)
      die("pthread_create(%d) failed errno=%d", i, errno);
  }
  for (int i = 0; i < threads; i++) {
    if (pthread_join(tids[i], nullptr) != 0)
      die("pthread_join(%d) failed errno=%d", i, errno);
  }

  fprintf(stderr, "PASS(scudo): shared arena multi-thread malloc/free ok\n");
}

static void split_arena_vma_once(uintptr_t addr, size_t len) {
  if (mprotect((void *)addr, len, PROT_READ) != 0)
    die("mprotect(PROT_READ, addr=0x%016" PRIxPTR ", len=%zu) failed errno=%d",
        addr, len, errno);
  if (mprotect((void *)addr, len, PROT_READ | PROT_WRITE) != 0)
    die("mprotect(PROT_READ|PROT_WRITE, addr=0x%016" PRIxPTR
        ", len=%zu) failed errno=%d",
        addr, len, errno);
}

static void run_lifecycle_smoke(void) {
  pin_to_test_cpu();

  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const size_t alloc_sz = 32U << 20;
  const uintptr_t base = arena_base() +
                         (uintptr_t)g_test_cpu * arena_cap_per_core();
  const uintptr_t cap = arena_cap_per_core();
  const uintptr_t split_base = (base + cap / 2) & ~(uintptr_t)(page - 1);
  const int split_iters =
      (int)env_long("SCUDO_SHARED_ARENA_TEST_SPLIT_ITERS", 64);
  const int exit_children =
      (int)env_long("SCUDO_SHARED_ARENA_TEST_EXIT_CHILDREN", 12);

  if (split_iters <= 0 || split_iters > 4096)
    die("invalid SCUDO_SHARED_ARENA_TEST_SPLIT_ITERS=%d", split_iters);
  if (exit_children <= 0 || exit_children > 128)
    die("invalid SCUDO_SHARED_ARENA_TEST_EXIT_CHILDREN=%d", exit_children);

  fprintf(stderr,
          "lifecycle: cpu=%d arena=[0x%016" PRIxPTR ",0x%016" PRIxPTR
          ") split_base=0x%016" PRIxPTR " split_iters=%d exit_children=%d\n",
          g_test_cpu, base, base + cap, split_base, split_iters,
          exit_children);

  void *p = malloc(alloc_sz);
  if (!p)
    die("lifecycle: initial malloc(%zu) failed", alloc_sz);
  if (!is_in_test_arena(p))
    die("lifecycle: initial malloc VA %p not in expected cpu%d arena",
        p, g_test_cpu);
  yield_many(80);
  touch_rw(p, alloc_sz);

  for (int i = 0; i < split_iters; i++) {
    const uintptr_t addr = split_base + (uintptr_t)(i % 16) * page;
    split_arena_vma_once(addr, page);
  }

  free(p);
  yield_many(80);

  for (int i = 0; i < exit_children; i++) {
    pid_t pid = fork();
    if (pid < 0)
      die("lifecycle: fork child=%d failed errno=%d", i, errno);
    if (pid == 0) {
      pin_to_test_cpu();
      void *child_p = malloc(alloc_sz);
      if (!child_p)
        die("lifecycle child=%d: malloc(%zu) failed", i, alloc_sz);
      if (!is_in_test_arena(child_p))
        die("lifecycle child=%d: malloc VA %p not in expected cpu%d arena",
            i, child_p, g_test_cpu);
      yield_many(40);
      touch_rw(child_p, alloc_sz);
      split_arena_vma_once(split_base + (uintptr_t)(i % 16) * page, page);
      // Intentionally exit without free(): mm_release must clear ownership.
      _exit(0);
    }

    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
      die("lifecycle: waitpid child=%d failed errno=%d", i, errno);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
      die("lifecycle: child=%d failed status=%d", i, st);
  }

  yield_many(160);
  fprintf(stderr, "PASS(scudo): shared arena lifecycle split/exit ok\n");
}

static void empty_yield_child_main(int ready_fd, int start_fd, int child_idx,
                                  int iterations,
                                  EmptyYieldSwitchState *state,
                                  uint64_t *samples, bool strict_next) {
  pin_to_test_cpu();

  const size_t alloc_sz = 32U << 20;
  void *p = malloc(alloc_sz);
  if (!p)
    die("empty_yield child=%d: malloc(%zu) failed", child_idx, alloc_sz);
  if (!is_in_test_arena(p))
    die("empty_yield child=%d: malloc VA %p not in expected cpu%d arena",
        child_idx, p, g_test_cpu);

  // Create and then drain one ownership transition so this mm is an active
  // arena participant, but the measured phase has no pending log entries.
  yield_many_tag(80, YIELD_WARMUP);
  touch_rw(p, alloc_sz);
  free(p);
  yield_many_tag(80, YIELD_WARMUP);

  char ready = 'R';
  write_full(ready_fd, &ready, 1);
  char start = 0;
  read_full(start_fd, &start, 1);
  if (start != 'S')
    die("empty_yield child=%d: unexpected start byte=%d", child_idx, start);

  reset_yield_stats();
  if (child_idx == 0) {
    for (int i = 0; i < iterations; i++) {
      for (;;) {
        while (load_shared_u32(&state->turn) != 0)
          sched_yield();
        store_shared_u32(&state->index, (uint32_t)i);
        store_shared_u32(&state->miss, 0);
        store_shared_u64(&state->start, read_test_cycles());
        store_shared_u32(&state->turn, 1);
        sched_yield();
        if (!strict_next || load_shared_u32(&state->turn) == 0)
          break;
        uint32_t expected = 1;
        if (!cmpxchg_shared_u32(&state->turn, &expected, 3))
          break;
        add_shared_u64(&state->rejected, 1);
      }
    }
    while (load_shared_u32(&state->turn) != 0)
      sched_yield();
  } else if (child_idx == 1) {
    for (int accepted = 0; accepted < iterations;) {
      uint32_t turn = 0;
      while ((turn = load_shared_u32(&state->turn)) == 0)
        sched_yield();
      if (turn == 3) {
        store_shared_u32(&state->turn, 0);
        sched_yield();
        continue;
      }
      if (turn != 1)
        die("empty_yield child=1: unexpected turn=%u", turn);
      uint32_t expected = 1;
      if (cmpxchg_shared_u32(&state->turn, &expected, 4)) {
        const uint64_t delta =
            read_test_cycles() - load_shared_u64(&state->start);
        samples[accepted] = delta;
        record_yield_sample(YIELD_EMPTY_SWITCH, delta, accepted == 0);
        accepted++;
        store_shared_u32(&state->turn, 0);
      }
      sched_yield();
    }
    print_sample_stats("EMPTY_YIELD", "empty_switch", samples, iterations);
  } else {
    die("empty_yield child=%d: only child 0/1 are valid", child_idx);
  }
  print_yield_stats("empty_child", child_idx);
  _exit(0);
}

static void run_empty_yield_smoke(void) {
  pin_to_test_cpu();

  const int children =
      (int)env_long("SCUDO_SHARED_ARENA_TEST_EMPTY_CHILDREN", 2);
  const int iterations =
      (int)env_long("SCUDO_SHARED_ARENA_TEST_EMPTY_ITERS", 1000);
  const bool strict_next =
      env_long("SCUDO_SHARED_ARENA_TEST_STRICT_NEXT", 1) != 0;
  if (children != 2)
    die("invalid SCUDO_SHARED_ARENA_TEST_EMPTY_CHILDREN=%d", children);
  if (iterations <= 0 || iterations > 100000)
    die("invalid SCUDO_SHARED_ARENA_TEST_EMPTY_ITERS=%d", iterations);

  const size_t state_size =
      sizeof(EmptyYieldSwitchState) + (size_t)iterations * sizeof(uint64_t);
  void *mapping = mmap(nullptr, state_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED)
    die("empty_yield: mmap state failed errno=%d", errno);
  memset(mapping, 0, state_size);

  EmptyYieldSwitchState *state = (EmptyYieldSwitchState *)mapping;
  uint64_t *samples = (uint64_t *)(state + 1);

  int ready_pipe[2][2];
  int start_pipe[2][2];
  pid_t pids[2];

  for (int i = 0; i < children; i++) {
    if (pipe(ready_pipe[i]) != 0 || pipe(start_pipe[i]) != 0)
      die("empty_yield: pipe child=%d failed errno=%d", i, errno);
  }

  for (int i = 0; i < children; i++) {
    pid_t pid = fork();
    if (pid < 0)
      die("empty_yield: fork child=%d failed errno=%d", i, errno);
    if (pid == 0) {
      for (int j = 0; j < children; j++) {
        if (j == i) {
          close(ready_pipe[j][0]);
          close(start_pipe[j][1]);
          continue;
        }
        close(ready_pipe[j][0]);
        close(ready_pipe[j][1]);
        close(start_pipe[j][0]);
        close(start_pipe[j][1]);
      }
      empty_yield_child_main(ready_pipe[i][1], start_pipe[i][0], i,
                             iterations, state, samples, strict_next);
    }

    pids[i] = pid;
    close(ready_pipe[i][1]);
    close(start_pipe[i][0]);
  }

  for (int i = 0; i < children; i++) {
    char ready = 0;
    read_full(ready_pipe[i][0], &ready, 1);
    close(ready_pipe[i][0]);
    if (ready != 'R')
      die("empty_yield: child=%d sent unexpected ready byte=%d", i, ready);
  }

  reset_kernel_switch_stats_if_requested();

  for (int i = 0; i < children; i++) {
    char start = 'S';
    write_full(start_pipe[i][1], &start, 1);
    close(start_pipe[i][1]);
  }

  for (int i = 0; i < children; i++) {
    int st = 0;
    if (waitpid(pids[i], &st, 0) < 0)
      die("empty_yield: waitpid child=%d failed errno=%d", i, errno);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
      die("empty_yield: child=%d failed status=%d", i, st);
  }

  fprintf(stderr,
          "PASS(scudo): shared arena empty-yield multi-proc ok children=%d iters=%d strict_next=%d rejected=%" PRIu64 "\n",
          children, iterations, strict_next ? 1 : 0,
          load_shared_u64(&state->rejected));
}

static bool issue_pending_yield_turn(PendingYieldState *state, uint32_t turn,
                                     int index, bool measured,
                                     bool strict_next,
                                     volatile uint64_t *rejected) {
  while (load_shared_u32(&state->turn) != 0)
    sched_yield();
  store_shared_u32(&state->index, (uint32_t)index);
  store_shared_u32(&state->miss, measured ? 0 : 1);
  store_shared_u64(&state->start, read_test_cycles());
  store_shared_u32(&state->turn, turn);
  sched_yield();
  if (measured && strict_next && load_shared_u32(&state->turn) == turn) {
    uint32_t expected = turn;
    if (!cmpxchg_shared_u32(&state->turn, &expected, 3)) {
      while (load_shared_u32(&state->turn) != 0)
        sched_yield();
      return true;
    }
    add_shared_u64(rejected, 1);
    while (load_shared_u32(&state->turn) != 0)
      sched_yield();
    return false;
  }
  while (load_shared_u32(&state->turn) != 0)
    sched_yield();
  return measured;
}

static void pending_yield_child_main(int ready_fd, int start_fd, int child_idx,
                                     int iterations, size_t alloc_sz,
                                     PendingYieldState *state,
                                     uint64_t *alloc_samples,
                                     uint64_t *free_samples,
                                     bool strict_next) {
  pin_to_test_cpu();

  void *warm = malloc(alloc_sz);
  if (!warm)
    die("pending_yield child=%d: warm malloc(%zu) failed", child_idx,
        alloc_sz);
  if (!is_in_test_arena(warm))
    die("pending_yield child=%d: warm malloc VA %p not in expected cpu%d arena",
        child_idx, warm, g_test_cpu);
  yield_many_tag(80, YIELD_WARMUP);
  touch_rw(warm, alloc_sz);
  free(warm);
  yield_many_tag(80, YIELD_WARMUP);

  char ready = 'R';
  write_full(ready_fd, &ready, 1);
  char start = 0;
  read_full(start_fd, &start, 1);
  if (start != 'S')
    die("pending_yield child=%d: unexpected start byte=%d", child_idx, start);

  if (child_idx == 0) {
    for (int i = 0; i < iterations; i++) {
      void *p = nullptr;
      for (;;) {
        p = malloc(alloc_sz);
        if (!p)
          die("pending_yield: measured malloc(%zu) failed iter=%d", alloc_sz,
              i);
        if (!is_in_test_arena(p))
          die("pending_yield: measured malloc VA %p not in expected cpu%d arena",
              p, g_test_cpu);
        if (issue_pending_yield_turn(state, 1, i, true, strict_next,
                                     &state->rejected_alloc))
          break;
        free(p);
        issue_pending_yield_turn(state, 2, i, false, false,
                                 &state->rejected_free);
      }

      for (;;) {
        free(p);
        if (issue_pending_yield_turn(state, 2, i, true, strict_next,
                                     &state->rejected_free))
          break;
        p = malloc(alloc_sz);
        if (!p)
          die("pending_yield: restore malloc(%zu) failed iter=%d", alloc_sz,
              i);
        if (!is_in_test_arena(p))
          die("pending_yield: restore malloc VA %p not in expected cpu%d arena",
              p, g_test_cpu);
        issue_pending_yield_turn(state, 1, i, false, false,
                                 &state->rejected_alloc);
      }
    }
  } else if (child_idx == 1) {
    int alloc_accepted = 0;
    int free_accepted = 0;
    while (alloc_accepted < iterations || free_accepted < iterations) {
      uint32_t turn = 0;
      while ((turn = load_shared_u32(&state->turn)) == 0)
        sched_yield();
      if (turn == 3) {
        store_shared_u32(&state->turn, 0);
        sched_yield();
        continue;
      }
      if (turn != 1 && turn != 2)
        die("pending_yield child=1: unexpected turn=%u", turn);
      uint32_t expected = turn;
      if (!cmpxchg_shared_u32(&state->turn, &expected, 4))
        continue;
      const bool miss = load_shared_u32(&state->miss) != 0;
      const uint32_t index = load_shared_u32(&state->index);
      const uint64_t delta = read_test_cycles() - load_shared_u64(&state->start);
      if (!miss && index < (uint32_t)iterations) {
        if (turn == 1) {
          alloc_samples[index] = delta;
          alloc_accepted++;
        } else if (turn == 2) {
          free_samples[index] = delta;
          free_accepted++;
        }
      }
      store_shared_u32(&state->turn, 0);
      sched_yield();
    }
    print_sample_stats("PENDING_YIELD", "alloc_log_to_next",
                       alloc_samples, iterations);
    print_sample_stats("PENDING_YIELD", "free_log_to_next",
                       free_samples, iterations);
  } else {
    die("pending_yield child=%d: only child 0/1 are valid", child_idx);
  }

  _exit(0);
}

static void run_pending_sync_yield_bench(void) {
  pin_to_test_cpu();

  const int children = 2;
  const int iterations =
      (int)env_long("SCUDO_SHARED_ARENA_TEST_PENDING_ITERS", 200);
  const size_t alloc_sz =
      (size_t)env_long("SCUDO_SHARED_ARENA_TEST_PENDING_SIZE", 32L << 20);
  const bool strict_next =
      env_long("SCUDO_SHARED_ARENA_TEST_STRICT_NEXT", 1) != 0;
  if (iterations <= 0 || iterations > 100000)
    die("invalid SCUDO_SHARED_ARENA_TEST_PENDING_ITERS=%d", iterations);
  if (alloc_sz == 0)
    die("invalid SCUDO_SHARED_ARENA_TEST_PENDING_SIZE=%zu", alloc_sz);

  const size_t state_size = sizeof(PendingYieldState) +
      (size_t)iterations * sizeof(uint64_t) * 2;
  void *mapping = mmap(nullptr, state_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED)
    die("pending_yield: mmap state failed errno=%d", errno);
  memset(mapping, 0, state_size);

  PendingYieldState *state = (PendingYieldState *)mapping;
  uint64_t *alloc_samples = (uint64_t *)(state + 1);
  uint64_t *free_samples = alloc_samples + iterations;

  int ready_pipe[2][2];
  int start_pipe[2][2];
  pid_t pids[2];

  for (int i = 0; i < children; i++) {
    if (pipe(ready_pipe[i]) != 0 || pipe(start_pipe[i]) != 0)
      die("pending_yield: pipe child=%d failed errno=%d", i, errno);
  }

  for (int i = 0; i < children; i++) {
    pid_t pid = fork();
    if (pid < 0)
      die("pending_yield: fork child=%d failed errno=%d", i, errno);
    if (pid == 0) {
      for (int j = 0; j < children; j++) {
        if (j == i) {
          close(ready_pipe[j][0]);
          close(start_pipe[j][1]);
          continue;
        }
        close(ready_pipe[j][0]);
        close(ready_pipe[j][1]);
        close(start_pipe[j][0]);
        close(start_pipe[j][1]);
      }
      pending_yield_child_main(ready_pipe[i][1], start_pipe[i][0], i,
                               iterations, alloc_sz, state, alloc_samples,
                               free_samples, strict_next);
    }

    pids[i] = pid;
    close(ready_pipe[i][1]);
    close(start_pipe[i][0]);
  }

  for (int i = 0; i < children; i++) {
    char ready = 0;
    read_full(ready_pipe[i][0], &ready, 1);
    close(ready_pipe[i][0]);
    if (ready != 'R')
      die("pending_yield: child=%d sent unexpected ready byte=%d", i, ready);
  }

  reset_kernel_switch_stats_if_requested();

  for (int i = 0; i < children; i++) {
    char start = 'S';
    write_full(start_pipe[i][1], &start, 1);
    close(start_pipe[i][1]);
  }

  for (int i = 0; i < children; i++) {
    int st = 0;
    if (waitpid(pids[i], &st, 0) < 0)
      die("pending_yield: waitpid child=%d failed errno=%d", i, errno);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
      die("pending_yield: child=%d failed status=%d", i, st);
  }

  fprintf(stderr,
          "PASS(scudo): shared arena pending-sync yield ok iters=%d size=%zu strict_next=%d rejected_alloc=%" PRIu64 " rejected_free=%" PRIu64 "\n",
          iterations, alloc_sz, strict_next ? 1 : 0,
          load_shared_u64(&state->rejected_alloc),
          load_shared_u64(&state->rejected_free));
}

static void expect_segv_on_touch(void *p, size_t n) {
  g_saw_segv = 0;
  g_expect_segv_touch = 1;
  if (sigsetjmp(g_jmp, 1) == 0) {
    touch_rw(p, n);
    // If we got here, no SIGSEGV happened.
    die("expected SIGSEGV touching %p (len=%zu), but touch succeeded", p, n);
  }
  if (!g_saw_segv)
    die("siglongjmp happened but g_saw_segv=0 (unexpected)");
  g_expect_segv_touch = 0;
}

enum cmd_type : uint32_t {
  CMD_ALLOC = 1,
  CMD_FREE = 2,
  CMD_TOUCH_EXPECT_SEGV = 3,
  CMD_EXIT = 4,
};

struct cmd_msg {
  uint32_t type;
  uint32_t pad;
  uint64_t arg0;
  uint64_t arg1;
};

static void write_full(int fd, const void *buf, size_t n) {
  const uint8_t *p = (const uint8_t *)buf;
  while (n) {
    ssize_t r = write(fd, p, n);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      die("write(fd=%d) failed errno=%d", fd, errno);
    }
    p += (size_t)r;
    n -= (size_t)r;
  }
}

static void read_full(int fd, void *buf, size_t n) {
  uint8_t *p = (uint8_t *)buf;
  while (n) {
    ssize_t r = read(fd, p, n);
    if (r == 0)
      die("read(fd=%d) EOF", fd);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      die("read(fd=%d) failed errno=%d", fd, errno);
    }
    p += (size_t)r;
    n -= (size_t)r;
  }
}

static void child_main(int to_child, int to_parent, int child_idx) {
  pin_to_test_cpu();
  install_segv_handler();

  void *owned = NULL;

  for (;;) {
    struct cmd_msg m;
    read_full(to_child, &m, sizeof(m));
    fprintf(stderr, "child=%d pid=%d: cmd=%u\n", child_idx, getpid(), m.type);

    if (m.type == CMD_EXIT) {
      print_yield_stats("child", child_idx);
      _exit(0);
    } else if (m.type == CMD_ALLOC) {
      pin_to_test_cpu();
      const size_t sz = (size_t)m.arg0;

      fprintf(stderr,
              "child=%d pid=%d: CMD_ALLOC malloc sz=%zu sched_cpu=%d\n",
              child_idx, getpid(), sz, (int)sched_getcpu());

      void *p = malloc(sz);
      if (!p)
        die("malloc failed sz=%zu", sz);
      if (!is_in_test_arena(p))
        die("malloc VA %p not in expected cpu%d shared arena", p, g_test_cpu);

      // malloc() appends ownership to the shared log ring. Kernel commits
      // ownership on context switch, so yield before touching pages.
      fprintf(stderr,
              "child=%d pid=%d: yield_before_touch p=%p sz=%zu\n",
              child_idx, getpid(), p, sz);
      yield_many_tag(80, YIELD_AFTER_ALLOC_LOG);
      fprintf(stderr,
              "child=%d pid=%d: after_yield_before_touch p=%p sz=%zu\n",
              child_idx, getpid(), p, sz);
      fprintf(stderr,
              "child=%d pid=%d: touch_rw start p=%p sz=%zu\n",
              child_idx, getpid(), p, sz);
      touch_rw(p, sz);
      fprintf(stderr,
              "child=%d pid=%d: touch_rw done p=%p\n",
              child_idx, getpid(), p);
      yield_many_tag(20, YIELD_AFTER_TOUCH);

      owned = p;

      uint64_t resp[2] = {(uint64_t)(uintptr_t)p, (uint64_t)sz};
      write_full(to_parent, resp, sizeof(resp));
    } else if (m.type == CMD_FREE) {
      if (!owned)
        die("CMD_FREE with no owned allocation");
      if (g_free_cpu >= 0) {
        fprintf(stderr,
                "child=%d pid=%d: migrate_to_free_cpu cpu=%d before free\n",
                child_idx, getpid(), g_free_cpu);
        pin_to_cpu(g_free_cpu);
        yield_many_tag(20, YIELD_MIGRATE_FREE);
      }
      free(owned);
      owned = NULL;
      yield_many_tag(80, YIELD_AFTER_FREE_LOG);
      if (g_free_cpu >= 0) {
        pin_to_test_cpu();
        yield_many_tag(20, YIELD_MIGRATE_FREE);
      }
      uint64_t ok = 1;
      write_full(to_parent, &ok, sizeof(ok));
    } else if (m.type == CMD_TOUCH_EXPECT_SEGV) {
      void *p = (void *)(uintptr_t)m.arg0;
      size_t sz = (size_t)m.arg1;
      // Give scheduler a chance to switch from owner -> us.
      yield_many_tag(40, YIELD_BEFORE_SEGV_TOUCH);
      expect_segv_on_touch(p, sz);
      uint64_t ok = 1;
      write_full(to_parent, &ok, sizeof(ok));
    } else {
      die("unknown cmd type=%u", m.type);
    }
  }
}

int main(void) {
  fprintf(stderr, "scudo_shared_arena_test: start\n");

  // Hard timeout: avoid hanging QEMU runs.
  install_alarm((int)env_long("SCUDO_SHARED_ARENA_TEST_TIMEOUT", 40));
  g_yield_stats_enabled =
      env_long("SCUDO_SHARED_ARENA_TEST_YIELD_STATS", 0) != 0;

  init_test_cpu();
  fprintf(stderr, "scudo_shared_arena_test: target_cpu=%d free_cpu=%d\n",
          g_test_cpu, g_free_cpu);

  pin_to_test_cpu();

  // Debug mode: only run one malloc/free pair and exit, to validate
  // kernel/userspace wiring without stressing revoke/fault paths.
  const char *mode = getenv("SCUDO_SHARED_ARENA_TEST_MODE");
  if (mode && mode[0] != '\0' && strcmp(mode, "init_only") == 0) {
    run_single_process_malloc_free_smoke();
    fprintf(stderr, "MODE(init_only): malloc_free_smoke=1\n");
    return 0;
  }
  if (mode && mode[0] != '\0' && strcmp(mode, "single_process") == 0) {
    run_single_process_malloc_free_smoke();
    return 0;
  }
  if (mode && mode[0] != '\0' && strcmp(mode, "multi_thread") == 0) {
    run_multi_thread_malloc_free_smoke();
    return 0;
  }
  if (mode && mode[0] != '\0' && strcmp(mode, "pressure_toggle") == 0) {
    run_pressure_toggle_smoke();
    return 0;
  }
  if (mode && mode[0] != '\0' && strcmp(mode, "lifecycle") == 0) {
    run_lifecycle_smoke();
    return 0;
  }
  if (mode && mode[0] != '\0' && strcmp(mode, "empty_yield") == 0) {
    run_empty_yield_smoke();
    return 0;
  }
  if (mode && mode[0] != '\0' && strcmp(mode, "pending_sync_yield") == 0) {
    run_pending_sync_yield_bench();
    return 0;
  }

  // Warm-up: populate free list in SharedArena (best-effort).
  // Use large allocations to force the Secondary allocator (SharedArena is in
  // secondary.h). Keep it below per-core arena capacity.
  size_t warm_iters = 8;
  const long warm_iters_env =
      env_long("SCUDO_SHARED_ARENA_TEST_WARM_ITERS", (long)warm_iters);
  if (warm_iters_env < 0)
    die("invalid SCUDO_SHARED_ARENA_TEST_WARM_ITERS=%ld", warm_iters_env);
  warm_iters = (size_t)warm_iters_env;

  const long skip_warmup =
      env_long("SCUDO_SHARED_ARENA_TEST_SKIP_WARMUP", 0);
  if (!skip_warmup) {
    for (size_t i = 0; i < warm_iters; i++) {
      const size_t sz = (32U << 20) + (i % 8) * (1U << 20); // 32MB .. 39MB
      void *p = malloc(sz);
      if (!p)
        die("warmup malloc failed at iter=%zu", i);
      touch_rw(p, sz);
      free(p);
      if ((i & 63u) == 0u)
        yield_many_tag(2, YIELD_WARMUP);
    }
  } else {
    fprintf(stderr, "TEST_CFG: skip warmup\n");
  }
  yield_many_tag(100, YIELD_WARMUP);

  enum { N = 4 };
  int pc[N][2]; // parent->child
  int cp[N][2]; // child->parent
  pid_t pids[N];

  for (int i = 0; i < N; i++) {
    if (pipe(pc[i]) != 0 || pipe(cp[i]) != 0)
      die("pipe failed errno=%d", errno);
  }

  for (int i = 0; i < N; i++) {
    pid_t pid = fork();
    if (pid < 0) {
      die("fork failed errno=%d", errno);
    } else if (pid == 0) {
      // Child i: keep pc[i][0] and cp[i][1], close everything else.
      for (int j = 0; j < N; j++) {
        if (j == i) {
          close(pc[j][1]);
          close(cp[j][0]);
          continue;
        }
        close(pc[j][0]);
        close(pc[j][1]);
        close(cp[j][0]);
        close(cp[j][1]);
      }
      child_main(pc[i][0], cp[i][1], i);
      _exit(0);
    }

    // Parent: keep pc[i][1] and cp[i][0].
    pids[i] = pid;
    fprintf(stderr, "parent: child %d pid=%d pc_w=%d cp_r=%d\n",
            i, (int)pid, pc[i][1], cp[i][0]);
    close(pc[i][0]);
    close(cp[i][1]);
  }

  bool saw_reuse = false;
  uintptr_t last_addr = 0;

  const size_t alloc_sz = 32U << 20; // 32MB (force Secondary)
  int max_rounds = N * 2;
  {
    const char *mr = getenv("SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS");
    if (mr && mr[0] != '\0') {
      long v = env_long("SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS", 0);
      if (v > 0)
        max_rounds = (v < (long)N * 2) ? (int)v : (N * 2);
      fprintf(stderr,
              "TEST_CFG: SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=%s => %d/%d\n",
              mr, max_rounds, N * 2);
    }
  }
  for (int round = 0; round < max_rounds; round++) {
    const int owner = round % N;

    fprintf(stderr, "round=%d owner=%d: alloc...\n", round, owner);
    struct cmd_msg m = {};
    m.type = CMD_ALLOC;
    m.arg0 = (uint64_t)alloc_sz;
    write_full(pc[owner][1], &m, sizeof(m));

    uint64_t resp[2] = {};
    fprintf(stderr, "round=%d owner=%d: waiting alloc resp...\n", round, owner);
    read_full(cp[owner][0], resp, sizeof(resp));
    void *addr = (void *)(uintptr_t)resp[0];
    size_t sz = (size_t)resp[1];
    fprintf(stderr, "round=%d owner=%d: alloc resp addr=%p sz=%zu\n",
            round, owner, addr, sz);

    if (!is_in_test_arena(addr)) {
      const uintptr_t base = arena_base() +
                             (uintptr_t)g_test_cpu * arena_cap_per_core();
      const uintptr_t cap = arena_cap_per_core();
      die("allocation VA %p not in expected cpu%d shared arena range [0x%016" PRIxPTR
          ", 0x%016" PRIxPTR ")",
          addr, g_test_cpu, base, base + cap);
    }

    if ((uintptr_t)addr == last_addr)
      saw_reuse = true;
    last_addr = (uintptr_t)addr;

    // Ask all other processes to touch and expect SIGSEGV.
    fprintf(stderr, "round=%d owner=%d: asking others to touch(expect SEGV)...\n",
            round, owner);
    for (int i = 0; i < N; i++) {
      if (i == owner)
        continue;
      struct cmd_msg t = {};
      t.type = CMD_TOUCH_EXPECT_SEGV;
      t.arg0 = (uint64_t)(uintptr_t)addr;
      t.arg1 = (uint64_t)sz;
      write_full(pc[i][1], &t, sizeof(t));
    }
    for (int i = 0; i < N; i++) {
      if (i == owner)
        continue;
      uint64_t ok = 0;
      fprintf(stderr, "round=%d owner=%d: waiting touch resp from %d...\n",
              round, owner, i);
      read_full(cp[i][0], &ok, sizeof(ok));
      if (ok != 1)
        die("child %d touch response not ok", i);
    }

    // Free in owner.
    fprintf(stderr, "round=%d owner=%d: free...\n", round, owner);
    struct cmd_msg f = {};
    f.type = CMD_FREE;
    write_full(pc[owner][1], &f, sizeof(f));
    uint64_t ok = 0;
    fprintf(stderr, "round=%d owner=%d: waiting free resp...\n", round, owner);
    read_full(cp[owner][0], &ok, sizeof(ok));
    if (ok != 1)
      die("owner %d free response not ok", owner);

    yield_many_tag(80, YIELD_PARENT_AFTER_FREE);
  }

  // Shutdown children.
  for (int i = 0; i < N; i++) {
    struct cmd_msg e = {};
    e.type = CMD_EXIT;
    write_full(pc[i][1], &e, sizeof(e));
    close(pc[i][1]);
  }
  for (int i = 0; i < N; i++) {
    int st = 0;
    (void)waitpid(pids[i], &st, 0);
  }

  if (!saw_reuse)
    fprintf(stderr, "WARN(scudo): no immediate VA reuse observed (still ok)\n");

  print_yield_stats("parent", -1);
  fprintf(stderr, "PASS(scudo): shared arena malloc/free multi-proc revoke ok\n");
  fprintf(stderr, "PASS(scudo): shared arena retrieve/store ok\n");
  return 0;
}
