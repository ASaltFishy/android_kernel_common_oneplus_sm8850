// SPDX-License-Identifier: GPL-2.0
/*
 * Camera SharedArena donor workload.
 *
 * This helper is meant to run before a real camera burst experiment. It
 * allocates/touches/frees Scudo Secondary-sized chunks and then stays alive, so
 * default Scudo keeps private secondary cache while SharedArena returns those
 * chunks to the per-core shared arena freelist.
 */

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "shared_arena.h"

namespace {

struct AllocatorMemKb {
  uint64_t ScudoSecondaryRss;
  uint64_t ScudoSecondaryPss;
  uint64_t ScudoSecondaryPrivate;
  uint64_t ScudoSecondaryShared;
  uint64_t ArenaRss;
  uint64_t ArenaPss;
  uint64_t ArenaPrivate;
  uint64_t ArenaShared;
};

volatile sig_atomic_t g_stop;
int g_cpu = 0;
int g_chunks = 128;
size_t g_chunk_size = 1U << 20;
int g_hold_secs = 300;
int g_touch_rounds = 1;
int g_verify_arena = 0;

void onSignal(int) { g_stop = 1; }

void die(const char *Msg) {
  fprintf(stderr, "FAIL(camera_donor): %s errno=%d\n", Msg, errno);
  _exit(1);
}

long envLong(const char *Name, long Fallback) {
  const char *Value = getenv(Name);
  if (!Value || Value[0] == '\0')
    return Fallback;
  char *End = nullptr;
  errno = 0;
  const long Parsed = strtol(Value, &End, 10);
  if (errno != 0 || End == Value || (End && *End != '\0')) {
    fprintf(stderr, "FAIL(camera_donor): invalid %s=%s\n", Name, Value);
    _exit(2);
  }
  return Parsed;
}

void initConfig() {
  g_cpu = (int)envLong("MD_CAMERA_DONOR_CPU", 0);
  g_chunks = (int)envLong("MD_CAMERA_DONOR_CHUNKS", 128);
  g_chunk_size = (size_t)envLong("MD_CAMERA_DONOR_CHUNK_KB", 1024) << 10;
  g_hold_secs = (int)envLong("MD_CAMERA_DONOR_HOLD_SECS", 300);
  g_touch_rounds = (int)envLong("MD_CAMERA_DONOR_TOUCH_ROUNDS", 1);
  g_verify_arena = (int)envLong("MD_CAMERA_DONOR_VERIFY_ARENA", 0);

  if (g_cpu < 0 || g_cpu >= CPU_SETSIZE || g_chunks <= 0 ||
      g_chunks > 4096 || g_chunk_size < (64U << 10) ||
      g_chunk_size > (2U << 20) || g_hold_secs < 0 ||
      g_touch_rounds <= 0 || g_touch_rounds > 16) {
    fprintf(stderr,
            "FAIL(camera_donor): bad config cpu=%d chunks=%d chunk_size=%zu "
            "hold_secs=%d touch_rounds=%d\n",
            g_cpu, g_chunks, g_chunk_size, g_hold_secs, g_touch_rounds);
    _exit(2);
  }
}

void pinToCpu() {
  cpu_set_t Set;
  CPU_ZERO(&Set);
  CPU_SET(g_cpu, &Set);
  if (sched_setaffinity(0, sizeof(Set), &Set) != 0)
    die("sched_setaffinity");
}

uintptr_t arenaBaseForCpu(int Cpu) {
  return (uintptr_t)scudo::kSharedArenaBaseAddr +
         (uintptr_t)Cpu * (uintptr_t)scudo::kArenaCapacityPerCore;
}

bool isInAnyArena(void *Ptr) {
  const uintptr_t V = (uintptr_t)Ptr;
  const uintptr_t Base = (uintptr_t)scudo::kSharedArenaBaseAddr;
  const uintptr_t Cap = (uintptr_t)scudo::kArenaCapacityPerCore;
  for (uint32_t Core = 0; Core < scudo::kArenaMaxCores; ++Core) {
    const uintptr_t Start = Base + (uintptr_t)Core * Cap;
    if (V >= Start && V < Start + Cap)
      return true;
  }
  return false;
}

void touchEveryPage(void *Ptr, size_t Size, uint8_t Seed) {
  const size_t Page = (size_t)sysconf(_SC_PAGESIZE);
  volatile uint8_t *P = static_cast<volatile uint8_t *>(Ptr);
  for (size_t Off = 0; Off < Size; Off += Page)
    P[Off] = (uint8_t)(Seed + (uint8_t)(Off >> 12));
  P[Size - 1] ^= Seed;
}

bool rangeOverlaps(uintptr_t AStart, uintptr_t AEnd, uintptr_t BStart,
                   uintptr_t BEnd) {
  return AStart < BEnd && BStart < AEnd;
}

bool isArenaRange(uintptr_t Start, uintptr_t End) {
  const uintptr_t Base = (uintptr_t)scudo::kSharedArenaBaseAddr;
  const uintptr_t Cap = (uintptr_t)scudo::kArenaCapacityPerCore;
  for (uint32_t Core = 0; Core < scudo::kArenaMaxCores; ++Core) {
    const uintptr_t ArenaStart = Base + (uintptr_t)Core * Cap;
    if (rangeOverlaps(Start, End, ArenaStart, ArenaStart + Cap))
      return true;
  }
  return false;
}

void addMap(AllocatorMemKb &Stats, bool IsSecondary, bool IsArena,
            uint64_t Rss, uint64_t Pss, uint64_t Shared, uint64_t Private) {
  if (IsSecondary) {
    Stats.ScudoSecondaryRss += Rss;
    Stats.ScudoSecondaryPss += Pss;
    Stats.ScudoSecondaryShared += Shared;
    Stats.ScudoSecondaryPrivate += Private;
  }
  if (IsArena) {
    Stats.ArenaRss += Rss;
    Stats.ArenaPss += Pss;
    Stats.ArenaShared += Shared;
    Stats.ArenaPrivate += Private;
  }
}

AllocatorMemKb readAllocatorMemForSelf() {
  AllocatorMemKb Stats = {};
  FILE *Fp = fopen("/proc/self/smaps", "r");
  if (!Fp)
    return Stats;

  char Line[512];
  bool HaveMap = false;
  bool IsSecondary = false;
  bool IsArena = false;
  uint64_t Rss = 0;
  uint64_t Pss = 0;
  uint64_t Shared = 0;
  uint64_t Private = 0;

  auto Flush = [&]() {
    if (!HaveMap)
      return;
    addMap(Stats, IsSecondary, IsArena, Rss, Pss, Shared, Private);
  };

  while (fgets(Line, sizeof(Line), Fp)) {
    unsigned long Start = 0;
    unsigned long End = 0;
    char Perms[8] = {};
    char Rest[384] = {};
    if (sscanf(Line, "%lx-%lx %7s %*s %*s %*s %383[^\n]", &Start, &End,
               Perms, Rest) >= 3) {
      Flush();
      HaveMap = true;
      Rss = Pss = Shared = Private = 0;
      IsSecondary = strstr(Rest, "scudo:secondary") != nullptr;
      IsArena = isArenaRange((uintptr_t)Start, (uintptr_t)End);
      continue;
    }

    unsigned long Value = 0;
    if (sscanf(Line, "Rss: %lu kB", &Value) == 1)
      Rss = Value;
    else if (sscanf(Line, "Pss: %lu kB", &Value) == 1)
      Pss = Value;
    else if (sscanf(Line, "Shared_Clean: %lu kB", &Value) == 1)
      Shared += Value;
    else if (sscanf(Line, "Shared_Dirty: %lu kB", &Value) == 1)
      Shared += Value;
    else if (sscanf(Line, "Private_Clean: %lu kB", &Value) == 1)
      Private += Value;
    else if (sscanf(Line, "Private_Dirty: %lu kB", &Value) == 1)
      Private += Value;
  }
  Flush();
  fclose(Fp);
  return Stats;
}

void printStats(const char *Phase, size_t TotalBytes) {
  scudo::SharedArenaPoolDebugStats Arena = {};
  scudo::SharedArenaPool::getInstance().getDebugStats(Arena);

  const AllocatorMemKb Mem = readAllocatorMemForSelf();
  fprintf(stderr,
          "DONOR_STATS phase=%s pid=%d cpu=%d chunks=%d chunk_size=%zu "
          "total_bytes=%zu arena_bump_bytes=%" PRIu64
          " arena_free_reusable_bytes=%" PRIu64
          " arena_in_use_bytes=%" PRIu64
          " arena_donated_bytes=%" PRIu64
          " arena_retrieved_bytes=%" PRIu64
          " arena_donate_count=%" PRIu64
          " arena_retrieve_count=%" PRIu64
          " smaps_secondary_pss_kb=%" PRIu64
          " smaps_secondary_private_kb=%" PRIu64
          " smaps_arena_pss_kb=%" PRIu64
          " smaps_arena_private_kb=%" PRIu64
          " smaps_arena_shared_kb=%" PRIu64 "\n",
          Phase, (int)getpid(), g_cpu, g_chunks, g_chunk_size, TotalBytes,
          Arena.BumpBytes, Arena.FreeListBytes, Arena.InUseBytes,
          Arena.TotalDonatedBytes, Arena.TotalRetrievedBytes,
          (uint64_t)Arena.DonateCount, (uint64_t)Arena.RetrieveCount,
          Mem.ScudoSecondaryPss, Mem.ScudoSecondaryPrivate, Mem.ArenaPss,
          Mem.ArenaPrivate, Mem.ArenaShared);
}

void sleepUntilStop() {
  time_t Start = time(nullptr);
  while (!g_stop) {
    if (g_hold_secs != 0 && time(nullptr) - Start >= g_hold_secs)
      break;
    sleep(1);
  }
}

} // namespace

