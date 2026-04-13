/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MEMORY_DELEGATION_H
#define _LINUX_MEMORY_DELEGATION_H

#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/errno.h>
#include <uapi/linux/memory_delegation.h>

struct task_struct;

#ifdef CONFIG_MEMORY_DELEGATION
int memory_delegation_arena_register(unsigned int cpu, unsigned int arena_id,
				     unsigned int nr_pages,
				     unsigned int owner_slots);
void memory_delegation_arena_unregister(unsigned int cpu, unsigned int arena_id);

int memory_delegation_submit_log(unsigned int cpu,
				 const struct md_shadow_log *log,
				 unsigned int nr_entries);
int memory_delegation_submit_log_and_sync(unsigned int cpu,
					  const struct md_shadow_log *log,
					  unsigned int nr_entries,
					  unsigned long arena_base);

void memory_delegation_on_context_switch(struct task_struct *prev,
					 struct task_struct *next);

/*
 * Runs in process context with mmap_write_lock(mm) held.
 * Scans chunk generations and revokes stale mappings via unmap.
 */
int memory_delegation_sync_mm(struct mm_struct *mm, unsigned int cpu,
			      unsigned int arena_id,
			      unsigned long arena_base);

/*
 * Arena fault policy:
 *  - return true: owner still current mm, caller may restore mapping
 *  - return false: owner changed or page is free, caller must SIGSEGV
 */
bool memory_delegation_fault_allowed(struct mm_struct *mm, unsigned int cpu,
				     unsigned int arena_id,
				     unsigned long arena_base,
				     unsigned long address);
#else
static inline int memory_delegation_arena_register(unsigned int cpu,
						   unsigned int arena_id,
						   unsigned int nr_pages,
						   unsigned int owner_slots)
{
	return -EOPNOTSUPP;
}

static inline void memory_delegation_arena_unregister(unsigned int cpu,
						      unsigned int arena_id)
{
}

static inline int memory_delegation_submit_log(unsigned int cpu,
					       const struct md_shadow_log *log,
					       unsigned int nr_entries)
{
	return -EOPNOTSUPP;
}

static inline int memory_delegation_submit_log_and_sync(unsigned int cpu,
							const struct md_shadow_log *log,
							unsigned int nr_entries,
							unsigned long arena_base)
{
	return -EOPNOTSUPP;
}

static inline void memory_delegation_on_context_switch(struct task_struct *prev,
						       struct task_struct *next)
{
}

static inline int memory_delegation_sync_mm(struct mm_struct *mm,
					    unsigned int cpu,
					    unsigned int arena_id,
					    unsigned long arena_base)
{
	return -EOPNOTSUPP;
}

static inline bool memory_delegation_fault_allowed(struct mm_struct *mm,
						   unsigned int cpu,
						   unsigned int arena_id,
						   unsigned long arena_base,
						   unsigned long address)
{
	return false;
}
#endif

#endif /* _LINUX_MEMORY_DELEGATION_H */
