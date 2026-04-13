// SPDX-License-Identifier: GPL-2.0
#include <linux/bitmap.h>
#include <linux/hash.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/memory_delegation.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define MD_MAX_ARENAS_PER_CPU 4
#define MD_INVALID_SLOT 0xff
#define MD_CHUNK_PAGES 64
#define MD_MM_CTX_HASH_BITS 10

struct md_owner_entry {
	struct mm_struct *mm;
	u32 gen;
	bool valid;
};

// per-CPU arena 的真值表
struct md_arena_meta {
	spinlock_t lock;
	u32 arena_id;
	u32 nr_pages;
	u32 nr_chunks;
	u32 owner_slots;
	u64 global_commit_seq;
	// page_slot[u8] 用于记录每个页的 owner 索引
	u8 *page_slot;
	// page_owner_gen[u32] 用于记录每个页的 owner 代际
	u32 *page_owner_gen;
	// chunk_gen[u64] 用于记录每个 chunk 的代际
	u64 *chunk_gen;
	// owner_table[MD_MAX_OWNER_SLOTS] 用于记录每个 slot对应的 mm 和 gen
	struct md_owner_entry *owner_table;
};

struct md_mm_ctx_key {
	struct mm_struct *mm;
	u16 cpu;
	u16 arena_id;
};

// 记录每个md_mm_ctx_key三元组的last_seen_gen和needs_pt_sync
struct md_mm_ctx {
	struct hlist_node node;
	struct md_mm_ctx_key key;
	spinlock_t lock;
	u64 last_seen_gen;
	bool needs_pt_sync;
};

static DEFINE_MUTEX(md_arena_table_lock);
static DEFINE_HASHTABLE(md_mm_ctx_table, MD_MM_CTX_HASH_BITS);
static DEFINE_SPINLOCK(md_mm_ctx_table_lock);
static struct md_arena_meta __rcu *md_arenas[NR_CPUS][MD_MAX_ARENAS_PER_CPU];

static inline u32 md_chunk_of_page(u32 page_idx)
{
	return page_idx / MD_CHUNK_PAGES;
}

static inline bool md_page_belongs_to_mm(const struct md_arena_meta *arena,
					 u32 page_idx, struct mm_struct *mm)
{
	u8 slot;
	u32 page_gen;
	const struct md_owner_entry *owner;

	slot = READ_ONCE(arena->page_slot[page_idx]);
	if (slot == MD_INVALID_SLOT || slot >= arena->owner_slots)
		return false;

	page_gen = READ_ONCE(arena->page_owner_gen[page_idx]);
	owner = &arena->owner_table[slot];

	return READ_ONCE(owner->valid) &&
	       READ_ONCE(owner->mm) == mm &&
	       READ_ONCE(owner->gen) == page_gen;
}

static inline unsigned long md_mm_ctx_hash(const struct md_mm_ctx_key *key)
{
	unsigned long mix = (unsigned long)key->mm;

	mix ^= (unsigned long)key->cpu << 8;
	mix ^= (unsigned long)key->arena_id << 24;
	return mix;
}

static struct md_mm_ctx *md_mm_ctx_lookup_locked(const struct md_mm_ctx_key *key)
{
	struct md_mm_ctx *ctx;
	unsigned long hash = md_mm_ctx_hash(key);

	hash_for_each_possible(md_mm_ctx_table, ctx, node, hash) {
		if (ctx->key.mm == key->mm &&
		    ctx->key.cpu == key->cpu &&
		    ctx->key.arena_id == key->arena_id)
			return ctx;
	}

	return NULL;
}

static struct md_mm_ctx *
md_mm_ctx_get_or_create(struct mm_struct *mm, unsigned int cpu,
			unsigned int arena_id, gfp_t gfp)
{
	struct md_mm_ctx_key key = {
		.mm = mm,
		.cpu = cpu,
		.arena_id = arena_id,
	};
	struct md_mm_ctx *ctx;

	spin_lock(&md_mm_ctx_table_lock);
	ctx = md_mm_ctx_lookup_locked(&key);
	spin_unlock(&md_mm_ctx_table_lock);
	if (ctx)
		return ctx;

	ctx = kzalloc(sizeof(*ctx), gfp);
	if (!ctx)
		return NULL;

	ctx->key = key;
	spin_lock_init(&ctx->lock);

	spin_lock(&md_mm_ctx_table_lock);
	if (!md_mm_ctx_lookup_locked(&key)) {
		hash_add(md_mm_ctx_table, &ctx->node, md_mm_ctx_hash(&key));
		spin_unlock(&md_mm_ctx_table_lock);
		return ctx;
	}
	spin_unlock(&md_mm_ctx_table_lock);

	kfree(ctx);
	spin_lock(&md_mm_ctx_table_lock);
	ctx = md_mm_ctx_lookup_locked(&key);
	spin_unlock(&md_mm_ctx_table_lock);
	return ctx;
}

