// SPDX-License-Identifier: GPL-2.0
//
// Replay Scudo allocator CSV traces captured from the camera process.

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <unordered_map>

#include "shared_arena.h"

namespace {

enum class TouchMode { None, Pages, Full };

struct Options {
  const char *TracePath = nullptr;
  TouchMode Touch = TouchMode::None;
  uint64_t MaxEvents = 0;
  uint32_t Repeat = 1;
  uint32_t TouchDelayUs = 0;
  uint32_t HoldLiveMs = 0;
  uint32_t PssSampleInterval = 0;
  const char *LiveReadyFile = nullptr;
};

struct Counters {
  uint64_t Events = 0;
  uint64_t Malloc = 0;
  uint64_t Calloc = 0;
  uint64_t Free = 0;
  uint64_t Realloc = 0;
  uint64_t Aligned = 0;
  uint64_t MissingFree = 0;
  uint64_t AllocFailures = 0;
  uint64_t BytesRequested = 0;
  uint64_t BytesTouched = 0;
  uint64_t Live = 0;
  uint64_t PeakLive = 0;
  uint64_t DelegatedArenaInUseBytes = 0;
  uint64_t DelegatedArenaFreeReusableBytes = 0;
  uint32_t DelegatedArenaReady = 0;
  uint64_t ArenaAllocAttempts = 0;
  uint64_t ArenaAllocSuccess = 0;
  uint64_t ArenaAllocFallback = 0;
  uint64_t LiveSelfPssKb = 0;
  uint64_t LiveSelfRssKb = 0;
  uint64_t PeakSelfPssKb = 0;
  uint64_t PeakPssDelegatedArenaInUseBytes = 0;
  uint64_t HoldNs = 0;
  uint64_t AllocNs = 0;
  uint64_t MallocNs = 0;
  uint64_t CallocNs = 0;
  uint64_t ReallocNs = 0;
  uint64_t AlignedNs = 0;
  uint64_t FreeNs = 0;
  uint64_t CleanupFreeNs = 0;
  uint64_t TouchNs = 0;
  uint64_t TouchOps = 0;
  uint32_t ArenaProfileEnabled = 0;
  uint64_t ArenaShouldUseCalls = 0;
  uint64_t ArenaShouldUseNs = 0;
  uint64_t ArenaGetCurrentCalls = 0;
  uint64_t ArenaGetCurrentNs = 0;
  uint64_t ArenaRetrieveLockWaitCalls = 0;
  uint64_t ArenaRetrieveLockWaitNs = 0;
  uint64_t ArenaFreelistScanCalls = 0;
  uint64_t ArenaFreelistScanNs = 0;
  uint64_t ArenaBumpCalls = 0;
  uint64_t ArenaBumpNs = 0;
  uint64_t ArenaAppendLogCalls = 0;
  uint64_t ArenaAppendLogNs = 0;
  uint64_t ArenaStoreInsertMergeCalls = 0;
  uint64_t ArenaStoreInsertMergeNs = 0;
  uint64_t ArenaHeaderSetupCalls = 0;
  uint64_t ArenaHeaderSetupNs = 0;
  uint64_t ArenaInUsePushCalls = 0;
  uint64_t ArenaInUsePushNs = 0;
  uint64_t ArenaInUseRemoveCalls = 0;
  uint64_t ArenaInUseRemoveNs = 0;
  uint64_t ArenaReturnOuterCalls = 0;
  uint64_t ArenaReturnOuterNs = 0;
  uint64_t ArenaRemoteStoreCalls = 0;
  uint64_t ArenaRemoteStoreNs = 0;
  uint64_t ArenaRemoteEnqueueCalls = 0;
  uint64_t ArenaRemoteEnqueueNs = 0;
  uint64_t ArenaRemoteDrainCalls = 0;
  uint64_t ArenaRemoteDrainNs = 0;
};

struct Allocation {
  void *Ptr = nullptr;
  size_t Size = 0;
};

uint64_t nowNs() {
  struct timespec Ts = {};
  clock_gettime(CLOCK_MONOTONIC, &Ts);
  return static_cast<uint64_t>(Ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(Ts.tv_nsec);
}

long maxRssKb() {
  struct rusage Usage = {};
  getrusage(RUSAGE_SELF, &Usage);
  return Usage.ru_maxrss;
}

double bytesToMb(uint64_t Bytes) {
  return static_cast<double>(Bytes) / (1024.0 * 1024.0);
}

void captureArenaStats(Counters *Stats) {
  scudo::SharedArenaPoolDebugStats ArenaStats = {};
  scudo::SharedArenaPool::getInstance().getDebugStats(ArenaStats);
  Stats->DelegatedArenaInUseBytes =
      static_cast<uint64_t>(ArenaStats.InUseBytes);
  Stats->DelegatedArenaFreeReusableBytes =
      static_cast<uint64_t>(ArenaStats.FreeListBytes);
  Stats->DelegatedArenaReady = ArenaStats.Initialized;
  Stats->ArenaAllocAttempts = ArenaStats.ArenaAllocAttempts;
  Stats->ArenaAllocSuccess = ArenaStats.ArenaAllocSuccess;
  Stats->ArenaAllocFallback = ArenaStats.ArenaAllocFallback;
  Stats->ArenaProfileEnabled = ArenaStats.ProfileEnabled;
  Stats->ArenaShouldUseCalls = ArenaStats.ProfileShouldUseArenaCalls;
  Stats->ArenaShouldUseNs = ArenaStats.ProfileShouldUseArenaNs;
  Stats->ArenaGetCurrentCalls = ArenaStats.ProfileGetCurrentArenaCalls;
  Stats->ArenaGetCurrentNs = ArenaStats.ProfileGetCurrentArenaNs;
  Stats->ArenaRetrieveLockWaitCalls = ArenaStats.ProfileRetrieveLockWaitCalls;
  Stats->ArenaRetrieveLockWaitNs = ArenaStats.ProfileRetrieveLockWaitNs;
  Stats->ArenaFreelistScanCalls = ArenaStats.ProfileFreelistScanCalls;
  Stats->ArenaFreelistScanNs = ArenaStats.ProfileFreelistScanNs;
  Stats->ArenaBumpCalls = ArenaStats.ProfileBumpCalls;
  Stats->ArenaBumpNs = ArenaStats.ProfileBumpNs;
  Stats->ArenaAppendLogCalls = ArenaStats.ProfileAppendLogCalls;
  Stats->ArenaAppendLogNs = ArenaStats.ProfileAppendLogNs;
  Stats->ArenaStoreInsertMergeCalls = ArenaStats.ProfileStoreInsertMergeCalls;
  Stats->ArenaStoreInsertMergeNs = ArenaStats.ProfileStoreInsertMergeNs;
  Stats->ArenaHeaderSetupCalls = ArenaStats.ProfileArenaHeaderSetupCalls;
  Stats->ArenaHeaderSetupNs = ArenaStats.ProfileArenaHeaderSetupNs;
  Stats->ArenaInUsePushCalls = ArenaStats.ProfileArenaInUsePushCalls;
  Stats->ArenaInUsePushNs = ArenaStats.ProfileArenaInUsePushNs;
  Stats->ArenaInUseRemoveCalls = ArenaStats.ProfileArenaInUseRemoveCalls;
  Stats->ArenaInUseRemoveNs = ArenaStats.ProfileArenaInUseRemoveNs;
  Stats->ArenaReturnOuterCalls = ArenaStats.ProfileArenaReturnOuterCalls;
  Stats->ArenaReturnOuterNs = ArenaStats.ProfileArenaReturnOuterNs;
  Stats->ArenaRemoteStoreCalls = ArenaStats.ProfileRemoteStoreCalls;
  Stats->ArenaRemoteStoreNs = ArenaStats.ProfileRemoteStoreNs;
  Stats->ArenaRemoteEnqueueCalls = ArenaStats.ProfileRemoteEnqueueCalls;
  Stats->ArenaRemoteEnqueueNs = ArenaStats.ProfileRemoteEnqueueNs;
  Stats->ArenaRemoteDrainCalls = ArenaStats.ProfileRemoteDrainCalls;
  Stats->ArenaRemoteDrainNs = ArenaStats.ProfileRemoteDrainNs;
}

void captureSelfSmapsRollup(Counters *Stats) {
  FILE *Fp = fopen("/proc/self/smaps_rollup", "r");
  if (!Fp)
    return;
  char Line[256];
  while (fgets(Line, sizeof(Line), Fp)) {
    unsigned long Value = 0;
    if (sscanf(Line, "Pss: %lu kB", &Value) == 1)
      Stats->LiveSelfPssKb = static_cast<uint64_t>(Value);
    else if (sscanf(Line, "Rss: %lu kB", &Value) == 1)
      Stats->LiveSelfRssKb = static_cast<uint64_t>(Value);
  }
  fclose(Fp);
  if (Stats->LiveSelfPssKb > Stats->PeakSelfPssKb) {
    Stats->PeakSelfPssKb = Stats->LiveSelfPssKb;
    captureArenaStats(Stats);
    Stats->PeakPssDelegatedArenaInUseBytes =
        Stats->DelegatedArenaInUseBytes;
  }
}

void writeLiveReadyFile(const char *Path) {
  if (!Path || !Path[0])
    return;
  FILE *Fp = fopen(Path, "w");
  if (!Fp)
    return;
  fprintf(Fp, "pid=%ld\n", static_cast<long>(getpid()));
  fclose(Fp);
}

void usage(const char *Argv0) {
  fprintf(stderr,
          "usage: %s --trace FILE [--touch none|pages|full] "
          "[--max-events N] [--repeat N] [--touch-delay-us N] "
          "[--hold-live-ms N] [--pss-sample-interval N] "
          "[--live-ready-file FILE]\n",
          Argv0);
}

bool parseU64(const char *Text, uint64_t *Out) {
  if (!Text || !Out)
    return false;
  char *End = nullptr;
  errno = 0;
  unsigned long long Value = strtoull(Text, &End, 0);
  if (errno || End == Text || *End != '\0')
    return false;
  *Out = static_cast<uint64_t>(Value);
  return true;
}

bool parseOptions(int argc, char **argv, Options *Opt) {
  for (int I = 1; I < argc; ++I) {
    if (!strcmp(argv[I], "--trace") && I + 1 < argc) {
      Opt->TracePath = argv[++I];
    } else if (!strcmp(argv[I], "--touch") && I + 1 < argc) {
      const char *Mode = argv[++I];
      if (!strcmp(Mode, "none"))
        Opt->Touch = TouchMode::None;
      else if (!strcmp(Mode, "pages"))
        Opt->Touch = TouchMode::Pages;
      else if (!strcmp(Mode, "full"))
        Opt->Touch = TouchMode::Full;
      else
        return false;
    } else if (!strcmp(argv[I], "--max-events") && I + 1 < argc) {
      uint64_t Value = 0;
      if (!parseU64(argv[++I], &Value))
        return false;
      Opt->MaxEvents = Value;
    } else if (!strcmp(argv[I], "--repeat") && I + 1 < argc) {
      uint64_t Value = 0;
      if (!parseU64(argv[++I], &Value) || Value == 0)
        return false;
      Opt->Repeat = static_cast<uint32_t>(Value);
    } else if (!strcmp(argv[I], "--touch-delay-us") && I + 1 < argc) {
      uint64_t Value = 0;
      if (!parseU64(argv[++I], &Value))
        return false;
      Opt->TouchDelayUs = static_cast<uint32_t>(Value);
    } else if (!strcmp(argv[I], "--hold-live-ms") && I + 1 < argc) {
      uint64_t Value = 0;
      if (!parseU64(argv[++I], &Value))
        return false;
      Opt->HoldLiveMs = static_cast<uint32_t>(Value);
    } else if (!strcmp(argv[I], "--pss-sample-interval") && I + 1 < argc) {
      uint64_t Value = 0;
      if (!parseU64(argv[++I], &Value))
        return false;
      Opt->PssSampleInterval = static_cast<uint32_t>(Value);
    } else if (!strcmp(argv[I], "--live-ready-file") && I + 1 < argc) {
      Opt->LiveReadyFile = argv[++I];
    } else {
      return false;
    }
  }
  return Opt->TracePath != nullptr;
}

uint64_t parseFieldU64(const char *Text) {
  if (!Text || !Text[0])
    return 0;
  return strtoull(Text, nullptr, 0);
}

void touchAllocated(void *Ptr, size_t Size, TouchMode Mode, uint32_t DelayUs,
                    Counters *Stats) {
  if (!Ptr || Size == 0 || Mode == TouchMode::None)
    return;
  if (DelayUs)
    usleep(DelayUs);
  volatile unsigned char *Bytes = static_cast<volatile unsigned char *>(Ptr);
  if (Mode == TouchMode::Full) {
    memset(Ptr, 0xa5, Size);
    Stats->BytesTouched += Size;
    return;
  }
  const size_t Page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  for (size_t Off = 0; Off < Size; Off += Page) {
    Bytes[Off] = static_cast<unsigned char>(Bytes[Off] + 1);
    Stats->BytesTouched++;
  }
}

void timedTouchAllocated(void *Ptr, size_t Size, TouchMode Mode,
                         uint32_t DelayUs, Counters *Stats) {
  if (!Ptr || Size == 0 || Mode == TouchMode::None)
    return;
  const uint64_t Start = nowNs();
  touchAllocated(Ptr, Size, Mode, DelayUs, Stats);
  Stats->TouchNs += nowNs() - Start;
  Stats->TouchOps++;
}

void remember(std::unordered_map<uint64_t, Allocation> *Live, uint64_t Key,
              void *Ptr, size_t Size, Counters *Stats) {
  if (!Ptr)
    return;
  (*Live)[Key] = {Ptr, Size};
  Stats->Live++;
  if (Stats->Live > Stats->PeakLive)
    Stats->PeakLive = Stats->Live;
}

void forget(std::unordered_map<uint64_t, Allocation> *Live, uint64_t Key,
            Counters *Stats) {
  auto It = Live->find(Key);
  if (It == Live->end()) {
    Stats->MissingFree++;
    return;
  }
  const uint64_t FreeStart = nowNs();
  free(It->second.Ptr);
  Stats->FreeNs += nowNs() - FreeStart;
  Live->erase(It);
  if (Stats->Live)
    Stats->Live--;
}

bool replayLine(char *Line, const Options &Opt,
                std::unordered_map<uint64_t, Allocation> *Live,
                Counters *Stats) {
  char *Fields[13] = {};
  char *Save = nullptr;
  for (int I = 0; I < 13; ++I) {
    Fields[I] = strtok_r(I == 0 ? Line : nullptr, ",\n\r", &Save);
    if (!Fields[I])
      return false;
  }

  const char *Op = Fields[4];
  const uint64_t PtrKey = parseFieldU64(Fields[5]);
  const uint64_t OldPtrKey = parseFieldU64(Fields[6]);
  const size_t ReqSize = static_cast<size_t>(parseFieldU64(Fields[7]));
  const size_t Nmemb = static_cast<size_t>(parseFieldU64(Fields[8]));
  size_t Alignment = static_cast<size_t>(parseFieldU64(Fields[9]));
  const size_t UsableSize = static_cast<size_t>(parseFieldU64(Fields[11]));
  const size_t TouchSize = UsableSize ? UsableSize : ReqSize;

  Stats->Events++;
  if (Opt.PssSampleInterval && Stats->Events % Opt.PssSampleInterval == 0)
    captureSelfSmapsRollup(Stats);
  if (!strcmp(Op, "malloc")) {
    Stats->Malloc++;
    Stats->BytesRequested += ReqSize;
    const uint64_t AllocStart = nowNs();
    void *Ptr = malloc(ReqSize);
    const uint64_t AllocDelta = nowNs() - AllocStart;
    Stats->AllocNs += AllocDelta;
    Stats->MallocNs += AllocDelta;
    if (!Ptr)
      Stats->AllocFailures++;
    remember(Live, PtrKey, Ptr, ReqSize, Stats);
    timedTouchAllocated(Ptr, TouchSize, Opt.Touch, Opt.TouchDelayUs, Stats);
  } else if (!strcmp(Op, "calloc")) {
    Stats->Calloc++;
    const size_t Count = Nmemb ? Nmemb : 1;
    const size_t Size = Count ? ReqSize / Count : ReqSize;
    Stats->BytesRequested += ReqSize;
    const uint64_t AllocStart = nowNs();
    void *Ptr = calloc(Count, Size);
    const uint64_t AllocDelta = nowNs() - AllocStart;
    Stats->AllocNs += AllocDelta;
    Stats->CallocNs += AllocDelta;
    if (!Ptr)
      Stats->AllocFailures++;
    remember(Live, PtrKey, Ptr, ReqSize, Stats);
    timedTouchAllocated(Ptr, TouchSize, Opt.Touch, Opt.TouchDelayUs, Stats);
  } else if (!strcmp(Op, "free")) {
    Stats->Free++;
    forget(Live, PtrKey, Stats);
  } else if (!strcmp(Op, "realloc")) {
    Stats->Realloc++;
    Stats->BytesRequested += ReqSize;
    void *OldPtr = nullptr;
    if (OldPtrKey) {
      auto It = Live->find(OldPtrKey);
      if (It != Live->end()) {
        OldPtr = It->second.Ptr;
        Live->erase(It);
        if (Stats->Live)
          Stats->Live--;
      } else {
        Stats->MissingFree++;
      }
    }
    const uint64_t AllocStart = nowNs();
    void *Ptr = realloc(OldPtr, ReqSize);
    const uint64_t AllocDelta = nowNs() - AllocStart;
    Stats->AllocNs += AllocDelta;
    Stats->ReallocNs += AllocDelta;
    if (!Ptr && ReqSize)
      Stats->AllocFailures++;
    remember(Live, PtrKey, Ptr, ReqSize, Stats);
    timedTouchAllocated(Ptr, TouchSize, Opt.Touch, Opt.TouchDelayUs, Stats);
  } else if (!strcmp(Op, "memalign") || !strcmp(Op, "posix_memalign") ||
             !strcmp(Op, "valloc") || !strcmp(Op, "pvalloc") ||
             !strcmp(Op, "aligned_alloc")) {
    Stats->Aligned++;
    Stats->BytesRequested += ReqSize;
    if (Alignment == 0)
      Alignment = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void *Ptr = nullptr;
    const uint64_t AllocStart = nowNs();
    if (!strcmp(Op, "aligned_alloc")) {
      const size_t Rounded = ((ReqSize + Alignment - 1) / Alignment) * Alignment;
      Ptr = aligned_alloc(Alignment, Rounded);
    } else {
      if (posix_memalign(&Ptr, Alignment, ReqSize) != 0)
        Ptr = nullptr;
    }
    const uint64_t AllocDelta = nowNs() - AllocStart;
    Stats->AllocNs += AllocDelta;
    Stats->AlignedNs += AllocDelta;
    if (!Ptr)
      Stats->AllocFailures++;
    remember(Live, PtrKey, Ptr, ReqSize, Stats);
    timedTouchAllocated(Ptr, TouchSize, Opt.Touch, Opt.TouchDelayUs, Stats);
  }
  return true;
}

bool replayOnce(const Options &Opt, Counters *Stats) {
  FILE *Fp = fopen(Opt.TracePath, "r");
  if (!Fp) {
    perror("fopen trace");
    return false;
  }

  std::unordered_map<uint64_t, Allocation> Live;
  Live.reserve(16384);
  char *Line = nullptr;
  size_t Cap = 0;
  bool HeaderSkipped = false;
  while (getline(&Line, &Cap, Fp) > 0) {
    if (!HeaderSkipped) {
      HeaderSkipped = true;
      if (!strncmp(Line, "seq,", 4))
        continue;
    }
    if (Opt.MaxEvents && Stats->Events >= Opt.MaxEvents)
      break;
    replayLine(Line, Opt, &Live, Stats);
  }
  free(Line);
  fclose(Fp);

  captureArenaStats(Stats);
  captureSelfSmapsRollup(Stats);
  writeLiveReadyFile(Opt.LiveReadyFile);
  if (Opt.HoldLiveMs) {
    const uint64_t HoldStart = nowNs();
    usleep(static_cast<useconds_t>(Opt.HoldLiveMs) * 1000u);
    Stats->HoldNs += nowNs() - HoldStart;
  }
  for (auto &Entry : Live) {
    const uint64_t FreeStart = nowNs();
    free(Entry.second.Ptr);
    Stats->CleanupFreeNs += nowNs() - FreeStart;
  }
  Stats->Live = 0;
  return true;
}

} // namespace

int main(int argc, char **argv) {
  Options Opt;
  if (!parseOptions(argc, argv, &Opt)) {
    usage(argv[0]);
    return 2;
  }

  Counters Stats;
  const uint64_t Start = nowNs();
  bool Ok = true;
  for (uint32_t I = 0; I < Opt.Repeat; ++I)
    Ok = replayOnce(Opt, &Stats) && Ok;
  const uint64_t End = nowNs();
  const uint64_t WallNs = End - Start;
  const uint64_t ElapsedNs = WallNs > Stats.HoldNs ? WallNs - Stats.HoldNs : 0;
  const double ElapsedMs = static_cast<double>(ElapsedNs) / 1000000.0;
  const double ElapsedS = static_cast<double>(ElapsedNs) / 1000000000.0;
  const double RequestedMb =
      static_cast<double>(Stats.BytesRequested) / (1024.0 * 1024.0);
  const double TouchedMb =
      static_cast<double>(Stats.BytesTouched) / (1024.0 * 1024.0);
  const long RssKb = maxRssKb();

  const uint64_t AllocOps = Stats.Malloc + Stats.Calloc + Stats.Realloc +
                            Stats.Aligned;
  printf("REPLAY_SUMMARY events=%" PRIu64 " malloc=%" PRIu64
         " calloc=%" PRIu64 " free=%" PRIu64 " realloc=%" PRIu64
         " aligned=%" PRIu64 " missing_free=%" PRIu64
         " alloc_failures=%" PRIu64 " live=%" PRIu64 " peak_live=%" PRIu64
         " requested_mb=%.3f touched_mb=%.3f elapsed_ms=%.3f"
         " events_per_s=%.3f requested_mb_per_s=%.3f"
         " touched_mb_per_s=%.3f maxrss_mb=%.3f"
         " delegated_arena_in_use_mb=%.3f"
         " delegated_arena_free_reusable_mb=%.3f"
         " delegated_arena_ready=%" PRIu32
         " arena_alloc_attempts=%" PRIu64
         " arena_alloc_success=%" PRIu64
         " arena_alloc_fallback=%" PRIu64
         " replay_live_pss_mb=%.3f replay_live_rss_mb=%.3f"
         " replay_peak_pss_mb=%.3f"
         " delegated_arena_in_use_at_peak_pss_mb=%.3f"
         " bytes_requested=%" PRIu64 " bytes_touched=%" PRIu64
         " elapsed_ns=%" PRIu64 " maxrss_kb=%ld"
         " alloc_ns=%" PRIu64 " free_ns=%" PRIu64
         " cleanup_free_ns=%" PRIu64 " touch_ns=%" PRIu64
         " alloc_avg_ns=%.3f free_avg_ns=%.3f touch_avg_ns=%.3f"
         " malloc_ns=%" PRIu64 " calloc_ns=%" PRIu64
         " realloc_ns=%" PRIu64 " aligned_ns=%" PRIu64
         " arena_profile_enabled=%" PRIu32
         " arena_should_use_calls=%" PRIu64 " arena_should_use_ns=%" PRIu64
         " arena_get_current_calls=%" PRIu64 " arena_get_current_ns=%" PRIu64
         " arena_retrieve_lock_wait_calls=%" PRIu64
         " arena_retrieve_lock_wait_ns=%" PRIu64
         " arena_freelist_scan_calls=%" PRIu64
         " arena_freelist_scan_ns=%" PRIu64
         " arena_bump_calls=%" PRIu64 " arena_bump_ns=%" PRIu64
         " arena_append_log_calls=%" PRIu64
         " arena_append_log_ns=%" PRIu64
         " arena_store_insert_merge_calls=%" PRIu64
         " arena_store_insert_merge_ns=%" PRIu64
         " arena_header_setup_calls=%" PRIu64
         " arena_header_setup_ns=%" PRIu64
         " arena_inuse_push_calls=%" PRIu64
         " arena_inuse_push_ns=%" PRIu64
         " arena_inuse_remove_calls=%" PRIu64
         " arena_inuse_remove_ns=%" PRIu64
         " arena_return_outer_calls=%" PRIu64
         " arena_return_outer_ns=%" PRIu64
         " arena_remote_store_calls=%" PRIu64
         " arena_remote_store_ns=%" PRIu64
         " arena_remote_enqueue_calls=%" PRIu64
         " arena_remote_enqueue_ns=%" PRIu64
         " arena_remote_drain_calls=%" PRIu64
         " arena_remote_drain_ns=%" PRIu64 "\n",
         Stats.Events, Stats.Malloc, Stats.Calloc, Stats.Free, Stats.Realloc,
         Stats.Aligned, Stats.MissingFree, Stats.AllocFailures, Stats.Live,
         Stats.PeakLive, RequestedMb, TouchedMb, ElapsedMs,
         ElapsedS > 0 ? static_cast<double>(Stats.Events) / ElapsedS : 0.0,
         ElapsedS > 0 ? RequestedMb / ElapsedS : 0.0,
         ElapsedS > 0 ? TouchedMb / ElapsedS : 0.0,
         static_cast<double>(RssKb) / 1024.0,
         bytesToMb(Stats.DelegatedArenaInUseBytes),
         bytesToMb(Stats.DelegatedArenaFreeReusableBytes),
         Stats.DelegatedArenaReady,
         Stats.ArenaAllocAttempts, Stats.ArenaAllocSuccess,
         Stats.ArenaAllocFallback,
         static_cast<double>(Stats.LiveSelfPssKb) / 1024.0,
         static_cast<double>(Stats.LiveSelfRssKb) / 1024.0,
         static_cast<double>(Stats.PeakSelfPssKb) / 1024.0,
         bytesToMb(Stats.PeakPssDelegatedArenaInUseBytes),
         Stats.BytesRequested, Stats.BytesTouched, ElapsedNs, RssKb,
         Stats.AllocNs, Stats.FreeNs, Stats.CleanupFreeNs, Stats.TouchNs,
         AllocOps > 0 ? static_cast<double>(Stats.AllocNs) /
                            static_cast<double>(AllocOps)
                      : 0.0,
         Stats.Free > 0 ? static_cast<double>(Stats.FreeNs) /
                              static_cast<double>(Stats.Free)
                        : 0.0,
         Stats.TouchOps > 0 ? static_cast<double>(Stats.TouchNs) /
                                  static_cast<double>(Stats.TouchOps)
                            : 0.0,
         Stats.MallocNs, Stats.CallocNs, Stats.ReallocNs, Stats.AlignedNs,
         Stats.ArenaProfileEnabled,
         Stats.ArenaShouldUseCalls, Stats.ArenaShouldUseNs,
         Stats.ArenaGetCurrentCalls, Stats.ArenaGetCurrentNs,
         Stats.ArenaRetrieveLockWaitCalls, Stats.ArenaRetrieveLockWaitNs,
         Stats.ArenaFreelistScanCalls, Stats.ArenaFreelistScanNs,
         Stats.ArenaBumpCalls, Stats.ArenaBumpNs,
         Stats.ArenaAppendLogCalls, Stats.ArenaAppendLogNs,
         Stats.ArenaStoreInsertMergeCalls, Stats.ArenaStoreInsertMergeNs,
         Stats.ArenaHeaderSetupCalls, Stats.ArenaHeaderSetupNs,
         Stats.ArenaInUsePushCalls, Stats.ArenaInUsePushNs,
         Stats.ArenaInUseRemoveCalls, Stats.ArenaInUseRemoveNs,
         Stats.ArenaReturnOuterCalls, Stats.ArenaReturnOuterNs,
         Stats.ArenaRemoteStoreCalls, Stats.ArenaRemoteStoreNs,
         Stats.ArenaRemoteEnqueueCalls, Stats.ArenaRemoteEnqueueNs,
         Stats.ArenaRemoteDrainCalls, Stats.ArenaRemoteDrainNs);
  return Ok && Stats.AllocFailures == 0 ? 0 : 1;
}
