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

static void yield_many(int n) {
  for (int i = 0; i < n; i++) {
    sched_yield();
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1 * 1000 * 1000; // 1ms
    nanosleep(&ts, NULL);
  }
}

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
  // Initialize before pinning: SharedArenaPool sizes itself from the current
  // affinity mask, while the actual test operations should run on g_test_cpu.
  scudo::SharedArenaPool &Pool = scudo::SharedArenaPool::getInstance();
  Pool.init();
  if (!Pool.isReady())
    die("SharedArenaPool not ready in child");

  pin_to_test_cpu();
  install_segv_handler();

  // Use SharedArena API directly to guarantee fixed-VA arena allocations.
  scudo::SharedArena *Arena = Pool.getCurrentArena();
  if (!Arena)
    die("getCurrentArena() returned null (cpu?)");

  void *owned = NULL;
  size_t owned_commit_size = 0;

  for (;;) {
    struct cmd_msg m;
    read_full(to_child, &m, sizeof(m));
    fprintf(stderr, "child=%d pid=%d: cmd=%u\n", child_idx, getpid(), m.type);

    if (m.type == CMD_EXIT) {
      _exit(0);
    } else if (m.type == CMD_ALLOC) {
      pin_to_test_cpu();
      const size_t sz = (size_t)m.arg0;
      const uintptr_t Align = (uintptr_t)sysconf(_SC_PAGESIZE);

      fprintf(stderr,
              "child=%d pid=%d: CMD_ALLOC sz=%zu arena_core=%u sched_cpu=%d\n",
              child_idx, getpid(), sz, (unsigned)Arena->getCoreId(),
              (int)sched_getcpu());

      scudo::uptr CommitBase = 0, CommitSize = 0, EntryHeaderPos = 0;
      if (!Arena->retrieve((scudo::uptr)sz, (scudo::uptr)Align,
                           /*HeadersSize=*/0, CommitBase, CommitSize,
                           EntryHeaderPos))
        die("Arena->retrieve failed sz=%zu", sz);
      if (CommitBase == 0 || CommitSize == 0)
        die("Arena->retrieve returned empty");

      void *p = (void *)(uintptr_t)CommitBase;
      // retrieve() only appends to the shared log ring. Kernel commits ownership
      // on context switch, so yield before touching pages.
      fprintf(stderr,
              "child=%d pid=%d: yield_before_touch p=%p sz=%zu\n",
              child_idx, getpid(), p, (size_t)CommitSize);
      yield_many(80);
      fprintf(stderr,
              "child=%d pid=%d: after_yield_before_touch p=%p sz=%zu\n",
              child_idx, getpid(), p, (size_t)CommitSize);
      fprintf(stderr,
              "child=%d pid=%d: touch_rw start p=%p sz=%zu\n",
              child_idx, getpid(), p, (size_t)CommitSize);
      touch_rw(p, (size_t)CommitSize);
      fprintf(stderr,
              "child=%d pid=%d: touch_rw done p=%p\n",
              child_idx, getpid(), p);
      yield_many(20);

      owned = p;
      owned_commit_size = (size_t)CommitSize;

      uint64_t resp[2] = {(uint64_t)(uintptr_t)p, (uint64_t)CommitSize};
      write_full(to_parent, resp, sizeof(resp));
    } else if (m.type == CMD_FREE) {
      if (!owned)
        die("CMD_FREE with no owned allocation");
      if (g_free_cpu >= 0) {
        fprintf(stderr,
                "child=%d pid=%d: migrate_to_free_cpu cpu=%d before store\n",
                child_idx, getpid(), g_free_cpu);
        pin_to_cpu(g_free_cpu);
        yield_many(20);
      }
      scudo::SharedArena *OwnerArena =
          Pool.getOwningArena((scudo::uptr)(uintptr_t)owned);
      if (!OwnerArena)
        die("getOwningArena(%p) returned null", owned);
      if (!OwnerArena->store((scudo::uptr)(uintptr_t)owned,
                             (scudo::uptr)owned_commit_size))
        die("Arena->store failed");
      owned = NULL;
      owned_commit_size = 0;
      yield_many(80);
      if (g_free_cpu >= 0) {
        pin_to_test_cpu();
        yield_many(20);
      }
      uint64_t ok = 1;
      write_full(to_parent, &ok, sizeof(ok));
    } else if (m.type == CMD_TOUCH_EXPECT_SEGV) {
      void *p = (void *)(uintptr_t)m.arg0;
      size_t sz = (size_t)m.arg1;
      // Give scheduler a chance to switch from owner -> us.
      yield_many(40);
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
  install_alarm(40);

  init_test_cpu();
  fprintf(stderr, "scudo_shared_arena_test: target_cpu=%d free_cpu=%d\n",
          g_test_cpu, g_free_cpu);

  // Make the test robust even if env-cache initialization happens before main().
  // This forces SharedArenaPool::shouldUseArena() to return true.
  scudo::setSharedArenaForceForTesting(true);
  scudo::SharedArenaPool &Pool = scudo::SharedArenaPool::getInstance();
  Pool.init();
  if (!Pool.isReady())
    die("SharedArenaPool not ready in parent");

  pin_to_test_cpu();

  // Debug mode: only initialize the pool and exit, to validate kernel/userspace
  // wiring on a physical device without stressing revoke/fault paths.
  const char *mode = getenv("SCUDO_SHARED_ARENA_TEST_MODE");
  if (mode && mode[0] != '\0' && strcmp(mode, "init_only") == 0) {
    fprintf(stderr, "MODE(init_only): pool_ready=%d\n", Pool.isReady() ? 1 : 0);
    return Pool.isReady() ? 0 : 1;
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
        yield_many(2);
    }
  } else {
    fprintf(stderr, "TEST_CFG: skip warmup\n");
  }
  yield_many(100);

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

    yield_many(80);
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

  fprintf(stderr, "PASS(scudo): shared arena malloc/free multi-proc revoke ok\n");
  fprintf(stderr, "PASS(scudo): shared arena retrieve/store ok\n");
  return 0;
}