static struct md_arena_meta *md_get_arena_rcu(unsigned int cpu,
					      unsigned int arena_id)
{
	if (cpu >= nr_cpu_ids || arena_id >= MD_MAX_ARENAS_PER_CPU)
		return NULL;

	return rcu_dereference(md_arenas[cpu][arena_id]);
}

static int md_unmap_range_locked(struct mm_struct *mm, unsigned long start,
				 unsigned long end)
{
	struct vm_area_struct *vma;
	unsigned long cursor = start;

	while (cursor < end) {
		unsigned long unmap_end;

		vma = find_vma(mm, cursor);
		if (!vma)
			break;

		if (cursor < vma->vm_start)
			cursor = vma->vm_start;
		if (cursor >= end)
			break;

		unmap_end = min(end, vma->vm_end);
		zap_page_range_single(vma, cursor, unmap_end - cursor, NULL);
		cursor = unmap_end;
	}

	return 0;
}

static void md_apply_log_entry(struct md_arena_meta *arena,
			       const struct md_shadow_log *entry,
			       struct mm_struct *owner_mm,
			       unsigned long *dirty_chunks)
{
	u32 start = entry->start_page;
	u32 end = min_t(u32, arena->nr_pages, start + entry->nr_pages);
	u32 i;

	if (start >= end)
		return;

	bitmap_set(dirty_chunks, md_chunk_of_page(start),
		   md_chunk_of_page(end - 1) - md_chunk_of_page(start) + 1);

	if (entry->op == MD_LOG_ALLOC) {
		u16 slot = entry->owner_slot;

		if (slot >= arena->owner_slots)
			return;

		arena->owner_table[slot].mm = owner_mm;
		arena->owner_table[slot].gen = entry->owner_gen;
		arena->owner_table[slot].valid = true;
		for (i = start; i < end; i++) {
			arena->page_slot[i] = (u8)slot;
			arena->page_owner_gen[i] = entry->owner_gen;
		}
		return;
	}

	if (entry->op == MD_LOG_FREE) {
		for (i = start; i < end; i++) {
			arena->page_slot[i] = MD_INVALID_SLOT;
			arena->page_owner_gen[i] = 0;
		}
	}
}

