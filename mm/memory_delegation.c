// SPDX-License-Identifier: GPL-2.0
#include <linux/bitmap.h>
#include <linux/hash.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/memory_delegation.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/task_work.h>

#define MD_INVALID_SLOT 0xff
#define MD_CHUNK_PAGES 64
#define MD_MM_CTX_HASH_BITS 10
#define MD_SHARED_ARENA_CAPACITY	(256UL * 1024 * 1024)
#define MD_SHARED_ARENA_HEADER_SIZE	PAGE_SIZE
#define MD_SHARED_ARENA_NR_PAGES	\
	((MD_SHARED_ARENA_CAPACITY - MD_SHARED_ARENA_HEADER_SIZE) / PAGE_SIZE)
#define MD_DEFAULT_OWNER_SLOTS		254

struct md_owner_entry {
	struct mm_struct *mm;
	u32 gen;
	u32 ref_pages;
	bool valid;
};

struct md_arena_meta {
	spinlock_t lock;
	u32 nr_pages;
	u32 nr_chunks;
	u32 owner_slots;
	u64 global_commit_seq;
	u8 *page_slot;
	u8 *page_free;
	u32 *page_owner_gen;
	u64 *chunk_gen;
	struct md_owner_entry *owner_table;
};

struct md_mm_ctx_key {
	struct mm_struct *mm;
	u16 cpu;
};

struct md_mm_ctx {
	struct hlist_node node;
	refcount_t refs;
	struct md_mm_ctx_key key;
	spinlock_t lock;
	struct callback_head sync_work;
	unsigned long arena_base;
	struct page *ring_page;
	u64 last_seen_gen;
	bool sync_queued;
	bool dead;
};

static DEFINE_MUTEX(md_arena_table_lock);
static DEFINE_HASHTABLE(md_mm_ctx_table, MD_MM_CTX_HASH_BITS);
static DEFINE_SPINLOCK(md_mm_ctx_table_lock);
static struct md_arena_meta __rcu *md_arenas[NR_CPUS];

struct md_vma_wrap {
	const struct vm_operations_struct *orig_ops;
	unsigned int cpu;
	unsigned long arena_base; /* userspace DataBase (not header base) */
	struct vm_area_struct *owner_vma;
};

static vm_fault_t md_arena_vma_fault(struct vm_fault *vmf);
static vm_fault_t md_arena_vma_page_mkwrite(struct vm_fault *vmf);
static void md_arena_vma_open(struct vm_area_struct *vma);
static void md_arena_vma_close(struct vm_area_struct *vma);

static const struct vm_operations_struct md_arena_vm_ops = {
	.open = md_arena_vma_open,
	.close = md_arena_vma_close,
	.fault = md_arena_vma_fault,
	.page_mkwrite = md_arena_vma_page_mkwrite,
};

static void md_drain_log_ring(struct md_mm_ctx *ctx);

/*
 * Number of arenas currently registered.  Incremented under md_arena_table_lock
 * in arena_register, decremented in arena_unregister.  Read locklessly as a
 * fast-path guard: if zero, no delegation is active and all hot-path hooks can
 * return immediately without acquiring any other lock.
 */
static atomic_t md_active_arenas = ATOMIC_INIT(0);

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

static inline bool md_page_belongs_to_mm_locked(const struct md_arena_meta *arena,
						u32 page_idx,
						struct mm_struct *mm)
{
	u8 slot = arena->page_slot[page_idx];
	u32 page_gen;
	const struct md_owner_entry *owner;

	if (slot == MD_INVALID_SLOT || slot >= arena->owner_slots)
		return false;

	page_gen = arena->page_owner_gen[page_idx];
	owner = &arena->owner_table[slot];

	return owner->valid && owner->mm == mm && owner->gen == page_gen;
}

static inline bool md_page_accessible_to_mm_locked(const struct md_arena_meta *arena,
						   u32 page_idx,
						   struct mm_struct *mm)
{
	if (arena->page_slot[page_idx] == MD_INVALID_SLOT)
		return true;

	return md_page_belongs_to_mm_locked(arena, page_idx, mm);
}

static inline unsigned long md_mm_ctx_hash(const struct md_mm_ctx_key *key)
{
	return hash_long(hash_ptr(key->mm, MD_MM_CTX_HASH_BITS) ^ key->cpu,
			 MD_MM_CTX_HASH_BITS);
}

static struct md_mm_ctx *md_mm_ctx_lookup_locked(const struct md_mm_ctx_key *key)
{
	struct md_mm_ctx *ctx;
	unsigned long hash = md_mm_ctx_hash(key);

	hash_for_each_possible(md_mm_ctx_table, ctx, node, hash) {
		if (ctx->key.mm == key->mm && ctx->key.cpu == key->cpu)
			return ctx;
	}

	return NULL;
}

/*
	use reference count to maintain lifecycle of md_mm_ctx
	if ref=0, free md_mm_ctx
	与之对应：
	md_mm_ctx_get_or_create：用于获取或创建 md_mm_ctx 上下文（基于 mm_struct 和 CPU），并增加引用计数。如果上下文不存在，会创建新的；如果存在，会直接增加引用计数返回。
	md_mm_ctx_lookup_get：用于查找已存在的 md_mm_ctx 上下文（同样基于 mm_struct 和 CPU），并增加引用计数。如果找到，则返回并增加引用；否则返回 NULL。
*/
static void md_mm_ctx_put(struct md_mm_ctx *ctx)
{
	if (refcount_dec_and_test(&ctx->refs)) {
		if (ctx->ring_page)
			unpin_user_page(ctx->ring_page);
		kfree(ctx);
	}
}

static void md_sync_task_work(struct callback_head *work);

