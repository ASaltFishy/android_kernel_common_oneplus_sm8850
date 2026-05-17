/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_MEMORY_DELEGATION_DEBUGFS_H
#define _MM_MEMORY_DELEGATION_DEBUGFS_H

#include <linux/percpu.h>
#include <linux/types.h>

#define MD_CHUNK_PAGES_MIN 32
#define MD_CHUNK_PAGES_DEFAULT 256
#define MD_CHUNK_PAGES_MAX 512

#ifdef CONFIG_MEMORY_DELEGATION_DEBUGFS
struct md_switch_cycle_stats {
	u64 ctx_switch_calls;
	u64 ctx_switch_fast_calls;
	u64 ctx_switch_slow_calls;
	u64 ctx_switch_log_slow_calls;
	u64 ctx_switch_pte_slow_calls;
	u64 ctx_switch_cycles;
	u64 ctx_switch_fast_cycles;
	u64 ctx_switch_slow_cycles;
	u64 drain_calls;
	u64 drain_empty_calls;
	u64 drain_nonempty_calls;
	u64 drain_cycles;
	u64 drain_empty_cycles;
	u64 drain_nonempty_cycles;
	u64 drain_scan_cycles;
	u64 drain_lock_wait_cycles;
	u64 drain_lock_hold_cycles;
	u64 drain_apply_cycles;
	u64 drain_apply_alloc_cycles;
	u64 drain_apply_free_cycles;
	u64 drain_alloc_check_cycles;
	u64 drain_alloc_update_cycles;
	u64 drain_free_check_cycles;
	u64 drain_free_update_cycles;
	u64 drain_commit_cycles;
	u64 drain_entries;
	u64 drain_alloc_entries;
	u64 drain_free_entries;
	u64 drain_pages;
	u64 drain_alloc_pages;
	u64 drain_free_pages;
	u64 pte_sync_calls;
	u64 pte_sync_unchanged_calls;
	u64 pte_sync_modified_calls;
	u64 pte_sync_cycles;
	u64 pte_sync_unchanged_cycles;
	u64 pte_sync_modified_cycles;
	u64 pte_sync_lock_wait_cycles;
	u64 pte_sync_lock_hold_cycles;
	u64 pte_sync_scan_cycles;
	u64 pte_sync_snapshot_cycles;
	u64 pte_sync_snapshot_lock_wait_cycles;
	u64 pte_sync_snapshot_lock_hold_cycles;
	u64 pte_sync_snapshot_loop_cycles;
	u64 pte_sync_unmap_cycles;
	u64 pte_sync_prefault_cycles;
	u64 pte_sync_dirty_chunks;
	u64 pte_sync_skipped_chunks;
	u64 pte_sync_unmap_ranges;
	u64 pte_sync_unmap_pages;
	u64 pte_sync_prefault_ranges;
	u64 pte_sync_prefault_pages;
	u64 pte_sync_delta_runs;
	u64 pte_sync_delta_revoke_runs;
	u64 pte_sync_delta_prefault_runs;
	u64 pte_sync_delta_fallbacks;
	u64 pte_sync_delta_lost_fallbacks;
	u64 fork_calls;
	u64 fork_cycles;
	u64 max_ctx_switch_cycles;
	u64 max_ctx_switch_fast_cycles;
	u64 max_ctx_switch_slow_cycles;
	u64 max_drain_cycles;
	u64 max_drain_empty_cycles;
	u64 max_drain_nonempty_cycles;
	u64 max_drain_scan_cycles;
	u64 max_drain_lock_wait_cycles;
	u64 max_drain_lock_hold_cycles;
	u64 max_drain_apply_cycles;
	u64 max_drain_apply_alloc_cycles;
	u64 max_drain_apply_free_cycles;
	u64 max_drain_alloc_check_cycles;
	u64 max_drain_alloc_update_cycles;
	u64 max_drain_free_check_cycles;
	u64 max_drain_free_update_cycles;
	u64 max_drain_commit_cycles;
	u64 max_pte_sync_cycles;
	u64 max_pte_sync_unchanged_cycles;
	u64 max_pte_sync_modified_cycles;
	u64 max_pte_sync_lock_wait_cycles;
	u64 max_pte_sync_lock_hold_cycles;
	u64 max_pte_sync_scan_cycles;
	u64 max_pte_sync_snapshot_cycles;
	u64 max_pte_sync_snapshot_lock_wait_cycles;
	u64 max_pte_sync_snapshot_lock_hold_cycles;
	u64 max_pte_sync_snapshot_loop_cycles;
	u64 max_pte_sync_unmap_cycles;
	u64 max_pte_sync_prefault_cycles;
	u64 max_fork_cycles;
};

struct md_pte_sync_detail {
	u64 lock_wait_cycles;
	u64 lock_hold_cycles;
	u64 scan_cycles;
	u64 snapshot_cycles;
	u64 snapshot_lock_wait_cycles;
	u64 snapshot_lock_hold_cycles;
	u64 snapshot_loop_cycles;
	u64 unmap_cycles;
	u64 prefault_cycles;
	u64 dirty_chunks;
	u64 skipped_chunks;
	u64 unmap_ranges;
	u64 unmap_pages;
	u64 prefault_ranges;
	u64 prefault_pages;
	u64 delta_runs;
	u64 delta_revoke_runs;
	u64 delta_prefault_runs;
	u64 delta_fallbacks;
	u64 delta_lost_fallbacks;
};