int memory_delegation_arena_register(unsigned int cpu, unsigned int arena_id,
				     unsigned int nr_pages,
				     unsigned int owner_slots)
{
	struct md_arena_meta *arena;
	struct md_arena_meta *old_arena;

	if (cpu >= nr_cpu_ids || arena_id >= MD_MAX_ARENAS_PER_CPU)
		return -EINVAL;
	if (!nr_pages || !owner_slots || owner_slots >= MD_INVALID_SLOT)
		return -EINVAL;

	arena = kzalloc(sizeof(*arena), GFP_KERNEL);
	if (!arena)
		return -ENOMEM;

	arena->arena_id = arena_id;
	arena->nr_pages = nr_pages;
	arena->nr_chunks = DIV_ROUND_UP(nr_pages, MD_CHUNK_PAGES);
	arena->owner_slots = owner_slots;
	spin_lock_init(&arena->lock);

	arena->page_slot = kvzalloc(nr_pages, GFP_KERNEL);
	arena->page_owner_gen = kvcalloc(nr_pages, sizeof(*arena->page_owner_gen),
					 GFP_KERNEL);
	arena->chunk_gen = kvcalloc(arena->nr_chunks, sizeof(*arena->chunk_gen),
				    GFP_KERNEL);
	arena->owner_table = kvcalloc(owner_slots, sizeof(*arena->owner_table),
				      GFP_KERNEL);
	if (!arena->page_slot || !arena->page_owner_gen ||
	    !arena->chunk_gen || !arena->owner_table) {
		kvfree(arena->owner_table);
		kvfree(arena->chunk_gen);
		kvfree(arena->page_owner_gen);
		kvfree(arena->page_slot);
		kfree(arena);
		return -ENOMEM;
	}

	memset(arena->page_slot, MD_INVALID_SLOT, nr_pages);

	mutex_lock(&md_arena_table_lock);
	old_arena = rcu_dereference_protected(md_arenas[cpu][arena_id],
					      lockdep_is_held(&md_arena_table_lock));
	rcu_assign_pointer(md_arenas[cpu][arena_id], arena);
	mutex_unlock(&md_arena_table_lock);

	if (old_arena) {
		synchronize_rcu();
		kvfree(old_arena->owner_table);
		kvfree(old_arena->chunk_gen);
		kvfree(old_arena->page_owner_gen);
		kvfree(old_arena->page_slot);
		kfree(old_arena);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(memory_delegation_arena_register);

void memory_delegation_arena_unregister(unsigned int cpu, unsigned int arena_id)
{
	struct md_arena_meta *arena;

	if (cpu >= nr_cpu_ids || arena_id >= MD_MAX_ARENAS_PER_CPU)
		return;

	mutex_lock(&md_arena_table_lock);
	arena = rcu_dereference_protected(md_arenas[cpu][arena_id],
					  lockdep_is_held(&md_arena_table_lock));
	RCU_INIT_POINTER(md_arenas[cpu][arena_id], NULL);
	mutex_unlock(&md_arena_table_lock);

	if (!arena)
		return;

	synchronize_rcu();
	kvfree(arena->owner_table);
	kvfree(arena->chunk_gen);
	kvfree(arena->page_owner_gen);
	kvfree(arena->page_slot);
	kfree(arena);
}
EXPORT_SYMBOL_GPL(memory_delegation_arena_unregister);

int memory_delegation_submit_log(unsigned int cpu,
				 const struct md_shadow_log *log,
				 unsigned int nr_entries)
{
	struct md_arena_meta *arena;
	unsigned long *dirty_chunks;
	unsigned int i;
	u64 commit_seq;

	if (!log || !nr_entries)
		return 0;
	if (cpu >= nr_cpu_ids || log[0].arena_id >= MD_MAX_ARENAS_PER_CPU)
		return -EINVAL;

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu, log[0].arena_id);
	if (!arena) {
		rcu_read_unlock();
		return -ENOENT;
	}

	dirty_chunks = bitmap_zalloc(arena->nr_chunks, GFP_ATOMIC);
	if (!dirty_chunks) {
		rcu_read_unlock();
		return -ENOMEM;
	}

	spin_lock(&arena->lock);
	commit_seq = ++arena->global_commit_seq;
	for (i = 0; i < nr_entries; i++) {
		if (log[i].arena_id != log[0].arena_id)
			continue;
		md_apply_log_entry(arena, &log[i], current->mm, dirty_chunks);
	}
	for_each_set_bit(i, dirty_chunks, arena->nr_chunks)
		arena->chunk_gen[i] = commit_seq;
	spin_unlock(&arena->lock);

	bitmap_free(dirty_chunks);
	rcu_read_unlock();
	return 0;
}
EXPORT_SYMBOL_GPL(memory_delegation_submit_log);

int memory_delegation_submit_log_and_sync(unsigned int cpu,
					  const struct md_shadow_log *log,
					  unsigned int nr_entries,
					  unsigned long arena_base)
{
	int ret;
	struct mm_struct *mm = current->mm;
	unsigned int arena_id;

	if (!mm)
		return -EINVAL;
	if (!log || !nr_entries)
		return 0;

	arena_id = log[0].arena_id;
	ret = memory_delegation_submit_log(cpu, log, nr_entries);
	if (ret)
		return ret;

	mmap_write_lock(mm);
	ret = memory_delegation_sync_mm(mm, cpu, arena_id, arena_base);
	mmap_write_unlock(mm);

	return ret;
}
EXPORT_SYMBOL_GPL(memory_delegation_submit_log_and_sync);

void memory_delegation_on_context_switch(struct task_struct *prev,
					 struct task_struct *next)
{
	struct md_arena_meta *arena;
	struct md_mm_ctx *ctx;
	struct mm_struct *next_mm;
	unsigned int cpu;
	unsigned int arena_id;

	if (!next || !next->mm)
		return;
	(void)prev;

	next_mm = next->mm;
	cpu = smp_processor_id();

	rcu_read_lock();
	for (arena_id = 0; arena_id < MD_MAX_ARENAS_PER_CPU; arena_id++) {
		u64 latest_seq;

		arena = md_get_arena_rcu(cpu, arena_id);
		if (!arena)
			continue;

		ctx = md_mm_ctx_get_or_create(next_mm, cpu, arena_id, GFP_ATOMIC);
		if (!ctx)
			continue;

		latest_seq = READ_ONCE(arena->global_commit_seq);
		spin_lock(&ctx->lock);
		if (latest_seq > ctx->last_seen_gen)
			ctx->needs_pt_sync = true;
		spin_unlock(&ctx->lock);
	}
	rcu_read_unlock();
}

int memory_delegation_sync_mm(struct mm_struct *mm, unsigned int cpu,
			      unsigned int arena_id,
			      unsigned long arena_base)
{
	struct md_arena_meta *arena;
	struct md_mm_ctx *ctx;
	u64 last_seen;
	u64 target_seq;
	u32 chunk;

	if (!mm || cpu >= nr_cpu_ids || arena_id >= MD_MAX_ARENAS_PER_CPU)
		return -EINVAL;

	mmap_assert_write_locked(mm);

	ctx = md_mm_ctx_get_or_create(mm, cpu, arena_id, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_lock(&md_arena_table_lock);
	arena = rcu_dereference_protected(md_arenas[cpu][arena_id],
					  lockdep_is_held(&md_arena_table_lock));
	if (!arena) {
		mutex_unlock(&md_arena_table_lock);
		return -ENOENT;
	}

	spin_lock(&ctx->lock);
	last_seen = ctx->last_seen_gen;
	spin_unlock(&ctx->lock);

	target_seq = READ_ONCE(arena->global_commit_seq);
	if (target_seq <= last_seen) {
		spin_lock(&ctx->lock);
		ctx->needs_pt_sync = false;
		spin_unlock(&ctx->lock);
		mutex_unlock(&md_arena_table_lock);
		return 0;
	}

	for (chunk = 0; chunk < arena->nr_chunks; chunk++) {
		u64 chunk_gen = READ_ONCE(arena->chunk_gen[chunk]);
		u32 start_page;
		u32 end_page;
		u32 page;
		u32 run_start = 0;
		bool in_run = false;

		if (chunk_gen <= last_seen)
			continue;

		start_page = chunk * MD_CHUNK_PAGES;
		end_page = min_t(u32, arena->nr_pages, start_page + MD_CHUNK_PAGES);

		for (page = start_page; page < end_page; page++) {
			bool owned = md_page_belongs_to_mm(arena, page, mm);

			if (!owned && !in_run) {
				in_run = true;
				run_start = page;
				continue;
			}
			if (owned && in_run) {
				unsigned long start = arena_base +
					(unsigned long)run_start * PAGE_SIZE;
				unsigned long end = arena_base +
					(unsigned long)page * PAGE_SIZE;
				md_unmap_range_locked(mm, start, end);
				in_run = false;
			}
		}

		if (in_run) {
			unsigned long start = arena_base +
				(unsigned long)run_start * PAGE_SIZE;
			unsigned long end = arena_base +
				(unsigned long)end_page * PAGE_SIZE;
			md_unmap_range_locked(mm, start, end);
		}
	}

	spin_lock(&ctx->lock);
	ctx->last_seen_gen = target_seq;
	ctx->needs_pt_sync = false;
	spin_unlock(&ctx->lock);

	mutex_unlock(&md_arena_table_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(memory_delegation_sync_mm);

bool memory_delegation_fault_allowed(struct mm_struct *mm, unsigned int cpu,
				     unsigned int arena_id,
				     unsigned long arena_base,
				     unsigned long address)
{
	struct md_arena_meta *arena;
	unsigned long offset;
	u32 page_idx;
	bool allowed = false;

	if (!mm || cpu >= nr_cpu_ids || arena_id >= MD_MAX_ARENAS_PER_CPU)
		return false;
	if (address < arena_base)
		return false;

	offset = address - arena_base;
	page_idx = offset >> PAGE_SHIFT;

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu, arena_id);
	if (!arena || page_idx >= arena->nr_pages)
		goto out;

	allowed = md_page_belongs_to_mm(arena, page_idx, mm);
out:
	rcu_read_unlock();
	return allowed;
}
EXPORT_SYMBOL_GPL(memory_delegation_fault_allowed);