static struct md_mm_ctx *md_mm_ctx_get_or_create(struct mm_struct *mm,
						 unsigned int cpu, gfp_t gfp)
{
	struct md_mm_ctx_key key = {
		.mm = mm,
		.cpu = cpu,
	};
	struct md_mm_ctx *ctx, *new_ctx;

	spin_lock(&md_mm_ctx_table_lock);
	ctx = md_mm_ctx_lookup_locked(&key);
	if (ctx && !ctx->dead) {
		refcount_inc(&ctx->refs);
		spin_unlock(&md_mm_ctx_table_lock);
		return ctx;
	}
	spin_unlock(&md_mm_ctx_table_lock);

	new_ctx = kzalloc(sizeof(*new_ctx), gfp);
	if (!new_ctx)
		return NULL;

	new_ctx->key = key;
	refcount_set(&new_ctx->refs, 2);
	spin_lock_init(&new_ctx->lock);
	init_task_work(&new_ctx->sync_work, md_sync_task_work);

	spin_lock(&md_mm_ctx_table_lock);
	ctx = md_mm_ctx_lookup_locked(&key);
	if (!ctx || ctx->dead) {
		hash_add(md_mm_ctx_table, &new_ctx->node, md_mm_ctx_hash(&key));
		spin_unlock(&md_mm_ctx_table_lock);
		return new_ctx;
	}
	refcount_inc(&ctx->refs);
	spin_unlock(&md_mm_ctx_table_lock);

	kfree(new_ctx);
	return ctx;
}

static struct md_mm_ctx *md_mm_ctx_lookup_get(struct mm_struct *mm,
					      unsigned int cpu)
{
	struct md_mm_ctx_key key = {
		.mm = mm,
		.cpu = cpu,
	};
	struct md_mm_ctx *ctx;

	spin_lock(&md_mm_ctx_table_lock);
	ctx = md_mm_ctx_lookup_locked(&key);
	if (ctx && !ctx->dead)
		refcount_inc(&ctx->refs);
	else
		ctx = NULL;
	spin_unlock(&md_mm_ctx_table_lock);

	return ctx;
}

static struct md_arena_meta *md_get_arena_rcu(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids)
		return NULL;

	return rcu_dereference(md_arenas[cpu]);
}

static inline bool md_vma_is_wrapped(const struct vm_area_struct *vma)
{
	return vma && vma->vm_ops == &md_arena_vm_ops;
}

static struct md_vma_wrap *md_vma_wrap_get(const struct vm_area_struct *vma)
{
	if (!vma)
		return NULL;
	if (vma->vm_ops != &md_arena_vm_ops)
		return NULL;
	return (struct md_vma_wrap *)vma->vm_private_data;
}

static bool md_fault_addr_allowed(const struct vm_area_struct *vma,
				  unsigned long address)
{
	struct md_vma_wrap *wrap;
	struct md_arena_meta *arena;
	unsigned long offset;
	u32 page_idx;
	bool allowed = false;
	u8 slot;

	if (!vma)
		return true;

	wrap = md_vma_wrap_get(vma);
	if (!wrap)
		return true;

	if (address < wrap->arena_base)
		return false;

	offset = address - wrap->arena_base;
	page_idx = offset >> PAGE_SHIFT;

	rcu_read_lock();
	arena = md_get_arena_rcu(wrap->cpu);
	if (arena && page_idx < arena->nr_pages) {
		slot = READ_ONCE(arena->page_slot[page_idx]);
		/*
		 * 空闲页（MD_INVALID_SLOT）里存放 SharedArena 的 freelist
		 * 元数据。允许其 fault 通过，否则一旦空闲页的 PTE
		 * 被撤销（sync/unmap），用户态在 retrieve/free-list scan
		 * 时会触发 SIGSEGV。
		 *
		 * 仅当页属于“其他 mm 的已分配所有权”时拒绝。
		 */
		if (slot == MD_INVALID_SLOT) {
			allowed = true;
		} else {
			allowed = md_page_belongs_to_mm(arena, page_idx, vma->vm_mm);
		}
	}
	rcu_read_unlock();

	return allowed;
}

static bool md_fault_addr_owned(const struct vm_area_struct *vma,
				unsigned long address)
{
	return md_fault_addr_allowed(vma, address);
}

// VMA 被创建或者分裂时调用
static void md_arena_vma_open(struct vm_area_struct *vma)
{
	struct md_vma_wrap *wrap = md_vma_wrap_get(vma);
	struct md_vma_wrap *new_wrap;

	/*
	 * VMA split/dup can copy vm_private_data. Ensure each VMA owns its wrap.
	 */
	if (wrap && wrap->owner_vma != vma) {
		new_wrap = kmemdup(wrap, sizeof(*wrap), GFP_KERNEL);
		if (new_wrap) {
			new_wrap->owner_vma = vma;
			vma->vm_private_data = new_wrap;
		}
	}

	wrap = md_vma_wrap_get(vma);
	if (wrap && wrap->orig_ops && wrap->orig_ops->open)
		wrap->orig_ops->open(vma);
}

// VMA 销毁时调用
static void md_arena_vma_close(struct vm_area_struct *vma)
{
	struct md_vma_wrap *wrap = md_vma_wrap_get(vma);

	if (!wrap)
		return;

	if (wrap->orig_ops && wrap->orig_ops->close)
		wrap->orig_ops->close(vma);

	kfree(wrap);
	vma->vm_private_data = NULL;
}

