/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MD_SHARED_LOG_V2_HELPERS_H
#define MD_SHARED_LOG_V2_HELPERS_H
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include "../../../../include/uapi/linux/memory_delegation.h"

/*
 * Reference producer protocol, not an allocator. normal_publish requires the
 * owner CPU's process-shared Arena Mutex; check CPU AFTER locking. remote
 * producers touch only log slots. Broker quiesces producers before STOPPED.
 * Only remote FREE retains receipts; normal head releases successful slots.
 */
static inline int md_v2_remote_reserve(struct md_shared_log *log,
				uint64_t *ticket)
{
	struct md_shared_queue *q = &log->remote;
	uint64_t tail = __atomic_load_n(&q->reserved, __ATOMIC_RELAXED);

	if (__atomic_load_n(&log->state, __ATOMIC_ACQUIRE) !=
	    MD_SHARED_LOG_RUNNING)
		return -ESHUTDOWN;
	for (;;) {
		uint64_t reclaimed =
			__atomic_load_n(&q->reclaimed, __ATOMIC_ACQUIRE);

		if (tail < reclaimed || tail == UINT64_MAX)
			return -EOVERFLOW;
		if (tail - reclaimed >= log->capacity)
			return -ENOSPC; /* no overwrite, drain, drop, or blocking fallback */
		if (__atomic_compare_exchange_n(&q->reserved, &tail, tail + 1,
						0, __ATOMIC_RELAXED,
						__ATOMIC_RELAXED)) {
			*ticket = tail;
			return 0;
		}
	}
}

static inline void md_v2_remote_publish(struct md_shared_log *log,
					uint64_t ticket,
					const struct md_shared_record *record)
{
	struct md_shared_slot *slot =
		&log->slots[log->capacity + ticket % log->capacity];

	slot->record = *record;
	__atomic_store_n(&slot->ready, ticket + 1, __ATOMIC_RELEASE);
}

static inline int md_v2_normal_publish(struct md_shared_log *log,
				       const struct md_shared_record *record,
				       uint64_t *ticket)
{
	struct md_shared_normal_queue *q = &log->normal;
	uint64_t tail = __atomic_load_n(&q->reserved, __ATOMIC_RELAXED);
	uint64_t reclaimed = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
	struct md_shared_slot *slot;

	if (__atomic_load_n(&log->state, __ATOMIC_ACQUIRE) !=
	    MD_SHARED_LOG_RUNNING)
		return -ESHUTDOWN;
	if (tail < reclaimed || tail == UINT64_MAX)
		return -EOVERFLOW;
	if (tail - reclaimed >= log->capacity)
		return -ENOSPC;
	slot = &log->slots[tail % log->capacity];
	slot->record = *record;
	__atomic_store_n(&slot->ready, tail + 1, __ATOMIC_RELAXED);
	__atomic_store_n(&q->reserved, tail + 1, __ATOMIC_RELEASE);
	*ticket = tail;
	return 0;
}
#endif