int main() {
  signal(SIGTERM, onSignal);
  signal(SIGINT, onSignal);
  initConfig();
  pinToCpu();

  const size_t TotalBytes = (size_t)g_chunks * g_chunk_size;
  fprintf(stderr,
          "md_camera_arena_donor: pid=%d cpu=%d chunks=%d chunk_size=%zu "
          "total_bytes=%zu hold_secs=%d touch_rounds=%d verify_arena=%d "
          "cpu_arena_base=0x%" PRIxPTR "\n",
          (int)getpid(), g_cpu, g_chunks, g_chunk_size, TotalBytes,
          g_hold_secs, g_touch_rounds, g_verify_arena, arenaBaseForCpu(g_cpu));

  void **Ptrs = static_cast<void **>(calloc((size_t)g_chunks, sizeof(void *)));
  if (!Ptrs)
    die("calloc");

  int ArenaAllocs = 0;
  for (int I = 0; I < g_chunks; ++I) {
    Ptrs[I] = malloc(g_chunk_size);
    if (!Ptrs[I])
      die("malloc");
    if (isInAnyArena(Ptrs[I]))
      ++ArenaAllocs;
    else if (g_verify_arena) {
      fprintf(stderr, "FAIL(camera_donor): ptr=%p outside shared arena\n",
              Ptrs[I]);
      _exit(1);
    }
    for (int Round = 0; Round < g_touch_rounds; ++Round)
      touchEveryPage(Ptrs[I], g_chunk_size, (uint8_t)(I + Round * 31));
    sched_yield();
  }

  printStats("after_alloc", TotalBytes);

  for (int I = 0; I < g_chunks; ++I)
    free(Ptrs[I]);
  free(Ptrs);

  for (int I = 0; I < 64; ++I)
    sched_yield();

  printStats("after_free", TotalBytes);
  fprintf(stderr,
          "DONOR_READY pid=%d arena_allocs=%d total_allocs=%d total_bytes=%zu\n",
          (int)getpid(), ArenaAllocs, g_chunks, TotalBytes);

  sleepUntilStop();
  printStats("before_exit", TotalBytes);
  fprintf(stderr,
          "DONOR_RESULT pid=%d arena_allocs=%d total_allocs=%d "
          "total_bytes=%zu stopped=%d\n",
          (int)getpid(), ArenaAllocs, g_chunks, TotalBytes, (int)g_stop);
  return 0;
}
