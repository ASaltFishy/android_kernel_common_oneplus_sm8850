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
	__u8 arena_id;
	__u16 owner_slot;
	__u32 start_page;
	__u32 nr_pages;
	__u32 owner_gen;
	__u64 seq;
};

#endif /* _UAPI_LINUX_MEMORY_DELEGATION_H */
