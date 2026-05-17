// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/timex.h>

#include "memory_delegation_debugfs.h"


DEFINE_PER_CPU(struct md_switch_cycle_stats, md_switch_cycle_stats);
bool md_switch_cycle_stats_enabled;
static struct dentry *md_debugfs_root;

u64 md_read_cycles(void)
{
	return get_cycles();
}

void md_stats_add_max(u64 *max, u64 value)
{
	if (value > *max)
		*max = value;
}

static __always_inline u64 md_stats_avg(u64 cycles, u64 calls)
{
	return calls ? div64_u64(cycles, calls) : 0;
}

void md_pte_sync_detail_add(struct md_pte_sync_detail *dst,
				   const struct md_pte_sync_detail *src)
{
	dst->lock_wait_cycles += src->lock_wait_cycles;
	dst->lock_hold_cycles += src->lock_hold_cycles;
	dst->scan_cycles += src->scan_cycles;
	dst->snapshot_cycles += src->snapshot_cycles;
	dst->snapshot_lock_wait_cycles += src->snapshot_lock_wait_cycles;
	dst->snapshot_lock_hold_cycles += src->snapshot_lock_hold_cycles;
	dst->snapshot_loop_cycles += src->snapshot_loop_cycles;
	dst->unmap_cycles += src->unmap_cycles;
	dst->prefault_cycles += src->prefault_cycles;
	dst->dirty_chunks += src->dirty_chunks;
	dst->skipped_chunks += src->skipped_chunks;
	dst->unmap_ranges += src->unmap_ranges;
	dst->unmap_pages += src->unmap_pages;
	dst->prefault_ranges += src->prefault_ranges;
	dst->prefault_pages += src->prefault_pages;
	dst->delta_runs += src->delta_runs;
	dst->delta_revoke_runs += src->delta_revoke_runs;
	dst->delta_prefault_runs += src->delta_prefault_runs;
	dst->delta_fallbacks += src->delta_fallbacks;
	dst->delta_lost_fallbacks += src->delta_lost_fallbacks;
}

void md_pte_sync_stats_add(struct md_switch_cycle_stats *stats,
				  const struct md_pte_sync_detail *detail)
{
	stats->pte_sync_lock_wait_cycles += detail->lock_wait_cycles;
	stats->pte_sync_lock_hold_cycles += detail->lock_hold_cycles;
	stats->pte_sync_scan_cycles += detail->scan_cycles;
	stats->pte_sync_snapshot_cycles += detail->snapshot_cycles;
	stats->pte_sync_snapshot_lock_wait_cycles +=
		detail->snapshot_lock_wait_cycles;
	stats->pte_sync_snapshot_lock_hold_cycles +=
		detail->snapshot_lock_hold_cycles;
	stats->pte_sync_snapshot_loop_cycles += detail->snapshot_loop_cycles;
	stats->pte_sync_unmap_cycles += detail->unmap_cycles;
	stats->pte_sync_prefault_cycles += detail->prefault_cycles;
	stats->pte_sync_dirty_chunks += detail->dirty_chunks;
	stats->pte_sync_skipped_chunks += detail->skipped_chunks;
	stats->pte_sync_unmap_ranges += detail->unmap_ranges;
	stats->pte_sync_unmap_pages += detail->unmap_pages;
	stats->pte_sync_prefault_ranges += detail->prefault_ranges;
	stats->pte_sync_prefault_pages += detail->prefault_pages;
	stats->pte_sync_delta_runs += detail->delta_runs;
	stats->pte_sync_delta_revoke_runs += detail->delta_revoke_runs;
	stats->pte_sync_delta_prefault_runs += detail->delta_prefault_runs;
	stats->pte_sync_delta_fallbacks += detail->delta_fallbacks;
	stats->pte_sync_delta_lost_fallbacks += detail->delta_lost_fallbacks;
	md_stats_add_max(&stats->max_pte_sync_lock_wait_cycles,
			 detail->lock_wait_cycles);
	md_stats_add_max(&stats->max_pte_sync_lock_hold_cycles,
			 detail->lock_hold_cycles);
	md_stats_add_max(&stats->max_pte_sync_scan_cycles,
			 detail->scan_cycles);
	md_stats_add_max(&stats->max_pte_sync_snapshot_cycles,
			 detail->snapshot_cycles);
	md_stats_add_max(&stats->max_pte_sync_snapshot_lock_wait_cycles,
			 detail->snapshot_lock_wait_cycles);
	md_stats_add_max(&stats->max_pte_sync_snapshot_lock_hold_cycles,
			 detail->snapshot_lock_hold_cycles);
	md_stats_add_max(&stats->max_pte_sync_snapshot_loop_cycles,
			 detail->snapshot_loop_cycles);
	md_stats_add_max(&stats->max_pte_sync_unmap_cycles,
			 detail->unmap_cycles);
	md_stats_add_max(&stats->max_pte_sync_prefault_cycles,
			 detail->prefault_cycles);
}

