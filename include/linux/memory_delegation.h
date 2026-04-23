/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MEMORY_DELEGATION_H
#define _LINUX_MEMORY_DELEGATION_H

#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/errno.h>
#include <uapi/linux/memory_delegation.h>

struct task_struct;

#ifdef CONFIG_MEMORY_DELEGATION
int memory_delegation_arena_register(unsigned int cpu, unsigned int nr_pages,
				     unsigned int owner_slots);
void memory_delegation_arena_unregister(unsigned int cpu);

int memory_delegation_register_ring(unsigned int cpu, unsigned long arena_base,
				    unsigned long ring_addr);

int memory_delegation_submit_log(unsigned int cpu, unsigned long arena_base,
				 const struct md_shadow_log *log,
				 unsigned int nr_entries);

void memory_delegation_on_context_switch(struct task_struct *prev,
					 struct task_struct *next);
int memory_delegation_fork_mm(struct task_struct *task, struct mm_struct *new_mm,
			      struct mm_struct *old_mm);
void memory_delegation_mm_release(struct mm_struct *mm);

/*
 * Runs in process context with mmap_write_lock(mm) held.
 * Scans chunk generations and revokes stale mappings via unmap.
 */
int memory_delegation_sync_mm(struct mm_struct *mm, unsigned int cpu,
			      unsigned long arena_base);

/*
 * Arena fault policy:
 *  - return true: non-delegation faults or owner still current mm
 *  - return false: registered delegation page is no longer owned by current mm
 */
bool memory_delegation_fault_allowed(struct mm_struct *mm,
				     unsigned long address,
				     const struct vm_area_struct *vma);
#else
static inline int memory_delegation_arena_register(unsigned int cpu,
						   unsigned int nr_pages,
						   unsigned int owner_slots)
{
	return -EOPNOTSUPP;
}

static inline void memory_delegation_arena_unregister(unsigned int cpu)
{
}

static inline int memory_delegation_register_ring(unsigned int cpu,
						  unsigned long arena_base,
						  unsigned long ring_addr)
{
	return -EOPNOTSUPP;
}

static inline int memory_delegation_submit_log(unsigned int cpu,
					       unsigned long arena_base,
					       const struct md_shadow_log *log,
					       unsigned int nr_entries)
{
	return -EOPNOTSUPP;
}

static inline void memory_delegation_on_context_switch(struct task_struct *prev,
						       struct task_struct *next)
{
}

static inline int memory_delegation_fork_mm(struct task_struct *task,
					    struct mm_struct *new_mm,
					    struct mm_struct *old_mm)
{
	return 0;
}

static inline void memory_delegation_mm_release(struct mm_struct *mm)
{
}

static inline int memory_delegation_sync_mm(struct mm_struct *mm,
					    unsigned int cpu,
					    unsigned long arena_base)
{
	return -EOPNOTSUPP;
}

static inline bool memory_delegation_fault_allowed(struct mm_struct *mm,
						   unsigned long address,
						   const struct vm_area_struct *vma)
{
	return true;
}
#endif

#endif /* _LINUX_MEMORY_DELEGATION_H */
