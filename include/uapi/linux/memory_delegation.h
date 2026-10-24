/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_MEMORY_DELEGATION_H
#define _UAPI_LINUX_MEMORY_DELEGATION_H

#include <linux/types.h>

/*
 * Userspace shadow-log record consumed by kernel truth layer.
 */
enum md_log_op {
	MD_LOG_ALLOC = 1,
	MD_LOG_FREE = 2,
};

#define MD_LOG_F_PREFAULT	(1U << 0)

struct md_shadow_log {
	__u8 op;
	__u8 src_cpu;	/* arena CPU the pages belong to; set by userspace via rseq */
	__u16 flags;
	__u32 start_page;
	__u32 nr_pages;
};

#define MD_LOG_RING_MAGIC	0x4d44524cU /* "MDRL" */
#define MD_LOG_RING_VERSION	1U

/*
 * Per-(mm, cpu) userspace<->kernel shared ring. Userspace appends lightweight
 * alloc/free intents on the hot path; kernel drains the ring on context switch.
 *
 * The ring occupies exactly one userspace page chosen by the allocator. The
 * runtime page size can differ across targets, so capacity is recorded
 * explicitly instead of being hard-coded in the ABI.
 */
struct md_log_ring {
	__u32 magic;
	__u16 version;
	__u16 flags;
	__u32 capacity;
	__u32 arena_nr_pages;
	__u32 ring_size;
	__u32 head;
	__u32 tail;
	__u32 dropped;
	__u32 last_error;	/* positive errno from the last rejected record */
	struct md_shadow_log entries[];
};


/*
 * Shared Arena log ABI v3. Old per-mm rings are intentionally not compatible.
 * All u64 fields are naturally aligned, including in 32-bit userspace.
 * log storage is MAP_SHARED shmem outside payload, held by the broker. Both
 * queues use capacity slots: normal first, remote FREE second.
 */
#define MD_SHARED_LOG_MAGIC 0x4d445332U
#define MD_SHARED_LOG_VERSION 3U
/* prctl(74, 0, SHARED_RINGS, 0, MD_SHARED_LOG_QUERY) returns ABI version. */
#define MD_SHARED_LOG_QUERY 1UL
#define MD_SHARED_LOG_MAX_PAGES 256U
#define MD_SHARED_LOG_RUNNING 0U
#define MD_SHARED_LOG_STOPPED 1U
#define MD_SHARED_LOG_F_INTRUSIVE (1U << 0)

struct md_shared_record {
	__aligned_u64 session;
	__aligned_u64 block_id;
	__aligned_u64 generation;
	/* Unique, strictly increasing per-Arena ALLOC serial under Arena Mutex. */
	__aligned_u64 allocation_id;
	__u32 start_page;
	__u32 nr_pages;
	__u16 cpu;
	__u8 op;
	__u8 flags;
	__u32 reserved;
};

struct md_shared_slot {
	/* Producer release-publishes ticket + 1 AFTER writing record. */
	__aligned_u64 ready;
	struct md_shared_record record;
	/* Remote: kernel writes result before completed. Normal: no success receipt. */
	__s32 result; /* 0 or negative errno; never inferred from completed alone */
	__u32 reserved;
};

/* Normal head is a contiguous SUCCESS prefix and immediately frees slots.
 * A rejected normal record is never passed: first error is retained, log stops.
 */
struct md_shared_normal_queue {
	__aligned_u64 reserved; /* userspace release-published tail */
	__aligned_u64 head;     /* kernel release-published successful consume head */
};

/* Remote FREE retains per-request results until userspace updates metadata. */
struct md_shared_queue {
	/* Remote: CAS reservation, NOT publication. */
	__aligned_u64 reserved;
	/* Kernel output only. Records/results retained until reclaimed. */
	__aligned_u64 completed;
	/* Owner-CPU user consumer release-publishes after processing results. */
	__aligned_u64 reclaimed;
};

struct md_shared_log {
	__u32 magic;
	__u16 version;
	__u16 flags;
	__u32 capacity; /* immutable; each queue has this many slots */
	__u32 arena_nr_pages;
	__u32 log_bytes;
	__u32 state; /* kernel fatal error, or broker shutdown: STOPPED */
	struct md_shared_normal_queue normal;
	struct md_shared_queue remote;
	/* Kernel writes once BEFORE release-publishing STOPPED; never overwritten.
	 * ready=ticket+1 for a rejected normal record, zero for queue corruption.
	 */
	struct md_shared_slot normal_error;
	struct md_shared_slot slots[];
};

/* prctl arg2 points here with F_EXTERNAL_BACKEND | F_SHARED_RINGS. */
struct md_shared_registration {
	__u32 size;
	__u32 version;
	__aligned_u64 log_address;
	__u32 log_bytes;
	__u32 reserved;
	/* Kernel output: tied to this mm, CPU, and Arena incarnation; not a PID. */
	__aligned_u64 session;
};

#endif /* _UAPI_LINUX_MEMORY_DELEGATION_H */