static void md_switch_cycle_stats_reset(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		memset(per_cpu_ptr(&md_switch_cycle_stats, cpu), 0,
		       sizeof(struct md_switch_cycle_stats));
}

static int md_switch_cycle_stats_show(struct seq_file *m, void *unused)
{
	struct md_switch_cycle_stats sum = {};
	int cpu;

	seq_printf(m, "enabled %u\n", READ_ONCE(md_switch_cycle_stats_enabled));
	seq_puts(m, "unit cycles\n");
	seq_puts(m, "clock arm64_arch_timer_cntvct\n");
	seq_printf(m, "chunk_pages %u\n", md_debugfs_current_chunk_pages());
	seq_puts(m, "sample ctx_switch requires prev->mm and next->mm both active arena mms\n");
	seq_puts(m, "columns cpu ctx_switch ctx_switch_fast ctx_switch_slow ctx_switch_log_slow ctx_switch_pte_slow drain drain_empty drain_nonempty pte_sync pte_sync_unchanged pte_sync_modified fork avg_ctx_switch avg_ctx_switch_fast avg_ctx_switch_slow avg_drain avg_drain_empty avg_drain_nonempty avg_pte_sync avg_pte_sync_unchanged avg_pte_sync_modified avg_fork max_ctx_switch max_ctx_switch_fast max_ctx_switch_slow max_drain max_drain_empty max_drain_nonempty max_pte_sync max_pte_sync_unchanged max_pte_sync_modified max_fork\n");
	seq_puts(m, "drain_detail_columns cpu entries alloc_entries free_entries pages alloc_pages free_pages avg_scan avg_lock_wait avg_lock_hold avg_apply avg_apply_alloc avg_apply_free avg_commit max_scan max_lock_wait max_lock_hold max_apply max_apply_alloc max_apply_free max_commit\n");
	seq_puts(m, "apply_detail_columns cpu avg_alloc_check avg_alloc_update avg_free_check avg_free_update max_alloc_check max_alloc_update max_free_check max_free_update\n");
	seq_puts(m, "pte_detail_columns cpu dirty_chunks skipped_chunks unmap_ranges unmap_pages prefault_ranges prefault_pages delta_runs delta_revoke_runs delta_prefault_runs delta_fallbacks delta_lost_fallbacks avg_lock_wait avg_lock_hold avg_scan avg_snapshot avg_unmap avg_prefault max_lock_wait max_lock_hold max_scan max_snapshot max_unmap max_prefault\n");
	seq_puts(m, "snapshot_detail_columns cpu avg_lock_wait avg_lock_hold avg_loop max_lock_wait max_lock_hold max_loop\n");

	for_each_possible_cpu(cpu) {
		const struct md_switch_cycle_stats *s =
			per_cpu_ptr(&md_switch_cycle_stats, cpu);

		if (!s->ctx_switch_calls && !s->drain_calls &&
		    !s->pte_sync_calls && !s->fork_calls)
			continue;

		seq_printf(m,
			   "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
			   cpu, s->ctx_switch_calls, s->ctx_switch_fast_calls,
			   s->ctx_switch_slow_calls,
			   s->ctx_switch_log_slow_calls,
			   s->ctx_switch_pte_slow_calls, s->drain_calls,
			   s->drain_empty_calls, s->drain_nonempty_calls,
			   s->pte_sync_calls, s->pte_sync_unchanged_calls,
			   s->pte_sync_modified_calls, s->fork_calls,
			   md_stats_avg(s->ctx_switch_cycles, s->ctx_switch_calls),
			   md_stats_avg(s->ctx_switch_fast_cycles,
					s->ctx_switch_fast_calls),
			   md_stats_avg(s->ctx_switch_slow_cycles,
					s->ctx_switch_slow_calls),
			   md_stats_avg(s->drain_cycles, s->drain_calls),
			   md_stats_avg(s->drain_empty_cycles,
					s->drain_empty_calls),
			   md_stats_avg(s->drain_nonempty_cycles,
					s->drain_nonempty_calls),
			   md_stats_avg(s->pte_sync_cycles, s->pte_sync_calls),
			   md_stats_avg(s->pte_sync_unchanged_cycles,
					s->pte_sync_unchanged_calls),
			   md_stats_avg(s->pte_sync_modified_cycles,
					s->pte_sync_modified_calls),
			   md_stats_avg(s->fork_cycles, s->fork_calls),
			   s->max_ctx_switch_cycles,
			   s->max_ctx_switch_fast_cycles,
			   s->max_ctx_switch_slow_cycles, s->max_drain_cycles,
			   s->max_drain_empty_cycles,
			   s->max_drain_nonempty_cycles,
			   s->max_pte_sync_cycles,
			   s->max_pte_sync_unchanged_cycles,
			   s->max_pte_sync_modified_cycles,
			   s->max_fork_cycles);
		seq_printf(m,
			   "drain_detail_cpu%u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
			   cpu, s->drain_entries, s->drain_alloc_entries,
			   s->drain_free_entries, s->drain_pages,
			   s->drain_alloc_pages, s->drain_free_pages,
			   md_stats_avg(s->drain_scan_cycles,
					s->drain_nonempty_calls),
			   md_stats_avg(s->drain_lock_wait_cycles,
					s->drain_nonempty_calls),
			   md_stats_avg(s->drain_lock_hold_cycles,
					s->drain_nonempty_calls),
			   md_stats_avg(s->drain_apply_cycles,
					s->drain_entries),
			   md_stats_avg(s->drain_apply_alloc_cycles,
					s->drain_alloc_entries),
			   md_stats_avg(s->drain_apply_free_cycles,
					s->drain_free_entries),
			   md_stats_avg(s->drain_commit_cycles,
					s->drain_nonempty_calls),
			   s->max_drain_scan_cycles,
			   s->max_drain_lock_wait_cycles,
			   s->max_drain_lock_hold_cycles,
			   s->max_drain_apply_cycles,
			   s->max_drain_apply_alloc_cycles,
			   s->max_drain_apply_free_cycles,
			   s->max_drain_commit_cycles);
		seq_printf(m,
			   "apply_detail_cpu%u %llu %llu %llu %llu %llu %llu %llu %llu\n",
			   cpu,
			   md_stats_avg(s->drain_alloc_check_cycles,
					s->drain_alloc_entries),
			   md_stats_avg(s->drain_alloc_update_cycles,
					s->drain_alloc_entries),
			   md_stats_avg(s->drain_free_check_cycles,
					s->drain_free_entries),
			   md_stats_avg(s->drain_free_update_cycles,
					s->drain_free_entries),
			   s->max_drain_alloc_check_cycles,
			   s->max_drain_alloc_update_cycles,
			   s->max_drain_free_check_cycles,
			   s->max_drain_free_update_cycles);
		seq_printf(m,
			   "pte_detail_cpu%u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
			   cpu, s->pte_sync_dirty_chunks,
			   s->pte_sync_skipped_chunks, s->pte_sync_unmap_ranges,
			   s->pte_sync_unmap_pages, s->pte_sync_prefault_ranges,
			   s->pte_sync_prefault_pages,
			   s->pte_sync_delta_runs,
			   s->pte_sync_delta_revoke_runs,
			   s->pte_sync_delta_prefault_runs,
			   s->pte_sync_delta_fallbacks,
			   s->pte_sync_delta_lost_fallbacks,
			   md_stats_avg(s->pte_sync_lock_wait_cycles,
					s->pte_sync_calls),
			   md_stats_avg(s->pte_sync_lock_hold_cycles,
					s->pte_sync_calls),
			   md_stats_avg(s->pte_sync_scan_cycles,
					s->pte_sync_calls),
			   md_stats_avg(s->pte_sync_snapshot_cycles,
					s->pte_sync_dirty_chunks +
					s->pte_sync_skipped_chunks),
			   md_stats_avg(s->pte_sync_unmap_cycles,
					s->pte_sync_unmap_ranges),
			   md_stats_avg(s->pte_sync_prefault_cycles,
					s->pte_sync_prefault_ranges),
			   s->max_pte_sync_lock_wait_cycles,
			   s->max_pte_sync_lock_hold_cycles,
			   s->max_pte_sync_scan_cycles,
			   s->max_pte_sync_snapshot_cycles,
			   s->max_pte_sync_unmap_cycles,
			   s->max_pte_sync_prefault_cycles);
		seq_printf(m,
			   "snapshot_detail_cpu%u %llu %llu %llu %llu %llu %llu\n",
			   cpu,
			   md_stats_avg(s->pte_sync_snapshot_lock_wait_cycles,
					s->pte_sync_dirty_chunks +
					s->pte_sync_skipped_chunks),
			   md_stats_avg(s->pte_sync_snapshot_lock_hold_cycles,
					s->pte_sync_dirty_chunks +
					s->pte_sync_skipped_chunks),
			   md_stats_avg(s->pte_sync_snapshot_loop_cycles,
					s->pte_sync_dirty_chunks),
			   s->max_pte_sync_snapshot_lock_wait_cycles,
			   s->max_pte_sync_snapshot_lock_hold_cycles,
			   s->max_pte_sync_snapshot_loop_cycles);

		sum.ctx_switch_calls += s->ctx_switch_calls;
		sum.ctx_switch_fast_calls += s->ctx_switch_fast_calls;
		sum.ctx_switch_slow_calls += s->ctx_switch_slow_calls;
		sum.ctx_switch_log_slow_calls += s->ctx_switch_log_slow_calls;
		sum.ctx_switch_pte_slow_calls += s->ctx_switch_pte_slow_calls;
		sum.ctx_switch_cycles += s->ctx_switch_cycles;
		sum.ctx_switch_fast_cycles += s->ctx_switch_fast_cycles;
		sum.ctx_switch_slow_cycles += s->ctx_switch_slow_cycles;
		sum.drain_calls += s->drain_calls;
		sum.drain_empty_calls += s->drain_empty_calls;
		sum.drain_nonempty_calls += s->drain_nonempty_calls;
		sum.drain_cycles += s->drain_cycles;
		sum.drain_empty_cycles += s->drain_empty_cycles;
		sum.drain_nonempty_cycles += s->drain_nonempty_cycles;
		sum.drain_scan_cycles += s->drain_scan_cycles;
		sum.drain_lock_wait_cycles += s->drain_lock_wait_cycles;
		sum.drain_lock_hold_cycles += s->drain_lock_hold_cycles;
		sum.drain_apply_cycles += s->drain_apply_cycles;
		sum.drain_apply_alloc_cycles += s->drain_apply_alloc_cycles;
		sum.drain_apply_free_cycles += s->drain_apply_free_cycles;
		sum.drain_alloc_check_cycles += s->drain_alloc_check_cycles;
		sum.drain_alloc_update_cycles += s->drain_alloc_update_cycles;
		sum.drain_free_check_cycles += s->drain_free_check_cycles;
		sum.drain_free_update_cycles += s->drain_free_update_cycles;
		sum.drain_commit_cycles += s->drain_commit_cycles;
		sum.drain_entries += s->drain_entries;
		sum.drain_alloc_entries += s->drain_alloc_entries;
		sum.drain_free_entries += s->drain_free_entries;
		sum.drain_pages += s->drain_pages;
		sum.drain_alloc_pages += s->drain_alloc_pages;
		sum.drain_free_pages += s->drain_free_pages;
		sum.pte_sync_calls += s->pte_sync_calls;
		sum.pte_sync_unchanged_calls += s->pte_sync_unchanged_calls;
		sum.pte_sync_modified_calls += s->pte_sync_modified_calls;
		sum.pte_sync_cycles += s->pte_sync_cycles;
		sum.pte_sync_unchanged_cycles += s->pte_sync_unchanged_cycles;
		sum.pte_sync_modified_cycles += s->pte_sync_modified_cycles;
		sum.pte_sync_lock_wait_cycles += s->pte_sync_lock_wait_cycles;
		sum.pte_sync_lock_hold_cycles += s->pte_sync_lock_hold_cycles;
		sum.pte_sync_scan_cycles += s->pte_sync_scan_cycles;
		sum.pte_sync_snapshot_cycles += s->pte_sync_snapshot_cycles;
		sum.pte_sync_snapshot_lock_wait_cycles +=
			s->pte_sync_snapshot_lock_wait_cycles;
		sum.pte_sync_snapshot_lock_hold_cycles +=
			s->pte_sync_snapshot_lock_hold_cycles;
		sum.pte_sync_snapshot_loop_cycles +=
			s->pte_sync_snapshot_loop_cycles;
		sum.pte_sync_unmap_cycles += s->pte_sync_unmap_cycles;
		sum.pte_sync_prefault_cycles += s->pte_sync_prefault_cycles;
		sum.pte_sync_dirty_chunks += s->pte_sync_dirty_chunks;
		sum.pte_sync_skipped_chunks += s->pte_sync_skipped_chunks;
		sum.pte_sync_unmap_ranges += s->pte_sync_unmap_ranges;
		sum.pte_sync_unmap_pages += s->pte_sync_unmap_pages;
		sum.pte_sync_prefault_ranges += s->pte_sync_prefault_ranges;
		sum.pte_sync_prefault_pages += s->pte_sync_prefault_pages;
		sum.pte_sync_delta_runs += s->pte_sync_delta_runs;
		sum.pte_sync_delta_revoke_runs += s->pte_sync_delta_revoke_runs;
		sum.pte_sync_delta_prefault_runs +=
			s->pte_sync_delta_prefault_runs;
		sum.pte_sync_delta_fallbacks += s->pte_sync_delta_fallbacks;
		sum.pte_sync_delta_lost_fallbacks +=
			s->pte_sync_delta_lost_fallbacks;
		sum.fork_calls += s->fork_calls;
		sum.fork_cycles += s->fork_cycles;
		md_stats_add_max(&sum.max_ctx_switch_cycles,
				 s->max_ctx_switch_cycles);
		md_stats_add_max(&sum.max_ctx_switch_fast_cycles,
				 s->max_ctx_switch_fast_cycles);
		md_stats_add_max(&sum.max_ctx_switch_slow_cycles,
				 s->max_ctx_switch_slow_cycles);
		md_stats_add_max(&sum.max_drain_cycles, s->max_drain_cycles);
		md_stats_add_max(&sum.max_drain_empty_cycles,
				 s->max_drain_empty_cycles);
		md_stats_add_max(&sum.max_drain_nonempty_cycles,
				 s->max_drain_nonempty_cycles);
		md_stats_add_max(&sum.max_drain_scan_cycles,
				 s->max_drain_scan_cycles);
		md_stats_add_max(&sum.max_drain_lock_wait_cycles,
				 s->max_drain_lock_wait_cycles);
		md_stats_add_max(&sum.max_drain_lock_hold_cycles,
				 s->max_drain_lock_hold_cycles);
		md_stats_add_max(&sum.max_drain_apply_cycles,
				 s->max_drain_apply_cycles);
		md_stats_add_max(&sum.max_drain_apply_alloc_cycles,
				 s->max_drain_apply_alloc_cycles);
		md_stats_add_max(&sum.max_drain_apply_free_cycles,
				 s->max_drain_apply_free_cycles);
		md_stats_add_max(&sum.max_drain_alloc_check_cycles,
				 s->max_drain_alloc_check_cycles);
		md_stats_add_max(&sum.max_drain_alloc_update_cycles,
				 s->max_drain_alloc_update_cycles);
		md_stats_add_max(&sum.max_drain_free_check_cycles,
				 s->max_drain_free_check_cycles);
		md_stats_add_max(&sum.max_drain_free_update_cycles,
				 s->max_drain_free_update_cycles);
		md_stats_add_max(&sum.max_drain_commit_cycles,
				 s->max_drain_commit_cycles);
		md_stats_add_max(&sum.max_pte_sync_cycles, s->max_pte_sync_cycles);
		md_stats_add_max(&sum.max_pte_sync_unchanged_cycles,
				 s->max_pte_sync_unchanged_cycles);
		md_stats_add_max(&sum.max_pte_sync_modified_cycles,
				 s->max_pte_sync_modified_cycles);
		md_stats_add_max(&sum.max_pte_sync_lock_wait_cycles,
				 s->max_pte_sync_lock_wait_cycles);
		md_stats_add_max(&sum.max_pte_sync_lock_hold_cycles,
				 s->max_pte_sync_lock_hold_cycles);
		md_stats_add_max(&sum.max_pte_sync_scan_cycles,
				 s->max_pte_sync_scan_cycles);
		md_stats_add_max(&sum.max_pte_sync_snapshot_cycles,
				 s->max_pte_sync_snapshot_cycles);
		md_stats_add_max(&sum.max_pte_sync_snapshot_lock_wait_cycles,
				 s->max_pte_sync_snapshot_lock_wait_cycles);
		md_stats_add_max(&sum.max_pte_sync_snapshot_lock_hold_cycles,
				 s->max_pte_sync_snapshot_lock_hold_cycles);
		md_stats_add_max(&sum.max_pte_sync_snapshot_loop_cycles,
				 s->max_pte_sync_snapshot_loop_cycles);
		md_stats_add_max(&sum.max_pte_sync_unmap_cycles,
				 s->max_pte_sync_unmap_cycles);
		md_stats_add_max(&sum.max_pte_sync_prefault_cycles,
				 s->max_pte_sync_prefault_cycles);
		md_stats_add_max(&sum.max_fork_cycles, s->max_fork_cycles);
	}

	seq_printf(m,
		   "total %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
		   sum.ctx_switch_calls, sum.ctx_switch_fast_calls,
		   sum.ctx_switch_slow_calls, sum.ctx_switch_log_slow_calls,
		   sum.ctx_switch_pte_slow_calls, sum.drain_calls,
		   sum.drain_empty_calls, sum.drain_nonempty_calls,
		   sum.pte_sync_calls, sum.pte_sync_unchanged_calls,
		   sum.pte_sync_modified_calls, sum.fork_calls,
		   md_stats_avg(sum.ctx_switch_cycles, sum.ctx_switch_calls),
		   md_stats_avg(sum.ctx_switch_fast_cycles,
				sum.ctx_switch_fast_calls),
		   md_stats_avg(sum.ctx_switch_slow_cycles,
				sum.ctx_switch_slow_calls),
		   md_stats_avg(sum.drain_cycles, sum.drain_calls),
		   md_stats_avg(sum.drain_empty_cycles,
				sum.drain_empty_calls),
		   md_stats_avg(sum.drain_nonempty_cycles,
				sum.drain_nonempty_calls),
		   md_stats_avg(sum.pte_sync_cycles, sum.pte_sync_calls),
		   md_stats_avg(sum.pte_sync_unchanged_cycles,
				sum.pte_sync_unchanged_calls),
		   md_stats_avg(sum.pte_sync_modified_cycles,
				sum.pte_sync_modified_calls),
		   md_stats_avg(sum.fork_cycles, sum.fork_calls),
		   sum.max_ctx_switch_cycles, sum.max_ctx_switch_fast_cycles,
		   sum.max_ctx_switch_slow_cycles, sum.max_drain_cycles,
		   sum.max_drain_empty_cycles, sum.max_drain_nonempty_cycles,
		   sum.max_pte_sync_cycles, sum.max_pte_sync_unchanged_cycles,
		   sum.max_pte_sync_modified_cycles, sum.max_fork_cycles);
	seq_printf(m,
		   "drain_detail_total %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
		   sum.drain_entries, sum.drain_alloc_entries,
		   sum.drain_free_entries, sum.drain_pages,
		   sum.drain_alloc_pages, sum.drain_free_pages,
		   md_stats_avg(sum.drain_scan_cycles,
				sum.drain_nonempty_calls),
		   md_stats_avg(sum.drain_lock_wait_cycles,
				sum.drain_nonempty_calls),
		   md_stats_avg(sum.drain_lock_hold_cycles,
				sum.drain_nonempty_calls),
		   md_stats_avg(sum.drain_apply_cycles,
				sum.drain_entries),
		   md_stats_avg(sum.drain_apply_alloc_cycles,
				sum.drain_alloc_entries),
		   md_stats_avg(sum.drain_apply_free_cycles,
				sum.drain_free_entries),
		   md_stats_avg(sum.drain_commit_cycles,
				sum.drain_nonempty_calls),
		   sum.max_drain_scan_cycles, sum.max_drain_lock_wait_cycles,
		   sum.max_drain_lock_hold_cycles, sum.max_drain_apply_cycles,
		   sum.max_drain_apply_alloc_cycles,
		   sum.max_drain_apply_free_cycles,
		   sum.max_drain_commit_cycles);
	seq_printf(m,
		   "apply_detail_total %llu %llu %llu %llu %llu %llu %llu %llu\n",
		   md_stats_avg(sum.drain_alloc_check_cycles,
				sum.drain_alloc_entries),
		   md_stats_avg(sum.drain_alloc_update_cycles,
				sum.drain_alloc_entries),
		   md_stats_avg(sum.drain_free_check_cycles,
				sum.drain_free_entries),
		   md_stats_avg(sum.drain_free_update_cycles,
				sum.drain_free_entries),
		   sum.max_drain_alloc_check_cycles,
		   sum.max_drain_alloc_update_cycles,
		   sum.max_drain_free_check_cycles,
		   sum.max_drain_free_update_cycles);
	seq_printf(m,
		   "pte_detail_total %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
		   sum.pte_sync_dirty_chunks, sum.pte_sync_skipped_chunks,
		   sum.pte_sync_unmap_ranges, sum.pte_sync_unmap_pages,
		   sum.pte_sync_prefault_ranges, sum.pte_sync_prefault_pages,
		   sum.pte_sync_delta_runs, sum.pte_sync_delta_revoke_runs,
		   sum.pte_sync_delta_prefault_runs,
		   sum.pte_sync_delta_fallbacks,
		   sum.pte_sync_delta_lost_fallbacks,
		   md_stats_avg(sum.pte_sync_lock_wait_cycles,
				sum.pte_sync_calls),
		   md_stats_avg(sum.pte_sync_lock_hold_cycles,
				sum.pte_sync_calls),
		   md_stats_avg(sum.pte_sync_scan_cycles,
				sum.pte_sync_calls),
		   md_stats_avg(sum.pte_sync_snapshot_cycles,
				sum.pte_sync_dirty_chunks +
				sum.pte_sync_skipped_chunks),
		   md_stats_avg(sum.pte_sync_unmap_cycles,
				sum.pte_sync_unmap_ranges),
		   md_stats_avg(sum.pte_sync_prefault_cycles,
				sum.pte_sync_prefault_ranges),
		   sum.max_pte_sync_lock_wait_cycles,
		   sum.max_pte_sync_lock_hold_cycles,
		   sum.max_pte_sync_scan_cycles,
		   sum.max_pte_sync_snapshot_cycles,
		   sum.max_pte_sync_unmap_cycles,
		   sum.max_pte_sync_prefault_cycles);
	seq_printf(m,
		   "snapshot_detail_total %llu %llu %llu %llu %llu %llu\n",
		   md_stats_avg(sum.pte_sync_snapshot_lock_wait_cycles,
				sum.pte_sync_dirty_chunks +
				sum.pte_sync_skipped_chunks),
		   md_stats_avg(sum.pte_sync_snapshot_lock_hold_cycles,
				sum.pte_sync_dirty_chunks +
				sum.pte_sync_skipped_chunks),
		   md_stats_avg(sum.pte_sync_snapshot_loop_cycles,
				sum.pte_sync_dirty_chunks),
		   sum.max_pte_sync_snapshot_lock_wait_cycles,
		   sum.max_pte_sync_snapshot_lock_hold_cycles,
		   sum.max_pte_sync_snapshot_loop_cycles);
	return 0;
}

