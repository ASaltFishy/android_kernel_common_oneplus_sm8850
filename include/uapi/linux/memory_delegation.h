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

struct md_shadow_log {
	__u8 op;
	__u8 src_cpu;	/* arena CPU the pages belong to; set by userspace via rseq */
	__u16 reserved2;
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
	__u32 reserved;
	struct md_shadow_log entries[];
};

#endif /* _UAPI_LINUX_MEMORY_DELEGATION_H */