static vm_fault_t md_arena_vma_fault(struct vm_fault *vmf)
{
	struct md_vma_wrap *wrap;

	wrap = md_vma_wrap_get(vmf->vma);
	if (!wrap)
		return VM_FAULT_SIGSEGV;

	if (unlikely(!md_fault_addr_allowed(vmf->vma, vmf->address)))
		return VM_FAULT_SIGSEGV;

	if (!wrap || !wrap->orig_ops || !wrap->orig_ops->fault)
		return VM_FAULT_SIGSEGV;

	return wrap->orig_ops->fault(vmf);
}

// 对只读或者CoW页面进行写操作时产生写保护异常
static vm_fault_t md_arena_vma_page_mkwrite(struct vm_fault *vmf)
{
	struct md_vma_wrap *wrap = md_vma_wrap_get(vmf->vma);

	if (unlikely(!md_fault_addr_owned(vmf->vma, vmf->address)))
		return VM_FAULT_SIGSEGV;

	if (!wrap || !wrap->orig_ops || !wrap->orig_ops->page_mkwrite)
		return 0;

	return wrap->orig_ops->page_mkwrite(vmf);
}

static int md_install_vma_ops(struct mm_struct *mm, unsigned int cpu,
			      unsigned long arena_base, u32 nr_pages)
{
	struct vm_area_struct *vma;
	unsigned long end = arena_base + (unsigned long)nr_pages * PAGE_SIZE;
	struct md_vma_wrap *wrap;

	if (!mm || cpu >= nr_cpu_ids || !arena_base || !nr_pages)
		return -EINVAL;

	mmap_assert_write_locked(mm);

	vma = find_vma(mm, arena_base);
	if (!vma || arena_base < vma->vm_start)
		return -EINVAL;
	if (vma->vm_end < end)
		return -EINVAL;
	if (!(vma->vm_flags & VM_SHARED) || !vma->vm_file)
		return -EINVAL;

	/* Idempotent: already installed. */
	if (md_vma_is_wrapped(vma)) {
		wrap = md_vma_wrap_get(vma);
		if (!wrap)
			return -EINVAL;
		if (wrap->cpu != cpu || wrap->arena_base != arena_base)
			return -EINVAL;
		return 0;
	}

	/*
	 * Do not clobber mappings whose vm_private_data is in use by other vm_ops.
	 * Today SharedArena uses shmem/tmpfs (shmem_vm_ops) which does not use
	 * vm_private_data in this tree, so storing wrap here is safe.
	 */
	if (vma->vm_private_data)
		return -EBUSY;

	wrap = kzalloc(sizeof(*wrap), GFP_KERNEL);
	if (!wrap)
		return -ENOMEM;

	wrap->orig_ops = vma->vm_ops;
	wrap->cpu = cpu;
	wrap->arena_base = arena_base;
	wrap->owner_vma = vma;

	vma->vm_private_data = wrap;
	vma->vm_ops = &md_arena_vm_ops;

	/* Ensure open() is run for consistency with other vm_ops users. */
	md_arena_vma_open(vma);
	return 0;
}

static struct md_arena_meta *md_get_or_create_arena(unsigned int cpu,
						    u32 nr_pages)
{
	struct md_arena_meta *arena;
	int ret;

	if (cpu >= nr_cpu_ids)
		return NULL;

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu);
	rcu_read_unlock();
	if (arena) {
		if (arena->nr_pages == nr_pages)
			return arena;
		return NULL;
	}

	ret = memory_delegation_arena_register(cpu, nr_pages,
					       MD_DEFAULT_OWNER_SLOTS);
	if (ret && ret != -EEXIST)
		return NULL;

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu);
	rcu_read_unlock();
	return arena;
}