static int md_switch_cycle_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, md_switch_cycle_stats_show, inode->i_private);
}

static ssize_t md_switch_cycle_stats_write(struct file *file,
					   const char __user *buf,
					   size_t count, loff_t *ppos)
{
	char kbuf[16];
	size_t len = min(count, sizeof(kbuf) - 1);

	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';

	if (sysfs_streq(kbuf, "reset")) {
		md_switch_cycle_stats_reset();
		return count;
	}
	if (sysfs_streq(kbuf, "enable")) {
		WRITE_ONCE(md_switch_cycle_stats_enabled, true);
		return count;
	}
	if (sysfs_streq(kbuf, "disable")) {
		WRITE_ONCE(md_switch_cycle_stats_enabled, false);
		return count;
	}

	return -EINVAL;
}

static const struct file_operations md_switch_cycle_stats_fops = {
	.owner = THIS_MODULE,
	.open = md_switch_cycle_stats_open,
	.read = seq_read,
	.write = md_switch_cycle_stats_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int md_chunk_pages_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "%u\n", md_debugfs_current_chunk_pages());
	seq_printf(m, "min %u\n", MD_CHUNK_PAGES_MIN);
	seq_printf(m, "max %u\n", MD_CHUNK_PAGES_MAX);
	seq_printf(m, "active_arenas %d\n", md_debugfs_active_arenas());
	seq_printf(m, "active_mms %d\n", md_debugfs_active_mms());
	return 0;
}