DECLARE_PER_CPU(struct md_switch_cycle_stats, md_switch_cycle_stats);
extern bool md_switch_cycle_stats_enabled;

u64 md_read_cycles(void);
void md_stats_add_max(u64 *max, u64 value);
void md_pte_sync_detail_add(struct md_pte_sync_detail *dst,
				    const struct md_pte_sync_detail *src);
void md_pte_sync_stats_add(struct md_switch_cycle_stats *stats,
				   const struct md_pte_sync_detail *detail);
struct md_debug_timer {
	const char *what;
	u64 start;
	u64 elapsed;
	bool running;
};

#define MD_TIME_START(_timer, _what) \
	struct md_debug_timer _timer = { \
		.what = (_what), \
		.start = md_read_cycles(), \
		.running = true, \
	}
#define MD_TIME_PAUSE(_timer) \
	do { \
		if ((_timer).running) { \
			(_timer).elapsed += md_read_cycles() - (_timer).start; \
			(_timer).running = false; \
		} \
	} while (0)
#define MD_TIME_RESUME(_timer) \
	do { \
		if (!(_timer).running) { \
			(_timer).start = md_read_cycles(); \
			(_timer).running = true; \
		} \
	} while (0)
#define MD_TIME_END(_timer) \
	({ \
		if ((_timer).running) \
			(_timer).elapsed += md_read_cycles() - (_timer).start; \
		(_timer).running = false; \
		(_timer).elapsed; \
	})
#define MD_DEBUG_STATS_ENABLED() READ_ONCE(md_switch_cycle_stats_enabled)
#define MD_DEBUG_RAW_CPU_STATS() raw_cpu_ptr(&md_switch_cycle_stats)
#define MD_DEBUG_GET_CPU_STATS() get_cpu_ptr(&md_switch_cycle_stats)
#define MD_DEBUG_PUT_CPU_STATS() put_cpu_ptr(&md_switch_cycle_stats)
#define MD_STATS_INC(_stats, _field) \
	do { if (_stats) (_stats)->_field++; } while (0)
#define MD_STATS_ADD(_stats, _field, _value) \
	do { if (_stats) (_stats)->_field += (_value); } while (0)
#define MD_STATS_MAX(_stats, _field, _value) \
	do { if (_stats) md_stats_add_max(&(_stats)->_field, (_value)); } while (0)
#define MD_STATS_ADD_MAX(_stats, _field, _max_field, _value) \
	do { \
		if (_stats) { \
			(_stats)->_field += (_value); \
			md_stats_add_max(&(_stats)->_max_field, (_value)); \
		} \
	} while (0)
#define MD_DETAIL_ZERO(_detail) \
	do { if (_detail) memset((_detail), 0, sizeof(*(_detail))); } while (0)
#define MD_DETAIL_INC(_detail, _field) \
	do { if (_detail) (_detail)->_field++; } while (0)
#define MD_DETAIL_ADD(_detail, _field, _value) \
	do { if (_detail) (_detail)->_field += (_value); } while (0)

#else
struct md_debug_timer { int unused; };
struct md_switch_cycle_stats { int unused; };
struct md_pte_sync_detail { int unused; };
#define MD_TIME_START(_timer, _what) \
	struct md_debug_timer _timer __maybe_unused = { }
#define MD_TIME_PAUSE(_timer) do { } while (0)
#define MD_TIME_RESUME(_timer) do { } while (0)
#define MD_TIME_END(_timer) 0
static inline bool md_debug_stats_enabled(void) { return false; }
static inline struct md_switch_cycle_stats *md_debug_no_stats(void) { return NULL; }
static inline void md_pte_sync_detail_add(struct md_pte_sync_detail *dst,
					  const struct md_pte_sync_detail *src) { }
static inline void md_pte_sync_stats_add(struct md_switch_cycle_stats *stats,
					 const struct md_pte_sync_detail *detail) { }
#define MD_DEBUG_STATS_ENABLED() md_debug_stats_enabled()
#define MD_DEBUG_RAW_CPU_STATS() md_debug_no_stats()
#define MD_DEBUG_GET_CPU_STATS() md_debug_no_stats()
#define MD_DEBUG_PUT_CPU_STATS() do { } while (0)
#define MD_STATS_INC(_stats, _field) do { } while (0)
#define MD_STATS_ADD(_stats, _field, _value) do { } while (0)
#define MD_STATS_MAX(_stats, _field, _value) do { } while (0)
#define MD_STATS_ADD_MAX(_stats, _field, _max_field, _value) do { } while (0)
#define MD_DETAIL_ZERO(_detail) do { } while (0)
#define MD_DETAIL_INC(_detail, _field) do { } while (0)
#define MD_DETAIL_ADD(_detail, _field, _value) do { } while (0)
#endif /* CONFIG_MEMORY_DELEGATION_DEBUGFS */

unsigned int md_debugfs_current_chunk_pages(void);
int md_debugfs_set_chunk_pages(unsigned int value);
int md_debugfs_active_arenas(void);
int md_debugfs_active_mms(void);

#endif /* _MM_MEMORY_DELEGATION_DEBUGFS_H */