static int md_pin_log_ring(struct md_mm_ctx *ctx, unsigned long ring_addr,
			   u32 *out_nr_pages)
{
	struct page *page;
	struct md_log_ring *ring;
	u32 max_capacity;
	void *kaddr;
	long pinned;
	int ret = 0;

	if (!ctx || !ring_addr || !PAGE_ALIGNED(ring_addr) || !out_nr_pages)
		return -EINVAL;
	if (READ_ONCE(ctx->ring_page)) {
		kaddr = kmap_local_page(ctx->ring_page);
		ring = kaddr;
		*out_nr_pages = ring->arena_nr_pages;
		kunmap_local(kaddr);
		return 0;
	}

	pinned = pin_user_pages_fast(ring_addr, 1,
				     FOLL_WRITE | FOLL_LONGTERM, &page);
	if (pinned < 0)
		return pinned;
	if (pinned != 1) {
		if (pinned > 0)
			unpin_user_pages(&page, pinned);
		return -EFAULT;
	}

	kaddr = kmap_local_page(page);
	ring = kaddr;
	max_capacity = (PAGE_SIZE - sizeof(*ring)) / sizeof(struct md_shadow_log);
	if (ring->magic != MD_LOG_RING_MAGIC ||
	    ring->version != MD_LOG_RING_VERSION ||
	    ring->ring_size != PAGE_SIZE ||
	    !ring->capacity || ring->capacity > max_capacity ||
	    !ring->arena_nr_pages) {
		ret = -EINVAL;
	} else {
		*out_nr_pages = ring->arena_nr_pages;
	}
	kunmap_local(kaddr);
	if (ret) {
		unpin_user_page(page);
		return ret;
	}

	spin_lock(&ctx->lock);
	if (!ctx->ring_page)
		ctx->ring_page = page;
	else
		unpin_user_page(page);
	spin_unlock(&ctx->lock);
	return 0;
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

static int md_prefault_range_locked(struct mm_struct *mm, unsigned long start,
				    unsigned long end)
{
	unsigned long addr;

	for (addr = start; addr < end; addr += PAGE_SIZE) {
		int ret;

		// 这里不会再走vma_fault的检查路径，直接创建映射
		ret = fixup_user_fault(mm, addr, FAULT_FLAG_WRITE, NULL);
		if (ret)
			return ret;
	}

	return 0;
}

static u16 md_get_owner_slot_locked(struct md_arena_meta *arena,
				    struct mm_struct *mm)
{
	u16 free_slot = U16_MAX;
	u16 slot;

	for (slot = 0; slot < arena->owner_slots; slot++) {
		struct md_owner_entry *owner = &arena->owner_table[slot];

		if (owner->valid && owner->mm == mm)
			return slot;
		if (!owner->valid && free_slot == U16_MAX)
			free_slot = slot;
	}

	if (free_slot == U16_MAX)
		return U16_MAX;

	arena->owner_table[free_slot].mm = mm;
	arena->owner_table[free_slot].gen++;
	if (!arena->owner_table[free_slot].gen)
		arena->owner_table[free_slot].gen = 1;
	arena->owner_table[free_slot].ref_pages = 0;
	arena->owner_table[free_slot].valid = true;

	return free_slot;
}

static void md_put_owner_slot_locked(struct md_arena_meta *arena, u16 slot)
{
	struct md_owner_entry *owner;

	if (slot >= arena->owner_slots)
		return;

	owner = &arena->owner_table[slot];
	if (owner->ref_pages)
		return;

	owner->mm = NULL;
	owner->valid = false;
}

static void md_mark_dirty_chunks(unsigned long *dirty_chunks, u32 start, u32 end)
{
	bitmap_set(dirty_chunks, md_chunk_of_page(start),
		   md_chunk_of_page(end - 1) - md_chunk_of_page(start) + 1);
}

static int md_apply_log_entry_locked(struct md_arena_meta *arena,
				     const struct md_shadow_log *entry,
				     struct mm_struct *owner_mm,
				     unsigned long *dirty_chunks)
{
	u32 start = entry->start_page;
	u32 end;
	u32 i;

	if (!entry->nr_pages)
		return 0;
	if (check_add_overflow(start, entry->nr_pages, &end))
		return -EOVERFLOW;
	if (end > arena->nr_pages)
		return -EINVAL;

	if (entry->op == MD_LOG_ALLOC) {
		u16 slot;
		u32 owner_gen;

		for (i = start; i < end; i++) {
			if (arena->page_slot[i] != MD_INVALID_SLOT)
				return -EBUSY;
		}

		slot = md_get_owner_slot_locked(arena, owner_mm);
		if (slot == U16_MAX)
			return -ENOSPC;

		owner_gen = arena->owner_table[slot].gen;
		for (i = start; i < end; i++) {
			arena->page_slot[i] = (u8)slot;
			arena->page_owner_gen[i] = owner_gen;
		}
		arena->owner_table[slot].ref_pages += end - start;
		md_mark_dirty_chunks(dirty_chunks, start, end);
		return 0;
	}

	if (entry->op == MD_LOG_FREE) {
		for (i = start; i < end; i++) {
			if (!md_page_belongs_to_mm_locked(arena, i, owner_mm))
				return -EPERM;
		}

		for (i = start; i < end; i++) {
			u16 slot = arena->page_slot[i];

			arena->page_slot[i] = MD_INVALID_SLOT;
			arena->page_owner_gen[i] = 0;
			if (slot < arena->owner_slots &&
			    arena->owner_table[slot].ref_pages)
				arena->owner_table[slot].ref_pages--;
			md_put_owner_slot_locked(arena, slot);
		}
		md_mark_dirty_chunks(dirty_chunks, start, end);
		return 0;
	}

	return -EINVAL;
}

static u64 md_commit_dirty_chunks_locked(struct md_arena_meta *arena,
					 unsigned long *dirty_chunks)
{
	u64 commit_seq;
	unsigned int chunk;

	if (bitmap_empty(dirty_chunks, arena->nr_chunks))
		return 0;

	commit_seq = ++arena->global_commit_seq;
	for_each_set_bit(chunk, dirty_chunks, arena->nr_chunks)
		arena->chunk_gen[chunk] = commit_seq;

	return commit_seq;
}

/*
 * Drain the ring associated with ctx, dispatching each entry to the arena
 * identified by entry->src_cpu.  A single ring can therefore service
 * cross-core free operations without requiring the caller to hold any
 * particular arena lock.
 *
 * Algorithm:
 *   1. Read head/tail from the ring page (no arena lock needed).
 *   2. Build a bitmap of every src_cpu referenced in [head, tail).
 *   3. For each src_cpu: lock its arena, apply matching entries, commit.
 *   4. Advance ring->head to tail.
 *
 * Called from context_switch() – atomic context, no sleeping.
 */
static void md_drain_log_ring(struct md_mm_ctx *ctx)
{
	DECLARE_BITMAP(dirty_chunks, DIV_ROUND_UP(MD_SHARED_ARENA_NR_PAGES,
						  MD_CHUNK_PAGES));
	DECLARE_BITMAP(src_cpus, NR_CPUS);
	struct md_log_ring *ring;
	struct md_shadow_log *entries;
	struct page *ring_page;
	void *kaddr;
	u32 head, tail, i;
	unsigned long src_cpu;

	if (!ctx || !ctx->key.mm)
		return;

	spin_lock(&ctx->lock);
	ring_page = ctx->ring_page;
	spin_unlock(&ctx->lock);
	if (!ring_page)
		return;

	kaddr = kmap_local_page(ring_page);
	ring = kaddr;
	if (ring->magic != MD_LOG_RING_MAGIC ||
	    ring->version != MD_LOG_RING_VERSION ||
	    !ring->capacity || ring->ring_size > PAGE_SIZE ||
	    sizeof(*ring) + ring->capacity * sizeof(*entries) > ring->ring_size) {
		kunmap_local(kaddr);
		return;
	}

	entries = (struct md_shadow_log *)(ring + 1);
	head = READ_ONCE(ring->head);
	tail = smp_load_acquire(&ring->tail);
	if (tail - head > ring->capacity) {
		smp_store_release(&ring->head, tail);
		kunmap_local(kaddr);
		return;
	}

	if (head == tail) {
		kunmap_local(kaddr);
		return;
	}

	/* Pass 1: collect the set of src_cpus present in this batch. */
	bitmap_zero(src_cpus, NR_CPUS);
	for (i = head; i != tail; i++) {
		u8 src = entries[i % ring->capacity].src_cpu;

		if (src < nr_cpu_ids)
			__set_bit(src, src_cpus);
	}

	/*
	 * Pass 2: for each referenced arena, acquire its lock and apply all
	 * entries that target it.  Each arena is visited at most once, so
	 * there is no risk of lock reordering.
	 */
	for_each_set_bit(src_cpu, src_cpus, NR_CPUS) {
		struct md_arena_meta *arena;

		bitmap_zero(dirty_chunks,
			    DIV_ROUND_UP(MD_SHARED_ARENA_NR_PAGES, MD_CHUNK_PAGES));

		rcu_read_lock();
		arena = md_get_arena_rcu(src_cpu);
		if (!arena) {
			rcu_read_unlock();
			continue;
		}

		spin_lock(&arena->lock);
		for (i = head; i != tail; i++) {
			const struct md_shadow_log *entry =
				&entries[i % ring->capacity];
			int ret;

			if (entry->src_cpu != (u8)src_cpu)
				continue;

			ret = md_apply_log_entry_locked(arena, entry,
							ctx->key.mm,
							dirty_chunks);
			if (ret)
				pr_warn_ratelimited(
					"memory_delegation: drop log op=%u src_cpu=%u start=%u pages=%u ret=%d\n",
					entry->op, entry->src_cpu,
					entry->start_page, entry->nr_pages,
					ret);
		}
		md_commit_dirty_chunks_locked(arena, dirty_chunks);
		spin_unlock(&arena->lock);
		rcu_read_unlock();
	}

	smp_store_release(&ring->head, tail);
	kunmap_local(kaddr);
}

static void md_queue_sync_if_needed(struct md_mm_ctx *ctx,
				    struct task_struct *task, u64 latest_seq)
{
	bool queue = false;
	unsigned long arena_base = READ_ONCE(ctx->arena_base);

	spin_lock(&ctx->lock);
	if (arena_base && latest_seq > ctx->last_seen_gen &&
	    !ctx->sync_queued) {
		ctx->sync_queued = true;
		refcount_inc(&ctx->refs);
		queue = true;
	}
	spin_unlock(&ctx->lock);

	if (!queue)
		return;

	if (task_work_add(task, &ctx->sync_work, TWA_RESUME)) {
		spin_lock(&ctx->lock);
		ctx->sync_queued = false;
		spin_unlock(&ctx->lock);
		md_mm_ctx_put(ctx);
	}
}

// 在可睡眠上下文中实际进行当前arena PTE的修改（只改了next的PTE）
static void md_sync_task_work(struct callback_head *work)
{
	struct md_mm_ctx *ctx = container_of(work, struct md_mm_ctx, sync_work);
	struct mm_struct *mm = current->mm;
	unsigned long arena_base = READ_ONCE(ctx->arena_base);

	if (mm == ctx->key.mm && mm) {
		mmap_write_lock(mm);
		memory_delegation_sync_mm(mm, ctx->key.cpu, arena_base);
		mmap_write_unlock(mm);
	}

	spin_lock(&ctx->lock);
	ctx->sync_queued = false;
	spin_unlock(&ctx->lock);
	md_mm_ctx_put(ctx);
}

int memory_delegation_arena_register(unsigned int cpu, unsigned int nr_pages,
				     unsigned int owner_slots)
{
	struct md_arena_meta *arena;
	struct md_arena_meta *old_arena;

	if (cpu >= nr_cpu_ids)
		return -EINVAL;
	if (!nr_pages || !owner_slots || owner_slots >= MD_INVALID_SLOT)
		return -EINVAL;

	mutex_lock(&md_arena_table_lock);
	old_arena = rcu_dereference_protected(md_arenas[cpu],
				      lockdep_is_held(&md_arena_table_lock));
	if (old_arena) {
		int ret = (old_arena->nr_pages == nr_pages &&
			   old_arena->owner_slots == owner_slots) ? -EEXIST : -EBUSY;
		mutex_unlock(&md_arena_table_lock);
		return ret;
	}
	mutex_unlock(&md_arena_table_lock);

	arena = kzalloc(sizeof(*arena), GFP_KERNEL);
	if (!arena)
		return -ENOMEM;

	arena->nr_pages = nr_pages;
	arena->nr_chunks = DIV_ROUND_UP(nr_pages, MD_CHUNK_PAGES);
	arena->owner_slots = owner_slots;
	spin_lock_init(&arena->lock);

	arena->page_slot = kvzalloc(nr_pages, GFP_KERNEL);
	arena->page_free = kvzalloc(nr_pages, GFP_KERNEL);
	arena->page_owner_gen = kvcalloc(nr_pages, sizeof(*arena->page_owner_gen),
					 GFP_KERNEL);
	arena->chunk_gen = kvcalloc(arena->nr_chunks, sizeof(*arena->chunk_gen),
				    GFP_KERNEL);
	arena->owner_table = kvcalloc(owner_slots, sizeof(*arena->owner_table),
				      GFP_KERNEL);
	if (!arena->page_slot || !arena->page_free || !arena->page_owner_gen ||
	    !arena->chunk_gen || !arena->owner_table) {
		kvfree(arena->owner_table);
		kvfree(arena->chunk_gen);
		kvfree(arena->page_owner_gen);
		kvfree(arena->page_free);
		kvfree(arena->page_slot);
		kfree(arena);
		return -ENOMEM;
	}

	memset(arena->page_slot, MD_INVALID_SLOT, nr_pages);

	mutex_lock(&md_arena_table_lock);
	old_arena = rcu_dereference_protected(md_arenas[cpu],
				      lockdep_is_held(&md_arena_table_lock));
	if (old_arena) {
		mutex_unlock(&md_arena_table_lock);
		kvfree(arena->owner_table);
		kvfree(arena->chunk_gen);
		kvfree(arena->page_owner_gen);
		kvfree(arena->page_free);
		kvfree(arena->page_slot);
		kfree(arena);
		return (old_arena->nr_pages == nr_pages &&
			old_arena->owner_slots == owner_slots) ? -EEXIST : -EBUSY;
	}
	rcu_assign_pointer(md_arenas[cpu], arena);
	atomic_inc(&md_active_arenas);
	mutex_unlock(&md_arena_table_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(memory_delegation_arena_register);

void memory_delegation_arena_unregister(unsigned int cpu)
{
	struct md_arena_meta *arena;

	if (cpu >= nr_cpu_ids)
		return;

	mutex_lock(&md_arena_table_lock);
	arena = rcu_dereference_protected(md_arenas[cpu],
					  lockdep_is_held(&md_arena_table_lock));
	RCU_INIT_POINTER(md_arenas[cpu], NULL);
	if (arena)
		atomic_dec(&md_active_arenas);
	mutex_unlock(&md_arena_table_lock);

	if (!arena)
		return;

	synchronize_rcu();
	kvfree(arena->owner_table);
	kvfree(arena->chunk_gen);
	kvfree(arena->page_owner_gen);
	kvfree(arena->page_free);
	kvfree(arena->page_slot);
	kfree(arena);
}
EXPORT_SYMBOL_GPL(memory_delegation_arena_unregister);

int memory_delegation_register_ring(unsigned int cpu, unsigned long arena_base,
				     unsigned long ring_addr)
{
	struct md_arena_meta *arena;
	struct md_mm_ctx *ctx;
	u32 nr_pages = 0;
	int ret;
	unsigned long prev;

	if (cpu >= nr_cpu_ids || !arena_base || !ring_addr)
		return -EINVAL;

	ctx = md_mm_ctx_get_or_create(current->mm, cpu, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ret = md_pin_log_ring(ctx, ring_addr, &nr_pages);
	if (ret)
		goto out_put_ctx;

	arena = md_get_or_create_arena(cpu, nr_pages);
	if (!arena) {
		ret = -EINVAL;
		goto out_put_ctx;
	}

	/*
	 * ctx->arena_base is write-once.  After it becomes non-zero it must not
	 * change, so hot-path readers can use READ_ONCE() without taking ctx->lock.
	 */
	prev = cmpxchg(&ctx->arena_base, 0UL, arena_base);
	if (prev && prev != arena_base) {
		ret = -EINVAL;
		goto out_put_ctx;
	}

	spin_lock(&ctx->lock);
	if (ctx->ring_page && arena->nr_pages != nr_pages)
		ret = -EINVAL;
	spin_unlock(&ctx->lock);

	/*
	 * Attach a dedicated VMA fault handler to the arena mapping, so the
	 * ownership check and SIGSEGV decision are made in vm_ops->fault instead
	 * of the generic anonymous fault path.
	 */
	if (!ret) {
		mmap_write_lock(current->mm);
		ret = md_install_vma_ops(current->mm, cpu, arena_base, arena->nr_pages);
		mmap_write_unlock(current->mm);
	}
out_put_ctx:
	md_mm_ctx_put(ctx);
	return ret;
}
EXPORT_SYMBOL_GPL(memory_delegation_register_ring);

int memory_delegation_submit_log(unsigned int cpu, unsigned long arena_base,
				 const struct md_shadow_log *log,
				 unsigned int nr_entries)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(memory_delegation_submit_log);

/*
 * Called from context_switch() on the CPU performing the switch.
 *
 * switch-out (prev): drain only the current CPU's ring for prev->mm.
 *   md_drain_log_ring routes each entry to the arena named by entry->src_cpu,
 *   so cross-core free operations are applied to the correct arena even though
 *   only one ring is read.  Rings on other CPUs belonging to prev->mm are
 *   drained when those CPUs switch out their own prev tasks.
 *
 * switch-in (next): queue a page-table sync only for the current CPU's arena.
 *   If next->mm has stale mappings in other CPUs' arenas, those will be caught
 *   either when next's threads switch in on those CPUs or at page-fault time.
 */
void memory_delegation_on_context_switch(struct task_struct *prev,
					 struct task_struct *next)
{
	unsigned int cpu;

	/* Fast path: no arena registered system-wide, skip all locking. */
	if (!atomic_read(&md_active_arenas))
		return;

	cpu = smp_processor_id();

	if (prev && prev->mm) {
		struct md_mm_ctx *prev_ctx;
		unsigned long arena_base;

		prev_ctx = md_mm_ctx_lookup_get(prev->mm, cpu);
		if (prev_ctx) {
			arena_base = READ_ONCE(prev_ctx->arena_base);

			if (arena_base)
				md_drain_log_ring(prev_ctx);

			md_mm_ctx_put(prev_ctx);
		}
	}

	if (!next || !next->mm)
		return;

	{
		struct md_mm_ctx *ctx;
		struct md_arena_meta *arena;

		ctx = md_mm_ctx_lookup_get(next->mm, cpu);
		if (ctx) {
			rcu_read_lock();
			arena = md_get_arena_rcu(cpu);
			if (arena)
				md_queue_sync_if_needed(
					ctx, next,
					READ_ONCE(arena->global_commit_seq));
			rcu_read_unlock();
			md_mm_ctx_put(ctx);
		}
	}
}
EXPORT_SYMBOL_GPL(memory_delegation_on_context_switch);

/*
	对chunk内每个页面的dirty状态进行检查
*/
static int md_snapshot_chunk_state(struct mm_struct *mm, unsigned int cpu,
				   u32 chunk, u64 last_seen, bool accessible[],
				   bool free_pages[],
				   u32 *out_start_page, u32 *out_end_page)
{
	struct md_arena_meta *arena;
	u32 start_page;
	u32 end_page;
	u32 page;

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu);
	if (!arena) {
		rcu_read_unlock();
		return -ENOENT;
	}

	// chunk 已经是最新，直接返回
	if (chunk >= arena->nr_chunks ||
	    READ_ONCE(arena->chunk_gen[chunk]) <= last_seen) {
		rcu_read_unlock();
		return 1;
	}

	start_page = chunk * MD_CHUNK_PAGES;
	end_page = min_t(u32, arena->nr_pages, start_page + MD_CHUNK_PAGES);

	spin_lock(&arena->lock);
	if (arena->chunk_gen[chunk] <= last_seen) {
		spin_unlock(&arena->lock);
		rcu_read_unlock();
		return 1;
	}
	for (page = start_page; page < end_page; page++) {
		free_pages[page - start_page] =
			arena->page_slot[page] == MD_INVALID_SLOT;
		accessible[page - start_page] =
			md_page_accessible_to_mm_locked(arena, page, mm);
	}
	spin_unlock(&arena->lock);
	rcu_read_unlock();

	*out_start_page = start_page;
	*out_end_page = end_page;
	return 0;
}

/*
	实际处理修改 PTE 的函数
*/
int memory_delegation_sync_mm(struct mm_struct *mm, unsigned int cpu,
			      unsigned long arena_base)
{
	struct md_arena_meta *arena;
	struct md_mm_ctx *ctx;
	u64 last_seen;
	u64 target_seq;
	u32 nr_chunks;
	u32 chunk;
	bool accessible[MD_CHUNK_PAGES];
	bool free_pages[MD_CHUNK_PAGES];
	int ret = 0;

	if (!mm || cpu >= nr_cpu_ids || !arena_base)
		return -EINVAL;

	mmap_assert_write_locked(mm);

	ctx = md_mm_ctx_lookup_get(mm, cpu);
	if (!ctx)
		return 0;

	spin_lock(&ctx->lock);
	last_seen = ctx->last_seen_gen;
	spin_unlock(&ctx->lock);

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu);
	if (!arena) {
		rcu_read_unlock();
		ret = -ENOENT;
		goto out_put_ctx;
	}
	nr_chunks = arena->nr_chunks;
	target_seq = READ_ONCE(arena->global_commit_seq);
	rcu_read_unlock();

	if (target_seq <= last_seen)
		goto out_put_ctx;

	for (chunk = 0; chunk < nr_chunks; chunk++) {
		u32 start_page;
		u32 end_page;
		u32 page;
		u32 run_start = 0;
		bool in_run = false;
		u32 free_run_start = 0;
		bool in_free_run = false;
		int snap_ret;

		snap_ret = md_snapshot_chunk_state(mm, cpu, chunk, last_seen,
						   accessible, free_pages,
						   &start_page, &end_page);
		if (snap_ret == 1)
			continue;
		if (snap_ret) {
			ret = snap_ret;
			break;
		}

		for (page = start_page; page < end_page; page++) {
			bool page_accessible = accessible[page - start_page];

			// in_run状态位表示正在处理一个不可访问页面的连续段，进行unmap
			if (!page_accessible && !in_run) {
				in_run = true;
				run_start = page;
				continue;
			}
			if (page_accessible && in_run) {
				unsigned long start = arena_base +
					(unsigned long)run_start * PAGE_SIZE;
				unsigned long end = arena_base +
					(unsigned long)page * PAGE_SIZE;
				md_unmap_range_locked(mm, start, end);
				in_run = false;
			}

			// in_free_run表示正在处理一个空闲页面的连续段
			if (free_pages[page - start_page] && !in_free_run) {
				in_free_run = true;
				free_run_start = page;
				continue;
			}
			if (!free_pages[page - start_page] && in_free_run) {
				unsigned long start = arena_base +
					(unsigned long)free_run_start * PAGE_SIZE;
				unsigned long end = arena_base +
					(unsigned long)page * PAGE_SIZE;

				ret = md_prefault_range_locked(mm, start, end);
				if (ret)
					break;
				in_free_run = false;
			}
		}

		if (ret)
			break;

		// 尾端剩余页面
		if (in_run) {
			unsigned long start = arena_base +
				(unsigned long)run_start * PAGE_SIZE;
			unsigned long end = arena_base +
				(unsigned long)end_page * PAGE_SIZE;
			md_unmap_range_locked(mm, start, end);
		}

		if (in_free_run) {
			unsigned long start = arena_base +
				(unsigned long)free_run_start * PAGE_SIZE;
			unsigned long end = arena_base +
				(unsigned long)end_page * PAGE_SIZE;

			ret = md_prefault_range_locked(mm, start, end);
			if (ret)
				break;
		}
	}

	spin_lock(&ctx->lock);
	if (!ret && target_seq > ctx->last_seen_gen)
		ctx->last_seen_gen = target_seq;
	spin_unlock(&ctx->lock);

out_put_ctx:
	md_mm_ctx_put(ctx);
	return ret;
}
EXPORT_SYMBOL_GPL(memory_delegation_sync_mm);

int memory_delegation_fork_mm(struct task_struct *task, struct mm_struct *new_mm,
			      struct mm_struct *old_mm)
{
	unsigned int cpu;

	if (!task || !new_mm || !old_mm)
		return -EINVAL;

	for_each_possible_cpu(cpu) {
		struct md_mm_ctx *old_ctx;
		struct md_mm_ctx *new_ctx;
		struct md_arena_meta *arena;
		unsigned long arena_base;

		old_ctx = md_mm_ctx_lookup_get(old_mm, cpu);
		if (!old_ctx)
			continue;

		arena_base = READ_ONCE(old_ctx->arena_base);
		md_mm_ctx_put(old_ctx);

		if (!arena_base)
			continue;

		new_ctx = md_mm_ctx_get_or_create(new_mm, cpu, GFP_KERNEL);
		if (!new_ctx)
			return -ENOMEM;

		/*
		 * Carry over the parent's write-once arena_base.  If userspace
		 * later tries to register a different base for this (mm,cpu),
		 * memory_delegation_register_ring() will reject it.
		 */
		WRITE_ONCE(new_ctx->arena_base, arena_base);
		spin_lock(&new_ctx->lock);
		new_ctx->last_seen_gen = 0;
		spin_unlock(&new_ctx->lock);

		rcu_read_lock();
		arena = md_get_arena_rcu(cpu);
		if (arena)
			md_queue_sync_if_needed(new_ctx, task,
					READ_ONCE(arena->global_commit_seq));
		rcu_read_unlock();
		md_mm_ctx_put(new_ctx);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(memory_delegation_fork_mm);

void memory_delegation_mm_release(struct mm_struct *mm)
{
	unsigned int cpu;
	unsigned int bkt;
	struct md_mm_ctx *ctx;
	struct hlist_node *tmp;

	if (!mm)
		return;

	for_each_possible_cpu(cpu) {
		struct md_arena_meta *arena;
		unsigned long *dirty_chunks;
		unsigned int nr_chunks;
		u32 page;

		rcu_read_lock();
		arena = md_get_arena_rcu(cpu);
		if (!arena) {
			rcu_read_unlock();
			continue;
		}
		nr_chunks = arena->nr_chunks;
		rcu_read_unlock();

		dirty_chunks = bitmap_zalloc(nr_chunks, GFP_KERNEL);
		if (!dirty_chunks)
			continue;

		rcu_read_lock();
		arena = md_get_arena_rcu(cpu);
		if (!arena || arena->nr_chunks != nr_chunks) {
			rcu_read_unlock();
			bitmap_free(dirty_chunks);
			continue;
		}

		spin_lock(&arena->lock);
		for (page = 0; page < arena->nr_pages; page++) {
			u16 slot;

			if (!md_page_belongs_to_mm_locked(arena, page, mm))
				continue;

			slot = arena->page_slot[page];
			arena->page_slot[page] = MD_INVALID_SLOT;
			arena->page_owner_gen[page] = 0;
			if (slot < arena->owner_slots &&
			    arena->owner_table[slot].ref_pages)
				arena->owner_table[slot].ref_pages--;
			md_put_owner_slot_locked(arena, slot);
			__set_bit(md_chunk_of_page(page), dirty_chunks);
		}
		md_commit_dirty_chunks_locked(arena, dirty_chunks);
		spin_unlock(&arena->lock);
		rcu_read_unlock();

		bitmap_free(dirty_chunks);
	}

	spin_lock(&md_mm_ctx_table_lock);
	hash_for_each_safe(md_mm_ctx_table, bkt, tmp, ctx, node) {
		if (ctx->key.mm != mm)
			continue;
		hash_del(&ctx->node);
		ctx->dead = true;
		md_mm_ctx_put(ctx);
	}
	spin_unlock(&md_mm_ctx_table_lock);
}
EXPORT_SYMBOL_GPL(memory_delegation_mm_release);

bool memory_delegation_fault_allowed(struct mm_struct *mm,
				     unsigned long address,
				     const struct vm_area_struct *vma)
{
	unsigned int cpu;

	if (!mm || !vma)
		return true;

	/* Arena VMA is handled by md_arena_vm_ops->fault. */
	if (md_vma_is_wrapped(vma))
		return true;

	/* Fast path: no arena registered system-wide, skip all locking. */
	if (!atomic_read(&md_active_arenas))
		return true;

	for_each_possible_cpu(cpu) {
		struct md_mm_ctx *ctx;
		struct md_arena_meta *arena;
		unsigned long arena_base;
		unsigned long offset;
		u32 page_idx;
		bool allowed;

		ctx = md_mm_ctx_lookup_get(mm, cpu);
		if (!ctx)
			continue;

		arena_base = READ_ONCE(ctx->arena_base);
		if (!arena_base || address < arena_base) {
			md_mm_ctx_put(ctx);
			continue;
		}

		rcu_read_lock();
		arena = md_get_arena_rcu(cpu);
		if (!arena) {
			rcu_read_unlock();
			md_mm_ctx_put(ctx);
			continue;
		}

		offset = address - arena_base;
		page_idx = offset >> PAGE_SHIFT;
		if (page_idx >= arena->nr_pages) {
			rcu_read_unlock();
			md_mm_ctx_put(ctx);
			continue;
		}

		allowed = md_page_belongs_to_mm(arena, page_idx, mm);
		rcu_read_unlock();
		md_mm_ctx_put(ctx);
		return allowed;
	}

	return true;
}
EXPORT_SYMBOL_GPL(memory_delegation_fault_allowed);