static int md_chunk_pages_open(struct inode *inode, struct file *file)
{
	return single_open(file, md_chunk_pages_show, inode->i_private);
}

static ssize_t md_chunk_pages_write(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	char kbuf[32];
	unsigned int value;
	size_t len = min(count, sizeof(kbuf) - 1);
	int ret;

	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';

	ret = kstrtouint(strim(kbuf), 0, &value);
	if (ret)
		return ret;

	if (value < MD_CHUNK_PAGES_MIN || value > MD_CHUNK_PAGES_MAX ||
	    !is_power_of_2(value))
		return -EINVAL;

	ret = md_debugfs_set_chunk_pages(value);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations md_chunk_pages_fops = {
	.owner = THIS_MODULE,
	.open = md_chunk_pages_open,
	.read = seq_read,
	.write = md_chunk_pages_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init md_debugfs_init(void)
{
	md_debugfs_root = debugfs_create_dir("memory_delegation", NULL);
	debugfs_create_bool("switch_cycle_stats_enabled", 0600, md_debugfs_root,
			    &md_switch_cycle_stats_enabled);
	debugfs_create_file("switch_cycle_stats", 0600, md_debugfs_root, NULL,
			    &md_switch_cycle_stats_fops);
	debugfs_create_file("chunk_pages", 0600, md_debugfs_root, NULL,
			    &md_chunk_pages_fops);
	return 0;
}
late_initcall(md_debugfs_init);
