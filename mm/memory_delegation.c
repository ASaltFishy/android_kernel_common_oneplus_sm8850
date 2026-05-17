// SPDX-License-Identifier: GPL-2.0
#include <linux/bitmap.h>
#include <linux/hash.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/limits.h>
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
#include <linux/string.h>
#include <linux/task_work.h>

#include "memory_delegation_debugfs.h"

#define MD_INVALID_SLOT 0xff
#define MD_MM_CTX_HASH_BITS 10
#define MD_ACTIVE_MM_HASH_BITS 10
#define MD_SHARED_ARENA_CAPACITY	(512UL * 1024 * 1024)
#define MD_SHARED_ARENA_HEADER_SIZE	PAGE_SIZE
#define MD_SHARED_ARENA_NR_PAGES	\
	((MD_SHARED_ARENA_CAPACITY - MD_SHARED_ARENA_HEADER_SIZE) / PAGE_SIZE)
#define MD_SHARED_ARENA_NR_CHUNKS_MAX	\
	DIV_ROUND_UP(MD_SHARED_ARENA_NR_PAGES, MD_CHUNK_PAGES_MIN)
#define MD_DEFAULT_OWNER_SLOTS		254
#define MD_SHARED_ARENA_FREELIST_END	U32_MAX
#define MD_SHARED_ARENA_FREE_MAGIC	0xF4EEB10cU
#define MD_SHARED_ARENA_READY_VERSION	2U
#define MD_USER_ARENA_LOCK_RETRIES	1000000U
#define MD_PTE_DELTA_RUN_CAPACITY	4096U

enum md_sync_state {
	MD_SYNC_IDLE = 0,
	MD_SYNC_QUEUED,
	MD_SYNC_RUNNING,
	MD_SYNC_RERUN,
};

enum md_pte_delta_op {
	MD_PTE_DELTA_REVOKE_NON_OWNER = 1,
	MD_PTE_DELTA_PREFAULT_FREE = 2,
};

struct md_owner_entry {
	struct mm_struct *mm;
	u32 gen;
	u32 ref_pages;
	bool valid;
};

struct md_user_free_block {
	u32 prev_page_off;
	u32 next_page_off;
	u32 size_in_pages;
	u32 magic;
};

struct md_user_arena_header {
	u32 lock;
	u32 version;
	u32 bump_offset_in_pages;
	u32 total_data_pages;
	u32 freelist_head_page_off;
	u32 free_count;
	unsigned long total_donated_bytes;
	unsigned long total_retrieved_bytes;
	u32 donate_count;
	u32 retrieve_count;
};

struct md_page_run {
	u32 start;
	u32 nr_pages;
};

struct md_pte_delta_run {
	u64 serial;
	u64 seq;
	u8 op;
	u8 owner_slot;
	u16 reserved;
	u32 owner_gen;
	u32 start_page;
	u32 nr_pages;
};

struct md_arena_meta {
	spinlock_t lock;
	u32 nr_pages;
	u32 nr_chunks;
	u32 owner_slots;
	u64 global_commit_seq;
	u64 delta_next_serial;
	u64 delta_loss_seq;
	u8 *page_slot;
	u8 *page_free;
	u8 *page_prefault;
	u32 *page_owner_gen;
	u64 *chunk_gen;
	struct md_owner_entry *owner_table;
	struct md_pte_delta_run *delta_runs;
};

struct md_mm_ctx_key {
	struct mm_struct *mm;
	u16 cpu;
};

struct md_mm_ctx {
	struct hlist_node node;
	refcount_t refs;
	struct md_mm_ctx_key key;
	struct callback_head sync_work;
	unsigned long arena_base;
	unsigned long ring_addr;
	struct page *ring_page;
	u64 last_seen_gen;
	unsigned int sync_state;
	bool active;
	bool dead;
};

struct md_active_mm {
	struct hlist_node node;
	struct rcu_head rcu;
	struct mm_struct *mm;
	u32 nr_ctxs;
};

static DEFINE_MUTEX(md_arena_table_lock);
static DEFINE_HASHTABLE(md_mm_ctx_table, MD_MM_CTX_HASH_BITS);
static DEFINE_HASHTABLE(md_active_mm_table, MD_ACTIVE_MM_HASH_BITS);
static DEFINE_SPINLOCK(md_mm_ctx_table_lock);
static struct md_arena_meta __rcu *md_arenas[NR_CPUS];
static unsigned int md_chunk_pages __read_mostly = MD_CHUNK_PAGES_DEFAULT;

static inline u32 md_current_chunk_pages(void)
{
	return READ_ONCE(md_chunk_pages);
}

struct md_vma_wrap {
	refcount_t refs;
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

static bool md_drain_log_ring(struct md_mm_ctx *ctx,
			      struct md_switch_cycle_stats *stats);
static int md_memory_delegation_sync_mm(struct mm_struct *mm, unsigned int cpu,
					unsigned long arena_base,
					bool *out_pte_modified,
					struct md_pte_sync_detail *detail);

/*
 * Number of arenas currently registered.  Incremented under md_arena_table_lock
 * in arena_register, decremented in arena_unregister.  Read locklessly as a
 * fast-path guard: if zero, no delegation is active and all hot-path hooks can
 * return immediately without acquiring any other lock.
 */
static atomic_t md_active_arenas = ATOMIC_INIT(0);
static atomic_t md_active_mms = ATOMIC_INIT(0);

unsigned int md_debugfs_current_chunk_pages(void)
{
	return md_current_chunk_pages();
}

int md_debugfs_active_arenas(void)
{
	return atomic_read(&md_active_arenas);
}

int md_debugfs_active_mms(void)
{
	return atomic_read(&md_active_mms);
}

int md_debugfs_set_chunk_pages(unsigned int value)
{
	mutex_lock(&md_arena_table_lock);
	if (atomic_read(&md_active_arenas) || atomic_read(&md_active_mms)) {
		mutex_unlock(&md_arena_table_lock);
		return -EBUSY;
	}
	WRITE_ONCE(md_chunk_pages, value);
	mutex_unlock(&md_arena_table_lock);
	return 0;
}


/* 功能：将 arena 内页索引换算为 chunk 索引；调用时机：标记或扫描脏 chunk 时调用。 */
static inline u32 md_chunk_of_page(u32 page_idx)
{
	return page_idx / md_current_chunk_pages();
}

/* 功能：无锁检查指定页当前是否归属于某个 mm；调用时机：RCU 读侧的 fault 权限判断路径调用。 */
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

/* 功能：在 arena 锁保护下检查指定页是否归属于某个 mm；调用时机：提交日志、同步页表和释放 mm 资源时调用。 */
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

/* 功能：在 arena 锁保护下判断指定页对某个 mm 是否可访问；调用时机：构造页表同步快照时调用。 */
static inline bool md_page_accessible_to_mm_locked(const struct md_arena_meta *arena,
						   u32 page_idx,
						   struct mm_struct *mm)
{
	if (arena->page_slot[page_idx] == MD_INVALID_SLOT)
		return true;

	return md_page_belongs_to_mm_locked(arena, page_idx, mm);
}

/* 功能：计算 per-(mm,cpu) 上下文哈希值；调用时机：访问 md_mm_ctx_table 时调用。 */
static inline unsigned long md_mm_ctx_hash(const struct md_mm_ctx_key *key)
{
	return hash_long(hash_ptr(key->mm, MD_MM_CTX_HASH_BITS) ^ key->cpu,
			 MD_MM_CTX_HASH_BITS);
}

/* 功能：在全局 ctx 表锁保护下查找 per-(mm,cpu) 上下文；调用时机：获取、创建、引用或销毁 ctx 时调用。 */
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

/* 功能：计算 active mm 哈希值；调用时机：访问 active mm 表时调用。 */
static unsigned long md_active_mm_hash(struct mm_struct *mm)
{
	return hash_ptr(mm, MD_ACTIVE_MM_HASH_BITS);
}

/* 功能：在全局 ctx 表锁保护下查找活跃 mm 记录；调用时机：注册、注销或快速判断 mm 是否参与 delegation 时调用。 */
static struct md_active_mm *md_active_mm_lookup_locked(struct mm_struct *mm)
{
	struct md_active_mm *active;

	hash_for_each_possible(md_active_mm_table, active, node,
			       md_active_mm_hash(mm)) {
		if (active->mm == mm)
			return active;
	}

	return NULL;
}

/* 功能：增加或创建某个 mm 的活跃 delegation 计数；调用时机：ctx 首次激活时在全局 ctx 表锁内调用。 */
static bool md_active_mm_get_locked(struct mm_struct *mm,
				    struct md_active_mm *new_active)
{
	struct md_active_mm *active;

	active = md_active_mm_lookup_locked(mm);
	if (active) {
		active->nr_ctxs++;
		kfree(new_active);
		return true;
	}

	if (!new_active)
		return false;

	new_active->mm = mm;
	new_active->nr_ctxs = 1;
	atomic_inc(&md_active_mms);
	hash_add_rcu(md_active_mm_table, &new_active->node,
		     md_active_mm_hash(mm));
	return true;
}

/* 功能：将 per-(mm,cpu) ctx 标记为活跃并登记其 mm；调用时机：用户态注册 ring 和 arena 后调用。 */
static int md_mm_ctx_activate(struct md_mm_ctx *ctx, gfp_t gfp)
{
	struct md_active_mm *new_active;
	bool activated = false;
	int ret = 0;

	if (!ctx)
		return -EINVAL;

	spin_lock(&md_mm_ctx_table_lock);
	if (ctx->dead) {
		spin_unlock(&md_mm_ctx_table_lock);
		return -EINVAL;
	}
	if (ctx->active) {
		spin_unlock(&md_mm_ctx_table_lock);
		return 0;
	}
	spin_unlock(&md_mm_ctx_table_lock);

	new_active = kzalloc(sizeof(*new_active), gfp);
	if (!new_active)
		return -ENOMEM;

	spin_lock(&md_mm_ctx_table_lock);
	if (!ctx->dead && !ctx->active) {
		if (!md_active_mm_get_locked(ctx->key.mm, new_active)) {
			spin_unlock(&md_mm_ctx_table_lock);
			kfree(new_active);
			return -ENOMEM;
		}
		ctx->active = true;
		activated = true;
	}
	if (ctx->dead)
		ret = -EINVAL;
	spin_unlock(&md_mm_ctx_table_lock);

	if (!activated)
		kfree(new_active);

	return ret;
}

/* 功能：减少某个 mm 的活跃 delegation 计数并在归零时移除记录；调用时机：mm 释放并注销其 ctx 时调用。 */
static void md_active_mm_put_locked(struct mm_struct *mm)
{
	struct md_active_mm *active;

	active = md_active_mm_lookup_locked(mm);
	if (!active)
		return;

	if (--active->nr_ctxs)
		return;

	hash_del_rcu(&active->node);
	atomic_dec(&md_active_mms);
	kfree_rcu(active, rcu);
}

/* 功能：RCU 读侧判断 mm 是否存在活跃 delegation 上下文；调用时机：context switch、fault 和 mm release 的快速路径调用。 */
static bool md_mm_has_active_ctx(struct mm_struct *mm)
{
	struct md_active_mm *active_mm;
	bool active;

	if (!mm || !atomic_read(&md_active_mms))
		return false;

	active = false;
	rcu_read_lock();
	hash_for_each_possible_rcu(md_active_mm_table, active_mm, node,
				   md_active_mm_hash(mm)) {
		if (active_mm->mm == mm) {
			active = true;
			break;
		}
	}
	rcu_read_unlock();

	return active;
}

/* 功能：释放 md_mm_ctx 引用并在引用归零时回收资源；调用时机：所有 get/create/lookup 路径使用完 ctx 后调用。 */
static void md_mm_ctx_put(struct md_mm_ctx *ctx)
{
	if (refcount_dec_and_test(&ctx->refs)) {
		struct page *ring_page = READ_ONCE(ctx->ring_page);

		if (ring_page)
			unpin_user_page(ring_page);
		kfree(ctx);
	}
}

static void md_sync_task_work(struct callback_head *work);

/* 功能：获取或创建 per-(mm,cpu) ctx 并增加引用；调用时机：注册 ring 或 fork 继承 arena 信息时调用。 */
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

/* 功能：查找已有 per-(mm,cpu) ctx 并增加引用；调用时机：context switch、sync、fault 和 fork 查询现有状态时调用。 */
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

/* 功能：在 RCU 读侧获取指定 CPU 的 arena 元数据；调用时机：所有需要读取 md_arenas[cpu] 的路径调用。 */
static struct md_arena_meta *md_get_arena_rcu(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids)
		return NULL;

	return rcu_dereference(md_arenas[cpu]);
}

/* 功能：判断 VMA 是否已被 memory delegation 的 vm_ops 包装；调用时机：安装 vm_ops 或 fault 快速分流时调用。 */
static inline bool md_vma_is_wrapped(const struct vm_area_struct *vma)
{
	return vma && vma->vm_ops == &md_arena_vm_ops;
}

/* 功能：从已包装 VMA 中取出 delegation 私有包装信息；调用时机：VMA open/close/fault 和 page_mkwrite 回调中调用。 */
static struct md_vma_wrap *md_vma_wrap_get(const struct vm_area_struct *vma)
{
	if (!vma)
		return NULL;
	if (vma->vm_ops != &md_arena_vm_ops)
		return NULL;
	return (struct md_vma_wrap *)vma->vm_private_data;
}

/* 功能：判断 wrapped arena VMA 上的 fault 地址是否允许当前 mm 访问；调用时机：arena VMA fault 处理前调用。 */
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

	/*
	 * The wrapped shmem VMA starts at the SharedArena header page, while
	 * wrap->arena_base points at the ownership-tracked data area after that
	 * header. The header contains shared userspace allocator metadata and is
	 * intentionally outside page_slot[] protection.
	 */
	if (address < wrap->arena_base)
		return true;

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

/* 功能：判断写 fault 地址是否由当前 mm 拥有或者为空闲页；调用时机：arena VMA page_mkwrite 回调中调用。 */
static bool md_fault_addr_owned(const struct vm_area_struct *vma,
				unsigned long address)
{
	return md_fault_addr_allowed(vma, address);
}

/* 功能：为新建或分裂后的 wrapped VMA 增加包装引用并转发原始 open；调用时机：VMA 被创建、复制或 split 时由 mm 调用。 */
static void md_arena_vma_open(struct vm_area_struct *vma)
{
	struct md_vma_wrap *wrap = md_vma_wrap_get(vma);

	/*
	 * VMA split/dup copies vm_private_data and vm_ops. The wrapper is
	 * immutable after installation, so sharing it with a refcount avoids
	 * allocation failure in this no-return-value callback.
	 */
	if (wrap && wrap->owner_vma != vma)
		refcount_inc(&wrap->refs);

	if (wrap && wrap->orig_ops && wrap->orig_ops->open)
		wrap->orig_ops->open(vma);
}

/* 功能：转发原始 close 并释放 wrapped VMA 的私有包装；调用时机：VMA 销毁或解除映射时由 mm 调用。 */
static void md_arena_vma_close(struct vm_area_struct *vma)
{
	struct md_vma_wrap *wrap = md_vma_wrap_get(vma);

	if (!wrap)
		return;

	if (wrap->orig_ops && wrap->orig_ops->close)
		wrap->orig_ops->close(vma);

	if (refcount_dec_and_test(&wrap->refs))
		kfree(wrap);
	vma->vm_private_data = NULL;
}

/* 功能：对 arena VMA fault 执行 ownership 检查后转发给原始 fault；调用时机：用户访问 arena 映射触发缺页时调用。 */
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

/* 功能：对 arena 写保护 fault 执行 ownership 检查后转发原始 page_mkwrite；调用时机：用户写入只读或 CoW arena 页面时调用。 */
static vm_fault_t md_arena_vma_page_mkwrite(struct vm_fault *vmf)
{
	struct md_vma_wrap *wrap = md_vma_wrap_get(vmf->vma);

	if (unlikely(!md_fault_addr_owned(vmf->vma, vmf->address)))
		return VM_FAULT_SIGSEGV;

	if (!wrap || !wrap->orig_ops || !wrap->orig_ops->page_mkwrite)
		return 0;

	return wrap->orig_ops->page_mkwrite(vmf);
}

/* 功能：为用户态 SharedArena 映射安装 delegation 专用 vm_ops 包装；调用时机：register_ring 完成 arena 元数据准备后调用。 */
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
	refcount_set(&wrap->refs, 1);

	vma->vm_private_data = wrap;
	vma->vm_ops = &md_arena_vm_ops;

	/* Ensure open() is run for consistency with other vm_ops users. */
	md_arena_vma_open(vma);
	return 0;
}

/* 功能：获取指定 CPU 的 arena 或按页数创建默认 arena；调用时机：用户态首次注册该 CPU ring 时调用。 */
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


/* 功能：校验并长期 pin 用户态 shadow log ring 页；调用时机：register_ring 绑定用户态 ring 时调用。 */
static int md_pin_log_ring(struct md_mm_ctx *ctx, unsigned long ring_addr,
			   u32 *out_nr_pages)
{
	struct page *page;
	struct md_log_ring *ring;
	u32 max_capacity;
	void *kaddr;
	long pinned;
	int ret = 0;
	struct page *old_page;

	if (!ctx || !ring_addr || !PAGE_ALIGNED(ring_addr) || !out_nr_pages)
		return -EINVAL;
	old_page = READ_ONCE(ctx->ring_page);
	if (old_page) {
		if (READ_ONCE(ctx->ring_addr) != ring_addr)
			return -EBUSY;
		kaddr = kmap_local_page(old_page);
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

	old_page = cmpxchg(&ctx->ring_page, (struct page *)NULL, page);
	if (old_page) {
		unpin_user_page(page);
	} else {
		WRITE_ONCE(ctx->ring_addr, ring_addr);
	}
	return 0;
}

/* 功能：在 mmap 写锁保护下撤销指定虚拟地址范围的 PTE；调用时机：同步页表时处理不可访问页段调用。 */
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

/* 功能：在 mmap 写锁保护下批量预先 fault 指定虚拟地址范围；调用时机：同步页表时为 freelist 元数据页或 bump 预提交页建映射调用。 */
static int md_prefault_range_locked(struct mm_struct *mm, unsigned long start,
				    unsigned long end)
{
	unsigned long cursor = start;

	while (cursor < end) {
		struct vm_area_struct *vma;
		unsigned long prefault_end;
		unsigned long nr_pages;
		long ret;

		vma = find_vma(mm, cursor);
		if (!vma)
			return -EFAULT;
		if (cursor < vma->vm_start)
			cursor = vma->vm_start;
		if (cursor >= end)
			break;

		prefault_end = min(end, vma->vm_end);
		nr_pages = (prefault_end - cursor) >> PAGE_SHIFT;
		while (nr_pages) {
			ret = get_user_pages_remote(mm, cursor, nr_pages,
						    FOLL_WRITE, NULL, NULL);
			if (ret < 0)
				return ret;
			if (!ret)
				return -EFAULT;
			cursor += ret << PAGE_SHIFT;
			nr_pages -= ret;
		}
	}

	return 0;
}

static unsigned long md_user_free_block_addr(unsigned long arena_base,
					     u32 page_off)
{
	return arena_base + (unsigned long)page_off * PAGE_SIZE;
}

static int md_user_read(struct mm_struct *mm, unsigned long addr,
			void *buf, size_t len)
{
	int copied;

	if (len > INT_MAX)
		return -EINVAL;

	copied = access_remote_vm(mm, addr, buf, len, 0);
	return copied == len ? 0 : -EFAULT;
}

static int md_user_write(struct mm_struct *mm, unsigned long addr,
			 const void *buf, size_t len)
{
	int copied;

	if (len > INT_MAX)
		return -EINVAL;

	copied = access_remote_vm(mm, addr, (void *)buf, len, FOLL_WRITE);
	return copied == len ? 0 : -EFAULT;
}

static int md_user_write_u32(struct mm_struct *mm, unsigned long addr, u32 value)
{
	return md_user_write(mm, addr, &value, sizeof(value));
}

static int md_user_write_ulong(struct mm_struct *mm, unsigned long addr,
			       unsigned long value)
{
	return md_user_write(mm, addr, &value, sizeof(value));
}

static int md_user_read_free_block(struct mm_struct *mm, unsigned long arena_base,
				   u32 page_off, struct md_user_free_block *block)
{
	return md_user_read(mm, md_user_free_block_addr(arena_base, page_off),
			    block, sizeof(*block));
}

static int md_user_write_free_block(struct mm_struct *mm,
				    unsigned long arena_base, u32 page_off,
				    const struct md_user_free_block *block)
{
	return md_user_write(mm, md_user_free_block_addr(arena_base, page_off),
			     block, sizeof(*block));
}

static int md_user_arena_lock(struct mm_struct *mm, unsigned long arena_base,
			      struct page **out_page)
{
	unsigned long header_addr;
	struct page *page;
	unsigned int retry;
	long pinned;
	int locked = 1;

	if (!arena_base || arena_base < MD_SHARED_ARENA_HEADER_SIZE)
		return -EINVAL;

	header_addr = arena_base - MD_SHARED_ARENA_HEADER_SIZE;
	mmap_read_lock(mm);
	pinned = get_user_pages_remote(mm, header_addr, 1, FOLL_WRITE,
				       &page, &locked);
	if (locked)
		mmap_read_unlock(mm);
	if (pinned < 0)
		return pinned;
	if (pinned != 1) {
		if (pinned > 0)
			put_page(page);
		return -EFAULT;
	}

	for (retry = 0; retry < MD_USER_ARENA_LOCK_RETRIES; retry++) {
		void *kaddr = kmap_local_page(page);
		struct md_user_arena_header *hdr;
		int old;

		hdr = (struct md_user_arena_header *)((char *)kaddr +
						      offset_in_page(header_addr));
		old = atomic_cmpxchg_acquire((atomic_t *)&hdr->lock, 0, 1);
		kunmap_local(kaddr);

		if (!old) {
			*out_page = page;
			return 0;
		}

		if (!(retry & 0xff))
			cond_resched();
		else
			cpu_relax();
	}

	put_page(page);
	return -EBUSY;
}

static void md_user_arena_unlock(struct page *page, unsigned long arena_base)
{
	unsigned long header_addr = arena_base - MD_SHARED_ARENA_HEADER_SIZE;
	void *kaddr = kmap_local_page(page);
	struct md_user_arena_header *hdr;

	hdr = (struct md_user_arena_header *)((char *)kaddr +
					      offset_in_page(header_addr));
	atomic_set_release((atomic_t *)&hdr->lock, 0);
	kunmap_local(kaddr);
	put_page(page);
}

static int md_user_freelist_insert_locked(struct mm_struct *mm,
					  unsigned long arena_base,
					  u32 start, u32 nr_pages)
{
	unsigned long header_addr = arena_base - MD_SHARED_ARENA_HEADER_SIZE;
	struct md_user_arena_header hdr;
	struct md_user_free_block new_block;
	u32 prev = MD_SHARED_ARENA_FREELIST_END;
	u32 next;
	u32 walk_guard = 0;
	int ret;

	if (!nr_pages)
		return 0;

	ret = md_user_read(mm, header_addr, &hdr, sizeof(hdr));
	if (ret)
		return ret;
	if (hdr.version < MD_SHARED_ARENA_READY_VERSION ||
	    hdr.total_data_pages < start ||
	    nr_pages > hdr.total_data_pages - start) {
		pr_warn_ratelimited(
			"memory_delegation: userspace freelist header invalid base=%lx start=%u pages=%u version=%u total=%u head=%u count=%u\n",
			arena_base, start, nr_pages, hdr.version,
			hdr.total_data_pages, hdr.freelist_head_page_off,
			hdr.free_count);
		return -EINVAL;
	}

	next = hdr.freelist_head_page_off;
	while (next != MD_SHARED_ARENA_FREELIST_END) {
		struct md_user_free_block block;

		if (next >= hdr.total_data_pages || walk_guard++ >= hdr.free_count) {
			pr_warn_ratelimited(
				"memory_delegation: userspace freelist walk invalid base=%lx start=%u pages=%u next=%u total=%u guard=%u count=%u head=%u\n",
				arena_base, start, nr_pages, next,
				hdr.total_data_pages, walk_guard, hdr.free_count,
				hdr.freelist_head_page_off);
			return -EINVAL;
		}
		if (next > start)
			break;

		ret = md_user_read_free_block(mm, arena_base, next, &block);
		if (ret)
			return ret;
		if (block.magic != MD_SHARED_ARENA_FREE_MAGIC ||
		    !block.size_in_pages ||
		    block.size_in_pages > hdr.total_data_pages - next) {
			pr_warn_ratelimited(
				"memory_delegation: userspace freelist block invalid base=%lx off=%u prev=%u next=%u size=%u magic=%x total=%u\n",
				arena_base, next, block.prev_page_off,
				block.next_page_off, block.size_in_pages,
				block.magic, hdr.total_data_pages);
			return -EINVAL;
		}

		prev = next;
		next = block.next_page_off;
	}

	new_block.prev_page_off = prev;
	new_block.next_page_off = next;
	new_block.size_in_pages = nr_pages;
	new_block.magic = MD_SHARED_ARENA_FREE_MAGIC;
	ret = md_user_write_free_block(mm, arena_base, start, &new_block);
	if (ret)
		return ret;

	if (prev != MD_SHARED_ARENA_FREELIST_END) {
		unsigned long prev_next_addr;

		prev_next_addr = md_user_free_block_addr(arena_base, prev) +
			offsetof(struct md_user_free_block, next_page_off);
		ret = md_user_write_u32(mm, prev_next_addr, start);
		if (ret)
			return ret;
	} else {
		hdr.freelist_head_page_off = start;
	}

	if (next != MD_SHARED_ARENA_FREELIST_END) {
		unsigned long next_prev_addr;

		next_prev_addr = md_user_free_block_addr(arena_base, next) +
			offsetof(struct md_user_free_block, prev_page_off);
		ret = md_user_write_u32(mm, next_prev_addr, start);
		if (ret)
			return ret;
	}

	hdr.free_count++;

	if (next != MD_SHARED_ARENA_FREELIST_END &&
	    start + new_block.size_in_pages == next) {
		struct md_user_free_block next_block;
		u32 zero = 0;

		ret = md_user_read_free_block(mm, arena_base, next, &next_block);
		if (ret)
			return ret;
		if (next_block.magic != MD_SHARED_ARENA_FREE_MAGIC ||
		    !next_block.size_in_pages ||
		    next_block.size_in_pages > hdr.total_data_pages - next)
			return -EINVAL;

		new_block.size_in_pages += next_block.size_in_pages;
		new_block.next_page_off = next_block.next_page_off;
		ret = md_user_write_free_block(mm, arena_base, start, &new_block);
		if (ret)
			return ret;
		if (next_block.next_page_off != MD_SHARED_ARENA_FREELIST_END) {
			unsigned long next_next_prev_addr;

			next_next_prev_addr = md_user_free_block_addr(arena_base,
								      next_block.next_page_off);
			next_next_prev_addr += offsetof(struct md_user_free_block,
							prev_page_off);
			ret = md_user_write_u32(mm, next_next_prev_addr, start);
			if (ret)
				return ret;
		}
		ret = md_user_write_u32(mm,
					md_user_free_block_addr(arena_base, next) +
					offsetof(struct md_user_free_block, magic),
					zero);
		if (ret)
			return ret;
		hdr.free_count--;
	}

	if (prev != MD_SHARED_ARENA_FREELIST_END) {
		struct md_user_free_block prev_block;

		ret = md_user_read_free_block(mm, arena_base, prev, &prev_block);
		if (ret)
			return ret;
		if (prev_block.magic != MD_SHARED_ARENA_FREE_MAGIC ||
		    !prev_block.size_in_pages ||
		    prev_block.size_in_pages > hdr.total_data_pages - prev)
			return -EINVAL;

		if (prev + prev_block.size_in_pages == start) {
			u32 zero = 0;

			prev_block.size_in_pages += new_block.size_in_pages;
			prev_block.next_page_off = new_block.next_page_off;
			ret = md_user_write_free_block(mm, arena_base, prev,
						       &prev_block);
			if (ret)
				return ret;
			if (new_block.next_page_off !=
			    MD_SHARED_ARENA_FREELIST_END) {
				unsigned long new_next_prev_addr;

				new_next_prev_addr =
					md_user_free_block_addr(arena_base,
								new_block.next_page_off);
				new_next_prev_addr += offsetof(struct md_user_free_block,
							       prev_page_off);
				ret = md_user_write_u32(mm, new_next_prev_addr,
							prev);
				if (ret)
					return ret;
			}
			ret = md_user_write_u32(mm,
						md_user_free_block_addr(arena_base, start) +
						offsetof(struct md_user_free_block, magic),
						zero);
			if (ret)
				return ret;
			hdr.free_count--;
		}
	}

	hdr.total_donated_bytes += (unsigned long)nr_pages * PAGE_SIZE;
	hdr.donate_count++;

	ret = md_user_write_u32(mm, header_addr +
		offsetof(struct md_user_arena_header, freelist_head_page_off),
		hdr.freelist_head_page_off);
	if (ret)
		return ret;
	ret = md_user_write_u32(mm, header_addr +
		offsetof(struct md_user_arena_header, free_count),
		hdr.free_count);
	if (ret)
		return ret;
	ret = md_user_write_ulong(mm, header_addr +
		offsetof(struct md_user_arena_header, total_donated_bytes),
		hdr.total_donated_bytes);
	if (ret)
		return ret;
	return md_user_write_u32(mm, header_addr +
		offsetof(struct md_user_arena_header, donate_count),
		hdr.donate_count);
}

/* 功能：在 arena 锁保护下获取或分配某个 mm 的 owner slot；调用时机：应用 alloc shadow log 时调用。 */
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

/* 功能：在 arena 锁保护下释放无引用页面的 owner slot；调用时机：free log 或 mm release 清空页面所有权后调用。 */
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

/* 功能：把页范围覆盖到的 chunk 标记为 dirty；调用时机：alloc/free log 改变页面所有权后调用。 */
static void md_mark_dirty_chunks(unsigned long *dirty_chunks, u32 start, u32 end)
{
	bitmap_set(dirty_chunks, md_chunk_of_page(start),
		   md_chunk_of_page(end - 1) - md_chunk_of_page(start) + 1);
}

/* 功能：在 arena 锁保护下追加一个 PTE delta run；调用时机：成功应用 shadow log 并准备提交代际时调用。 */
static void md_record_pte_delta_locked(struct md_arena_meta *arena, u64 seq,
				       u8 op, u8 owner_slot, u32 owner_gen,
				       u32 start_page, u32 nr_pages)
{
	struct md_pte_delta_run *run;
	u64 serial;
	u32 index;

	if (!arena || !arena->delta_runs || !seq || !nr_pages)
		return;

	lockdep_assert_held(&arena->lock);

	serial = arena->delta_next_serial++;
	if (!serial)
		serial = arena->delta_next_serial++;
	index = serial % MD_PTE_DELTA_RUN_CAPACITY;
	run = &arena->delta_runs[index];
	if (run->serial && run->seq > arena->delta_loss_seq)
		arena->delta_loss_seq = run->seq;

	run->serial = serial;
	run->seq = seq;
	run->op = op;
	run->owner_slot = owner_slot;
	run->reserved = 0;
	run->owner_gen = owner_gen;
	run->start_page = start_page;
	run->nr_pages = nr_pages;
}

/* 功能：检查 u32 页元数据范围是否全部等于 value；调用时机：free log 连续同 owner 快路径验证。 */
static bool md_u32_range_all_equal(const u32 *array, u32 start, u32 end,
				   u32 value)
{
	u32 i;

	for (i = start; i < end; i++) {
		if (array[i] != value)
			return false;
	}

	return true;
}

/* 功能：在 arena 锁保护下把一条 shadow log 应用到页面所有权元数据；调用时机：context switch drain ring 时调用。 */
static int md_apply_log_entry_locked(struct md_arena_meta *arena,
				     const struct md_shadow_log *entry,
				     struct mm_struct *owner_mm,
				     unsigned long *dirty_chunks,
				     u64 pending_seq,
				     struct md_switch_cycle_stats *stats)
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
		u32 nr_pages = end - start;
		u8 prefault = (entry->flags & MD_LOG_F_PREFAULT) != 0;
		MD_TIME_START(phase, "drain_alloc_check");

		if (memchr_inv(arena->page_slot + start, MD_INVALID_SLOT,
			       nr_pages)) {
			MD_STATS_ADD_MAX(stats, drain_alloc_check_cycles,
					 max_drain_alloc_check_cycles,
					 MD_TIME_END(phase));
			return -EBUSY;
		}
		MD_STATS_ADD_MAX(stats, drain_alloc_check_cycles,
				 max_drain_alloc_check_cycles,
				 MD_TIME_END(phase));
		MD_TIME_START(update_phase, "drain_alloc_update");

		slot = md_get_owner_slot_locked(arena, owner_mm);
		if (slot == U16_MAX) {
			MD_STATS_ADD_MAX(stats, drain_alloc_update_cycles,
					 max_drain_alloc_update_cycles,
					 MD_TIME_END(update_phase));
			return -ENOSPC;
		}

		owner_gen = arena->owner_table[slot].gen;
		memset(arena->page_slot + start, (u8)slot, nr_pages);
		memset(arena->page_free + start, 0, nr_pages);
		memset(arena->page_prefault + start, prefault, nr_pages);
		memset32(arena->page_owner_gen + start, owner_gen, nr_pages);
		arena->owner_table[slot].ref_pages += nr_pages;
		md_mark_dirty_chunks(dirty_chunks, start, end);
		md_record_pte_delta_locked(arena, pending_seq,
					   MD_PTE_DELTA_REVOKE_NON_OWNER,
					   (u8)slot, owner_gen, start,
					   nr_pages);
		MD_STATS_ADD_MAX(stats, drain_alloc_update_cycles,
				 max_drain_alloc_update_cycles,
				 MD_TIME_END(update_phase));
		return 0;
	}

	if (entry->op == MD_LOG_FREE) {
		struct md_owner_entry *owner = NULL;
		u16 fast_slot = MD_INVALID_SLOT;
		u32 fast_gen = 0;
		u32 nr_pages = end - start;
		bool fast_free = false;
		MD_TIME_START(phase, "drain_free_check");

		fast_slot = arena->page_slot[start];
		if (fast_slot != MD_INVALID_SLOT &&
		    fast_slot < arena->owner_slots) {
			owner = &arena->owner_table[fast_slot];
			fast_gen = arena->page_owner_gen[start];
			if (owner->valid && owner->mm == owner_mm &&
			    owner->gen == fast_gen &&
			    !memchr_inv(arena->page_slot + start,
					(u8)fast_slot, nr_pages) &&
			    md_u32_range_all_equal(arena->page_owner_gen,
						   start, end, fast_gen) &&
			    owner->ref_pages >= nr_pages)
				fast_free = true;
		}

		if (!fast_free) {
			for (i = start; i < end; i++) {
				if (!md_page_belongs_to_mm_locked(arena, i,
								  owner_mm)) {
					MD_STATS_ADD_MAX(stats,
							 drain_free_check_cycles,
							 max_drain_free_check_cycles,
							 MD_TIME_END(phase));
					return -EPERM;
				}
			}
		}
		MD_STATS_ADD_MAX(stats, drain_free_check_cycles,
				 max_drain_free_check_cycles,
				 MD_TIME_END(phase));
		MD_TIME_START(update_phase, "drain_free_update");

		if (fast_free) {
			memset(arena->page_slot + start, MD_INVALID_SLOT,
			       nr_pages);
			memset(arena->page_free + start, 0, nr_pages);
			/*
			 * Only the first page of a freed block contains the
			 * intrusive freelist header.  Prefault that metadata page
			 * so future retrieve()/merge operations can inspect it,
			 * but keep the rest of the free run demand-faulted.
			 */
			arena->page_free[start] = 1;
			memset(arena->page_prefault + start, 0, nr_pages);
			memset32(arena->page_owner_gen + start, 0, nr_pages);
			owner->ref_pages -= nr_pages;
			md_put_owner_slot_locked(arena, fast_slot);
		} else {
			for (i = start; i < end; i++) {
				u16 slot = arena->page_slot[i];

				arena->page_slot[i] = MD_INVALID_SLOT;
				/*
				 * Only the first page of a freed block contains the
				 * intrusive freelist header.  Prefault that metadata page
				 * so future retrieve()/merge operations can inspect it,
				 * but keep the rest of the free run demand-faulted.
				 */
				arena->page_free[i] = (i == start);
				arena->page_prefault[i] = 0;
				arena->page_owner_gen[i] = 0;
				if (slot < arena->owner_slots &&
				    arena->owner_table[slot].ref_pages)
					arena->owner_table[slot].ref_pages--;
				md_put_owner_slot_locked(arena, slot);
			}
		}
		md_mark_dirty_chunks(dirty_chunks, start, end);
		md_record_pte_delta_locked(arena, pending_seq,
					   MD_PTE_DELTA_PREFAULT_FREE,
					   MD_INVALID_SLOT, 0, start, 1);
		MD_STATS_ADD_MAX(stats, drain_free_update_cycles,
				 max_drain_free_update_cycles,
				 MD_TIME_END(update_phase));
		return 0;
	}

	return -EINVAL;
}

/* 功能：在 arena 锁保护下提交 dirty chunk 并推进全局提交序号；调用时机：批量应用 log 或释放 mm 页面后调用。 */
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

/* 功能：清空 arena 内核真值表；调用时机：新一代用户态 backing 首次注册前调用。 */
static void md_reset_arena_locked(struct md_arena_meta *arena)
{
	if (!arena)
		return;

	lockdep_assert_held(&arena->lock);

	memset(arena->page_slot, MD_INVALID_SLOT, arena->nr_pages);
	memset(arena->page_free, 0, arena->nr_pages);
	memset(arena->page_prefault, 0, arena->nr_pages);
	memset(arena->page_owner_gen, 0,
	       array_size(arena->nr_pages, sizeof(*arena->page_owner_gen)));
	memset(arena->chunk_gen, 0,
	       array_size(arena->nr_chunks, sizeof(*arena->chunk_gen)));
	memset(arena->owner_table, 0,
	       array_size(arena->owner_slots, sizeof(*arena->owner_table)));
	arena->global_commit_seq = 0;
	arena->delta_next_serial = 1;
	arena->delta_loss_seq = 0;
	if (arena->delta_runs)
		memset(arena->delta_runs, 0,
		       array_size(MD_PTE_DELTA_RUN_CAPACITY,
				  sizeof(*arena->delta_runs)));
}

static void md_reset_all_arenas_if_idle(void)
{
	unsigned int cpu;

	if (atomic_read(&md_active_mms))
		return;

	mutex_lock(&md_arena_table_lock);
	if (atomic_read(&md_active_mms)) {
		mutex_unlock(&md_arena_table_lock);
		return;
	}

	for_each_possible_cpu(cpu) {
		struct md_arena_meta *arena;

		arena = rcu_dereference_protected(
			md_arenas[cpu], lockdep_is_held(&md_arena_table_lock));
		if (!arena)
			continue;

		spin_lock(&arena->lock);
		md_reset_arena_locked(arena);
		spin_unlock(&arena->lock);
	}
	mutex_unlock(&md_arena_table_lock);
}

/* 功能：消费 ctx 关联的 shadow log ring 并按 src_cpu 分发提交到目标 arena；调用时机：进程切出 CPU 的 context switch 路径调用。 */
static bool md_drain_log_ring(struct md_mm_ctx *ctx,
			      struct md_switch_cycle_stats *stats)
{
	DECLARE_BITMAP(dirty_chunks, MD_SHARED_ARENA_NR_CHUNKS_MAX);
	DECLARE_BITMAP(src_cpus, NR_CPUS);
	struct md_log_ring *ring;
	struct md_shadow_log *entries;
	struct page *ring_page;
	void *kaddr;
	u32 head, tail, i;
	unsigned long src_cpu;
	u8 first_src_cpu = U8_MAX;
	bool single_src_batch = true;
	bool had_entries = false;
	MD_TIME_START(scan, "drain_scan");

	if (!ctx || !ctx->key.mm)
		return false;

	ring_page = READ_ONCE(ctx->ring_page);
	if (!ring_page)
		return false;

	kaddr = kmap_local_page(ring_page);
	ring = kaddr;
	if (ring->magic != MD_LOG_RING_MAGIC ||
	    ring->version != MD_LOG_RING_VERSION ||
	    !ring->capacity || ring->ring_size > PAGE_SIZE ||
	    sizeof(*ring) + ring->capacity * sizeof(*entries) > ring->ring_size) {
		kunmap_local(kaddr);
		return false;
	}

	entries = (struct md_shadow_log *)(ring + 1);
	head = READ_ONCE(ring->head);
	tail = smp_load_acquire(&ring->tail);
	if (READ_ONCE(ring->dropped))
		pr_warn_ratelimited(
			"memory_delegation: log dropped observed mm=%p cpu=%u head=%u tail=%u cap=%u dropped=%u\n",
			ctx->key.mm, ctx->key.cpu, head, tail, ring->capacity,
			READ_ONCE(ring->dropped));
	if (tail - head > ring->capacity) {
		pr_warn_ratelimited(
			"memory_delegation: log ring overflow mm=%p cpu=%u head=%u tail=%u cap=%u dropped=%u\n",
			ctx->key.mm, ctx->key.cpu, head, tail, ring->capacity,
			READ_ONCE(ring->dropped));
		smp_store_release(&ring->head, tail);
		kunmap_local(kaddr);
		return false;
	}

	if (head == tail) {
		kunmap_local(kaddr);
		return false;
	}
	had_entries = true;

	/* Pass 1: collect the set of src_cpus present in this batch. */
	bitmap_zero(src_cpus, NR_CPUS);
	for (i = head; i != tail; i++) {
		u8 src = entries[i % ring->capacity].src_cpu;

		if (src >= nr_cpu_ids) {
			single_src_batch = false;
			continue;
		}

		__set_bit(src, src_cpus);
		if (first_src_cpu == U8_MAX)
			first_src_cpu = src;
		else if (src != first_src_cpu)
			single_src_batch = false;
	}
	if (first_src_cpu == U8_MAX)
		single_src_batch = false;
	MD_STATS_ADD_MAX(stats, drain_scan_cycles, max_drain_scan_cycles,
			 MD_TIME_END(scan));

	/*
	 * Pass 2: for each referenced arena, acquire its lock and apply all
	 * entries that target it.  Each arena is visited at most once, so
	 * there is no risk of lock reordering.
	 */
	for_each_set_bit(src_cpu, src_cpus, NR_CPUS) {
		struct md_arena_meta *arena;
		bool filter_src = !single_src_batch;
		u64 pending_seq;

		rcu_read_lock();
		arena = md_get_arena_rcu(src_cpu);
		if (!arena) {
			rcu_read_unlock();
			continue;
		}
		bitmap_zero(dirty_chunks, arena->nr_chunks);

		// 本cpu的log可能涉及其他arena的修改（该线程可能在其他cpu上分配了内存才迁移至此）
		MD_TIME_START(lock_wait, "drain_lock_wait");
		spin_lock(&arena->lock);
		MD_STATS_ADD_MAX(stats, drain_lock_wait_cycles,
				 max_drain_lock_wait_cycles,
				 MD_TIME_END(lock_wait));
		MD_TIME_START(lock_hold, "drain_lock_hold");
		pending_seq = arena->global_commit_seq + 1;

		for (i = head; i != tail; i++) {
			const struct md_shadow_log *entry =
				&entries[i % ring->capacity];
			u64 apply_delta;
			int ret;

			if (filter_src && entry->src_cpu != (u8)src_cpu)
				continue;

			MD_TIME_START(apply, "drain_apply");
			ret = md_apply_log_entry_locked(arena, entry,
							ctx->key.mm,
							dirty_chunks,
							pending_seq, stats);
			apply_delta = MD_TIME_END(apply);
			MD_STATS_INC(stats, drain_entries);
			MD_STATS_ADD(stats, drain_pages, entry->nr_pages);
			MD_STATS_ADD_MAX(stats, drain_apply_cycles,
					 max_drain_apply_cycles, apply_delta);
			if (entry->op == MD_LOG_ALLOC) {
				MD_STATS_INC(stats, drain_alloc_entries);
				MD_STATS_ADD(stats, drain_alloc_pages,
					     entry->nr_pages);
				MD_STATS_ADD_MAX(stats, drain_apply_alloc_cycles,
						 max_drain_apply_alloc_cycles,
						 apply_delta);
			} else if (entry->op == MD_LOG_FREE) {
				MD_STATS_INC(stats, drain_free_entries);
				MD_STATS_ADD(stats, drain_free_pages,
					     entry->nr_pages);
				MD_STATS_ADD_MAX(stats, drain_apply_free_cycles,
						 max_drain_apply_free_cycles,
						 apply_delta);
			}
			if (ret)
				pr_warn_ratelimited(
					"memory_delegation: drop log op=%u src_cpu=%u start=%u pages=%u ret=%d\n",
					entry->op, entry->src_cpu,
					entry->start_page, entry->nr_pages,
					ret);
		}
		MD_TIME_START(commit, "drain_commit");
		md_commit_dirty_chunks_locked(arena, dirty_chunks);
		MD_STATS_ADD_MAX(stats, drain_commit_cycles,
				 max_drain_commit_cycles, MD_TIME_END(commit));
		MD_STATS_ADD_MAX(stats, drain_lock_hold_cycles,
				 max_drain_lock_hold_cycles,
				 MD_TIME_END(lock_hold));
		spin_unlock(&arena->lock);
		rcu_read_unlock();
	}

	smp_store_release(&ring->head, tail);
	kunmap_local(kaddr);
	return had_entries;
}

/* 功能：按需给即将运行的 task 排队一次 arena 页表同步 task_work；调用时机：进程切入 CPU 且 arena 有新提交时调用。 */
static bool md_queue_sync_if_needed(struct md_mm_ctx *ctx,
				    struct task_struct *task, u64 latest_seq)
{
	bool queue = false;
	unsigned long arena_base = READ_ONCE(ctx->arena_base);
	u64 last_seen = READ_ONCE(ctx->last_seen_gen);
	unsigned int state;

	if (!arena_base || latest_seq <= last_seen)
		return false;

	state = cmpxchg(&ctx->sync_state, MD_SYNC_IDLE, MD_SYNC_QUEUED);
	if (state == MD_SYNC_IDLE) {
		refcount_inc(&ctx->refs);
		queue = true;
	} else {
		if (state == MD_SYNC_RUNNING)
			(void)cmpxchg(&ctx->sync_state, MD_SYNC_RUNNING,
				      MD_SYNC_RERUN);
	}

	if (!queue)
		return false;

	if (task_work_add(task, &ctx->sync_work, TWA_RESUME)) {
		pr_warn_ratelimited(
			"memory_delegation: task_work_add failed mm=%p cpu=%u task=%s[%d] last_seen=%llu latest=%llu\n",
			ctx->key.mm, ctx->key.cpu, task->comm, task->pid,
			last_seen, latest_seq);
		WRITE_ONCE(ctx->sync_state, MD_SYNC_IDLE);
		md_mm_ctx_put(ctx);
		return false;
	}

	return true;
}

/* 功能：在可睡眠 task_work 上下文中执行当前 mm 的 arena 页表同步；调用时机：task 返回用户态前由 task_work 调用。 */
static void md_sync_task_work(struct callback_head *work)
{
	struct md_mm_ctx *ctx = container_of(work, struct md_mm_ctx, sync_work);
	struct mm_struct *mm = current->mm;
	unsigned long arena_base = READ_ONCE(ctx->arena_base);
	bool stats_enabled = MD_DEBUG_STATS_ENABLED();
	struct md_pte_sync_detail total_detail = {};
	MD_TIME_START(sync, "pte_sync");

	WRITE_ONCE(ctx->sync_state, MD_SYNC_RUNNING);

	if (mm == ctx->key.mm && mm) {
		bool pte_modified = false;

		for (;;) {
			unsigned int state;
			bool this_pte_modified = false;
			struct md_pte_sync_detail this_detail = {};
			MD_TIME_START(lock_wait, "pte_sync_mmap_lock_wait");

			mmap_write_lock(mm);
			MD_DETAIL_ADD(&this_detail, lock_wait_cycles,
				      MD_TIME_END(lock_wait));
			MD_TIME_START(lock_hold, "pte_sync_mmap_lock_hold");
			md_memory_delegation_sync_mm(mm, ctx->key.cpu, arena_base,
						     &this_pte_modified,
						     stats_enabled ?
						     &this_detail : NULL);
			MD_DETAIL_ADD(&this_detail, lock_hold_cycles,
				      MD_TIME_END(lock_hold));
			mmap_write_unlock(mm);
			if (unlikely(stats_enabled)) {
				md_pte_sync_detail_add(&total_detail,
						       &this_detail);
			}
			pte_modified |= this_pte_modified;

			state = cmpxchg(&ctx->sync_state, MD_SYNC_RUNNING,
					MD_SYNC_IDLE);
			if (state == MD_SYNC_RUNNING)
				break;
			if (state == MD_SYNC_RERUN) {
				WRITE_ONCE(ctx->sync_state, MD_SYNC_RUNNING);
				continue;
			}

			WRITE_ONCE(ctx->sync_state, MD_SYNC_IDLE);
			break;
		}
		if (unlikely(stats_enabled)) {
			u64 sync_delta = MD_TIME_END(sync);
			struct md_switch_cycle_stats *stats;

			stats = MD_DEBUG_GET_CPU_STATS();
			MD_STATS_INC(stats, pte_sync_calls);
			MD_STATS_ADD_MAX(stats, pte_sync_cycles,
					 max_pte_sync_cycles, sync_delta);
			md_pte_sync_stats_add(stats, &total_detail);
			if (pte_modified) {
				MD_STATS_INC(stats, pte_sync_modified_calls);
				MD_STATS_ADD_MAX(stats, pte_sync_modified_cycles,
						 max_pte_sync_modified_cycles,
						 sync_delta);
			} else {
				MD_STATS_INC(stats, pte_sync_unchanged_calls);
				MD_STATS_ADD_MAX(stats, pte_sync_unchanged_cycles,
						 max_pte_sync_unchanged_cycles,
						 sync_delta);
			}
			MD_DEBUG_PUT_CPU_STATS();
		}
	} else {
		WRITE_ONCE(ctx->sync_state, MD_SYNC_IDLE);
	}

	md_mm_ctx_put(ctx);
}

/* 功能：为指定 CPU 注册 arena 所有权元数据；调用时机：内核或 register_ring 初始化 per-CPU SharedArena 时调用。 */
int memory_delegation_arena_register(unsigned int cpu, unsigned int nr_pages,
				     unsigned int owner_slots)
{
	struct md_arena_meta *arena;
	struct md_arena_meta *old_arena;

	if (cpu >= nr_cpu_ids)
		return -EINVAL;
	if (!nr_pages || nr_pages > MD_SHARED_ARENA_NR_PAGES ||
	    !owner_slots || owner_slots >= MD_INVALID_SLOT)
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
	arena->nr_chunks = DIV_ROUND_UP(nr_pages, md_current_chunk_pages());
	arena->owner_slots = owner_slots;
	arena->delta_next_serial = 1;
	spin_lock_init(&arena->lock);

	arena->page_slot = kvzalloc(nr_pages, GFP_KERNEL);
	arena->page_free = kvzalloc(nr_pages, GFP_KERNEL);
	arena->page_prefault = kvzalloc(nr_pages, GFP_KERNEL);
	arena->page_owner_gen = kvcalloc(nr_pages, sizeof(*arena->page_owner_gen),
					 GFP_KERNEL);
	arena->chunk_gen = kvcalloc(arena->nr_chunks, sizeof(*arena->chunk_gen),
				    GFP_KERNEL);
	arena->owner_table = kvcalloc(owner_slots, sizeof(*arena->owner_table),
				      GFP_KERNEL);
	arena->delta_runs = kvcalloc(MD_PTE_DELTA_RUN_CAPACITY,
				     sizeof(*arena->delta_runs), GFP_KERNEL);
	if (!arena->page_slot || !arena->page_free || !arena->page_prefault ||
	    !arena->page_owner_gen || !arena->chunk_gen ||
	    !arena->owner_table || !arena->delta_runs) {
		kvfree(arena->delta_runs);
		kvfree(arena->owner_table);
		kvfree(arena->chunk_gen);
		kvfree(arena->page_owner_gen);
		kvfree(arena->page_prefault);
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
		kvfree(arena->delta_runs);
		kvfree(arena->owner_table);
		kvfree(arena->chunk_gen);
		kvfree(arena->page_owner_gen);
		kvfree(arena->page_prefault);
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

/* 功能：注销并释放指定 CPU 的 arena 所有权元数据；调用时机：SharedArena teardown 或模块清理时调用。 */
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
	kvfree(arena->delta_runs);
	kvfree(arena->owner_table);
	kvfree(arena->chunk_gen);
	kvfree(arena->page_owner_gen);
	kvfree(arena->page_prefault);
	kvfree(arena->page_free);
	kvfree(arena->page_slot);
	kfree(arena);
}
EXPORT_SYMBOL_GPL(memory_delegation_arena_unregister);

/* 功能：为当前 mm 注册指定 CPU 的 SharedArena 基址和 shadow log ring；调用时机：用户态分配器初始化 arena 共享内存池时调用。 */
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
	 * Android SharedArena backing is owned by the system broker, while
	 * kernel arena metadata remains global per CPU.  When the last
	 * participating mm has gone away, reset stale ownership/chunk state
	 * before a later generation starts registering rings.
	 */
	md_reset_all_arenas_if_idle();

	/*
	 * ctx->arena_base is write-once.  After it becomes non-zero it must not
	 * change, so hot-path readers can use READ_ONCE() without taking a lock.
	 */
	prev = cmpxchg(&ctx->arena_base, 0UL, arena_base);
	if (prev && prev != arena_base) {
		ret = -EINVAL;
		goto out_put_ctx;
	}

	if (READ_ONCE(ctx->ring_page) && arena->nr_pages != nr_pages)
		ret = -EINVAL;

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
	if (!ret)
		ret = md_mm_ctx_activate(ctx, GFP_KERNEL);
out_put_ctx:
	md_mm_ctx_put(ctx);
	return ret;
}
EXPORT_SYMBOL_GPL(memory_delegation_register_ring);

/* 功能：预留的直接提交 shadow log 接口；调用时机：当前实现不支持，外部调用会返回 -EOPNOTSUPP。 */
int memory_delegation_submit_log(unsigned int cpu, unsigned long arena_base,
				 const struct md_shadow_log *log,
				 unsigned int nr_entries)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(memory_delegation_submit_log);

/* 功能：在切出时 drain prev 的 log 并在切入时为 next 排队页表同步；调用时机：调度器 context_switch 钩子调用。 */
void memory_delegation_on_context_switch(struct task_struct *prev,
					 struct task_struct *next)
{
	unsigned int cpu;
	struct mm_struct *prev_mm = NULL;
	bool prev_active;
	bool next_active = next && next->mm && md_mm_has_active_ctx(next->mm);
	bool stats_enabled = MD_DEBUG_STATS_ENABLED();
	bool ctx_switch_sample = false;
	bool switch_drained_nonempty = false;
	bool switch_queued_pte_sync = false;
	struct md_switch_cycle_stats *stats = NULL;
	MD_TIME_START(ctx_switch, "ctx_switch");

	if (prev)
		prev_mm = prev->mm;
	prev_active = prev_mm && md_mm_has_active_ctx(prev_mm);

	ctx_switch_sample = prev_active && next_active;
	if (unlikely(stats_enabled && (prev_active || next_active))) {
		stats = MD_DEBUG_RAW_CPU_STATS();
		MD_TIME_RESUME(ctx_switch);
	}

	/* Fast path: neither switched mm participates in memory delegation. */
	if (!prev_active && !next_active)
		return;

	cpu = smp_processor_id();

	if (prev_active) {
		struct md_mm_ctx *prev_ctx;
		unsigned long arena_base;

		prev_ctx = md_mm_ctx_lookup_get(prev_mm, cpu);
		if (prev_ctx) {
			arena_base = READ_ONCE(prev_ctx->arena_base);
			if (arena_base) {
				if (unlikely(stats_enabled)) {
					MD_TIME_START(drain, "drain");
					bool nonempty = md_drain_log_ring(prev_ctx,
									 stats);
					u64 drain_delta;

					drain_delta = MD_TIME_END(drain);
					MD_STATS_INC(stats, drain_calls);
					if (nonempty) {
						switch_drained_nonempty = true;
						MD_STATS_INC(stats,
							     drain_nonempty_calls);
						MD_STATS_ADD_MAX(stats,
							drain_nonempty_cycles,
							max_drain_nonempty_cycles,
							drain_delta);
					} else {
						MD_STATS_INC(stats,
							     drain_empty_calls);
						MD_STATS_ADD_MAX(stats,
							drain_empty_cycles,
							max_drain_empty_cycles,
							drain_delta);
					}
					MD_STATS_ADD_MAX(stats, drain_cycles,
							 max_drain_cycles,
							 drain_delta);
				} else {
					md_drain_log_ring(prev_ctx, NULL);
				}
			}

			md_mm_ctx_put(prev_ctx);
		}
	}

	if (!next_active) {
		if (unlikely(stats && ctx_switch_sample)) {
			u64 delta = MD_TIME_END(ctx_switch);

			MD_STATS_INC(stats, ctx_switch_calls);
			MD_STATS_ADD_MAX(stats, ctx_switch_cycles,
					 max_ctx_switch_cycles, delta);
			if (switch_drained_nonempty) {
				MD_STATS_INC(stats, ctx_switch_slow_calls);
				MD_STATS_ADD_MAX(stats, ctx_switch_slow_cycles,
						 max_ctx_switch_slow_cycles,
						 delta);
			} else {
				MD_STATS_INC(stats, ctx_switch_fast_calls);
				MD_STATS_ADD_MAX(stats, ctx_switch_fast_cycles,
						 max_ctx_switch_fast_cycles,
						 delta);
			}
		}
		return;
	}

	{
		struct md_mm_ctx *ctx;
		struct md_arena_meta *arena;

		ctx = md_mm_ctx_lookup_get(next->mm, cpu);
		if (ctx) {
			rcu_read_lock();
			arena = md_get_arena_rcu(cpu);
			if (arena) {
				if (md_queue_sync_if_needed(
					    ctx, next,
					    READ_ONCE(arena->global_commit_seq)))
					switch_queued_pte_sync = true;
			}
			rcu_read_unlock();
			md_mm_ctx_put(ctx);
		}
	}
	if (unlikely(stats && ctx_switch_sample)) {
		u64 delta = MD_TIME_END(ctx_switch);

		MD_STATS_INC(stats, ctx_switch_calls);
		MD_STATS_ADD_MAX(stats, ctx_switch_cycles,
				 max_ctx_switch_cycles, delta);
		if (switch_drained_nonempty || switch_queued_pte_sync) {
			MD_STATS_INC(stats, ctx_switch_slow_calls);
			MD_STATS_ADD_MAX(stats, ctx_switch_slow_cycles,
					 max_ctx_switch_slow_cycles, delta);
			if (switch_drained_nonempty)
				MD_STATS_INC(stats, ctx_switch_log_slow_calls);
			if (switch_queued_pte_sync)
				MD_STATS_INC(stats, ctx_switch_pte_slow_calls);
		} else {
			MD_STATS_INC(stats, ctx_switch_fast_calls);
			MD_STATS_ADD_MAX(stats, ctx_switch_fast_cycles,
					 max_ctx_switch_fast_cycles, delta);
		}
	}
}
EXPORT_SYMBOL_GPL(memory_delegation_on_context_switch);

/* 功能：快照一个 dirty chunk 内每页的可访问性和需预 fault 状态；调用时机：memory_delegation_sync_mm 扫描需更新 chunk 时调用。 */
static int md_snapshot_chunk_state(struct mm_struct *mm, unsigned int cpu,
				   u32 chunk, u64 last_seen, bool accessible[],
				   bool prefault_pages[],
				   u32 *out_start_page, u32 *out_end_page,
				   struct md_pte_sync_detail *detail)
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

	start_page = chunk * md_current_chunk_pages();
	end_page = min_t(u32, arena->nr_pages,
			 start_page + md_current_chunk_pages());

	MD_TIME_START(snapshot_lock_wait, "pte_sync_snapshot_lock_wait");
	spin_lock(&arena->lock);
	MD_DETAIL_ADD(detail, snapshot_lock_wait_cycles,
		      MD_TIME_END(snapshot_lock_wait));
	MD_TIME_START(snapshot_lock_hold, "pte_sync_snapshot_lock_hold");
	if (arena->chunk_gen[chunk] <= last_seen) {
		MD_DETAIL_ADD(detail, snapshot_lock_hold_cycles,
			      MD_TIME_END(snapshot_lock_hold));
		spin_unlock(&arena->lock);
		rcu_read_unlock();
		return 1;
	}
	MD_TIME_START(snapshot_loop, "pte_sync_snapshot_loop");
	for (page = start_page; page < end_page; page++) {
		prefault_pages[page - start_page] =
			(arena->page_slot[page] == MD_INVALID_SLOT &&
			 arena->page_free[page]) ||
			(arena->page_prefault[page] &&
			 md_page_belongs_to_mm_locked(arena, page, mm));
		accessible[page - start_page] =
			md_page_accessible_to_mm_locked(arena, page, mm);
	}
	MD_DETAIL_ADD(detail, snapshot_loop_cycles, MD_TIME_END(snapshot_loop));
	MD_DETAIL_ADD(detail, snapshot_lock_hold_cycles,
		      MD_TIME_END(snapshot_lock_hold));
	spin_unlock(&arena->lock);
	rcu_read_unlock();

	*out_start_page = start_page;
	*out_end_page = end_page;
	return 0;
}

/* 功能：无锁批量跳过未变化 chunk；调用时机：PTE sync 线性扫描阶段调用。 */
static int md_find_next_changed_chunk(unsigned int cpu, u32 start_chunk,
				      u32 nr_chunks, u64 last_seen,
				      u32 *out_chunk, u32 *out_skipped)
{
	struct md_arena_meta *arena;
	u32 chunk = start_chunk;

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu);
	if (!arena) {
		rcu_read_unlock();
		return -ENOENT;
	}

	nr_chunks = min(nr_chunks, arena->nr_chunks);

	while (chunk + 4 <= nr_chunks) {
		u64 gen0 = READ_ONCE(arena->chunk_gen[chunk]);
		u64 gen1 = READ_ONCE(arena->chunk_gen[chunk + 1]);
		u64 gen2 = READ_ONCE(arena->chunk_gen[chunk + 2]);
		u64 gen3 = READ_ONCE(arena->chunk_gen[chunk + 3]);

		if (gen0 > last_seen || gen1 > last_seen ||
		    gen2 > last_seen || gen3 > last_seen)
			break;
		chunk += 4;
	}

	while (chunk < nr_chunks &&
	       READ_ONCE(arena->chunk_gen[chunk]) <= last_seen)
		chunk++;

	rcu_read_unlock();

	*out_chunk = chunk;
	*out_skipped = chunk - start_chunk;
	return 0;
}

/* 功能：遇到未变化 chunk gap 时收尾正在累积的 unmap/prefault run；调用时机：PTE sync 扫描阶段调用。 */
static int md_close_sync_gap_locked(struct mm_struct *mm,
				    unsigned long arena_base, u32 nr_pages,
				    u32 gap_start_page, u32 *run_start,
				    bool *in_run, u32 *prefault_run_start,
				    bool *in_prefault_run, bool *pte_modified,
				    struct md_pte_sync_detail *detail)
{
	int ret = 0;

	gap_start_page = min(gap_start_page, nr_pages);

	if (*in_run) {
		unsigned long start = arena_base +
			(unsigned long)*run_start * PAGE_SIZE;
		unsigned long end = arena_base +
			(unsigned long)gap_start_page * PAGE_SIZE;

		if (end > start) {
			MD_TIME_START(unmap, "pte_sync_unmap");
			md_unmap_range_locked(mm, start, end);
			MD_DETAIL_ADD(detail, unmap_cycles, MD_TIME_END(unmap));
			MD_DETAIL_INC(detail, unmap_ranges);
			MD_DETAIL_ADD(detail, unmap_pages,
				      (end - start) >> PAGE_SHIFT);
			*pte_modified = true;
		}
		*in_run = false;
	}

	if (*in_prefault_run) {
		unsigned long start = arena_base +
			(unsigned long)*prefault_run_start * PAGE_SIZE;
		unsigned long end = arena_base +
			(unsigned long)gap_start_page * PAGE_SIZE;

		if (end > start) {
			MD_TIME_START(prefault, "pte_sync_prefault");
			ret = md_prefault_range_locked(mm, start, end);
			MD_DETAIL_ADD(detail, prefault_cycles,
				      MD_TIME_END(prefault));
			MD_DETAIL_INC(detail, prefault_ranges);
			MD_DETAIL_ADD(detail, prefault_pages,
				      (end - start) >> PAGE_SHIFT);
			if (ret)
				return ret;
			*pte_modified = true;
		}
		*in_prefault_run = false;
	}

	return 0;
}

static bool md_delta_run_owner_is_mm(unsigned int cpu,
				     const struct md_pte_delta_run *run,
				     struct mm_struct *mm)
{
	struct md_arena_meta *arena;
	bool match = false;

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu);
	if (arena) {
		spin_lock(&arena->lock);
		if (run->owner_slot < arena->owner_slots) {
			struct md_owner_entry *owner =
				&arena->owner_table[run->owner_slot];

			match = owner->valid && owner->mm == mm &&
				owner->gen == run->owner_gen;
		}
		spin_unlock(&arena->lock);
	}
	rcu_read_unlock();

	return match;
}

static bool md_delta_free_prefault_still_valid(unsigned int cpu,
					       const struct md_pte_delta_run *run)
{
	struct md_arena_meta *arena;
	bool valid = false;

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu);
	if (arena) {
		spin_lock(&arena->lock);
		valid = run->start_page < arena->nr_pages &&
			arena->page_slot[run->start_page] == MD_INVALID_SLOT &&
			arena->page_free[run->start_page];
		spin_unlock(&arena->lock);
	}
	rcu_read_unlock();

	return valid;
}

/* 功能：尝试用 bounded delta run ring 同步 PTE；返回 -EAGAIN 时调用方回退 chunk 扫描。 */
static int md_sync_mm_from_deltas(struct mm_struct *mm, unsigned int cpu,
				  unsigned long arena_base, u32 nr_pages,
				  u64 last_seen, u64 target_seq,
				  bool *pte_modified,
				  struct md_pte_sync_detail *detail)
{
	struct md_pte_delta_run *runs;
	struct md_arena_meta *arena;
	u64 first_serial;
	u64 next_serial;
	u64 serial;
	u32 wanted = 0;
	u32 count = 0;
	u32 i;
	int ret = 0;
	MD_TIME_START(scan, "pte_delta_scan");

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu);
	if (!arena) {
		rcu_read_unlock();
		return -ENOENT;
	}

	spin_lock(&arena->lock);
	if (last_seen < arena->delta_loss_seq) {
		spin_unlock(&arena->lock);
		rcu_read_unlock();
		MD_DETAIL_INC(detail, delta_fallbacks);
		MD_DETAIL_INC(detail, delta_lost_fallbacks);
		MD_DETAIL_ADD(detail, scan_cycles, MD_TIME_END(scan));
		return -EAGAIN;
	}

	next_serial = arena->delta_next_serial;
	first_serial = next_serial > MD_PTE_DELTA_RUN_CAPACITY ?
		next_serial - MD_PTE_DELTA_RUN_CAPACITY : 1;

	for (serial = first_serial; serial < next_serial; serial++) {
		const struct md_pte_delta_run *run =
			&arena->delta_runs[serial % MD_PTE_DELTA_RUN_CAPACITY];

		if (run->serial != serial)
			continue;
		if (run->seq <= last_seen || run->seq > target_seq)
			continue;
		if (run->start_page >= nr_pages ||
		    run->nr_pages > nr_pages - run->start_page) {
			spin_unlock(&arena->lock);
			rcu_read_unlock();
			MD_DETAIL_INC(detail, delta_fallbacks);
			MD_DETAIL_ADD(detail, scan_cycles, MD_TIME_END(scan));
			return -EAGAIN;
		}
		wanted++;
	}
	spin_unlock(&arena->lock);
	rcu_read_unlock();

	if (!wanted) {
		MD_DETAIL_ADD(detail, scan_cycles, MD_TIME_END(scan));
		return 0;
	}

	runs = kcalloc(wanted, sizeof(*runs), GFP_KERNEL);
	if (!runs) {
		MD_DETAIL_INC(detail, delta_fallbacks);
		MD_DETAIL_ADD(detail, scan_cycles, MD_TIME_END(scan));
		return -EAGAIN;
	}

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu);
	if (!arena) {
		rcu_read_unlock();
		kfree(runs);
		return -ENOENT;
	}

	spin_lock(&arena->lock);
	if (last_seen < arena->delta_loss_seq) {
		spin_unlock(&arena->lock);
		rcu_read_unlock();
		kfree(runs);
		MD_DETAIL_INC(detail, delta_fallbacks);
		MD_DETAIL_INC(detail, delta_lost_fallbacks);
		MD_DETAIL_ADD(detail, scan_cycles, MD_TIME_END(scan));
		return -EAGAIN;
	}

	next_serial = arena->delta_next_serial;
	first_serial = next_serial > MD_PTE_DELTA_RUN_CAPACITY ?
		next_serial - MD_PTE_DELTA_RUN_CAPACITY : 1;

	for (serial = first_serial; serial < next_serial; serial++) {
		const struct md_pte_delta_run *run =
			&arena->delta_runs[serial % MD_PTE_DELTA_RUN_CAPACITY];

		if (run->serial != serial)
			continue;
		if (run->seq <= last_seen || run->seq > target_seq)
			continue;
		if (run->start_page >= nr_pages ||
		    run->nr_pages > nr_pages - run->start_page ||
		    count >= wanted) {
			spin_unlock(&arena->lock);
			rcu_read_unlock();
			kfree(runs);
			MD_DETAIL_INC(detail, delta_fallbacks);
			MD_DETAIL_ADD(detail, scan_cycles, MD_TIME_END(scan));
			return -EAGAIN;
		}
		runs[count++] = *run;
	}
	spin_unlock(&arena->lock);
	rcu_read_unlock();

	for (i = 0; i < count; i++) {
		const struct md_pte_delta_run *run = &runs[i];
		unsigned long start = arena_base +
			(unsigned long)run->start_page * PAGE_SIZE;
		unsigned long end = start +
			(unsigned long)run->nr_pages * PAGE_SIZE;

		MD_DETAIL_INC(detail, delta_runs);
		if (run->op == MD_PTE_DELTA_REVOKE_NON_OWNER) {
			MD_DETAIL_INC(detail, delta_revoke_runs);
			if (md_delta_run_owner_is_mm(cpu, run, mm))
				continue;

			MD_TIME_START(unmap, "pte_delta_unmap");
			md_unmap_range_locked(mm, start, end);
			MD_DETAIL_ADD(detail, unmap_cycles, MD_TIME_END(unmap));
			MD_DETAIL_INC(detail, unmap_ranges);
			MD_DETAIL_ADD(detail, unmap_pages, run->nr_pages);
			*pte_modified = true;
			continue;
		}

		if (run->op == MD_PTE_DELTA_PREFAULT_FREE) {
			MD_DETAIL_INC(detail, delta_prefault_runs);
			if (!md_delta_free_prefault_still_valid(cpu, run))
				continue;

			MD_TIME_START(prefault, "pte_delta_prefault");
			ret = md_prefault_range_locked(mm, start, end);
			MD_DETAIL_ADD(detail, prefault_cycles,
				      MD_TIME_END(prefault));
			MD_DETAIL_INC(detail, prefault_ranges);
			MD_DETAIL_ADD(detail, prefault_pages, run->nr_pages);
			if (ret) {
				ret = -EAGAIN;
				MD_DETAIL_INC(detail, delta_fallbacks);
				break;
			}
			*pte_modified = true;
			continue;
		}

		ret = -EAGAIN;
		MD_DETAIL_INC(detail, delta_fallbacks);
		break;
	}

	MD_DETAIL_ADD(detail, scan_cycles, MD_TIME_END(scan));
	kfree(runs);
	return ret;
}

/* 功能：根据 arena 所有权元数据同步修改 mm 的 arena PTE；调用时机：task_work、fork 后或显式内核同步路径调用。 */
static int md_memory_delegation_sync_mm(struct mm_struct *mm, unsigned int cpu,
					unsigned long arena_base,
					bool *out_pte_modified,
					struct md_pte_sync_detail *detail)
{
	struct md_arena_meta *arena;
	struct md_mm_ctx *ctx;
	u64 last_seen;
	u64 target_seq;
	u32 nr_chunks;
	u32 nr_pages;
	u32 chunk;
	bool accessible[MD_CHUNK_PAGES_MAX];
	bool prefault_pages[MD_CHUNK_PAGES_MAX];
	u32 run_start = 0;
	bool in_run = false;
	u32 prefault_run_start = 0;
	bool in_prefault_run = false;
	bool pte_modified = false;
	int ret = 0;
	MD_DETAIL_ZERO(detail);

	if (out_pte_modified)
		*out_pte_modified = false;

	if (!mm || cpu >= nr_cpu_ids || !arena_base)
		return -EINVAL;

	mmap_assert_write_locked(mm);

	ctx = md_mm_ctx_lookup_get(mm, cpu);
	if (!ctx)
		return 0;

	last_seen = READ_ONCE(ctx->last_seen_gen);

	rcu_read_lock();
	arena = md_get_arena_rcu(cpu);
	if (!arena) {
		rcu_read_unlock();
		ret = -ENOENT;
		goto out_put_ctx;
	}
	nr_chunks = arena->nr_chunks;
	nr_pages = arena->nr_pages;
	target_seq = READ_ONCE(arena->global_commit_seq);
	rcu_read_unlock();

	if (target_seq <= last_seen)
		goto out_put_ctx;

	ret = md_sync_mm_from_deltas(mm, cpu, arena_base, nr_pages, last_seen,
				     target_seq, &pte_modified, detail);
	if (!ret) {
		WRITE_ONCE(ctx->last_seen_gen, target_seq);
		goto out_put_ctx;
	}
	if (ret != -EAGAIN)
		goto out_put_ctx;
	ret = 0;
	pte_modified = false;

	MD_TIME_START(scan, "pte_sync_scan");
	for (chunk = 0; chunk < nr_chunks;) {
		u32 start_page;
		u32 end_page;
		u32 page;
		u32 scan_chunk = chunk;
		u32 skipped;
		int snap_ret;

		ret = md_find_next_changed_chunk(cpu, scan_chunk, nr_chunks,
						 last_seen, &chunk, &skipped);
		if (ret)
			break;
		if (skipped) {
			MD_DETAIL_ADD(detail, skipped_chunks, skipped);
			ret = md_close_sync_gap_locked(mm, arena_base, nr_pages,
					scan_chunk * md_current_chunk_pages(),
					&run_start, &in_run, &prefault_run_start,
					&in_prefault_run, &pte_modified, detail);
			if (ret || chunk >= nr_chunks)
				break;
		}

		MD_TIME_START(snapshot, "pte_sync_snapshot");
		snap_ret = md_snapshot_chunk_state(mm, cpu, chunk, last_seen,
						   accessible, prefault_pages,
						   &start_page, &end_page,
						   detail);
		MD_DETAIL_ADD(detail, snapshot_cycles, MD_TIME_END(snapshot));
		if (!snap_ret)
			MD_DETAIL_INC(detail, dirty_chunks);
		else if (snap_ret == 1)
			MD_DETAIL_INC(detail, skipped_chunks);
		if (snap_ret == 1) {
			ret = md_close_sync_gap_locked(mm, arena_base, nr_pages,
					chunk * md_current_chunk_pages(), &run_start,
					&in_run, &prefault_run_start,
					&in_prefault_run, &pte_modified, detail);
			if (ret)
				break;
			chunk++;
			continue;
		}
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
				MD_TIME_START(unmap, "pte_sync_unmap");

				md_unmap_range_locked(mm, start, end);
				MD_DETAIL_ADD(detail, unmap_cycles,
					      MD_TIME_END(unmap));
				MD_DETAIL_INC(detail, unmap_ranges);
				MD_DETAIL_ADD(detail, unmap_pages,
					      (end - start) >> PAGE_SHIFT);
				pte_modified = true;
				in_run = false;
			}

			// in_prefault_run表示正在处理一个需要预先建立PTE的连续段
			if (prefault_pages[page - start_page] && !in_prefault_run) {
				in_prefault_run = true;
				prefault_run_start = page;
				continue;
			}
			if (!prefault_pages[page - start_page] && in_prefault_run) {
				unsigned long start = arena_base +
					(unsigned long)prefault_run_start * PAGE_SIZE;
				unsigned long end = arena_base +
					(unsigned long)page * PAGE_SIZE;
				MD_TIME_START(prefault, "pte_sync_prefault");

				ret = md_prefault_range_locked(mm, start, end);
				MD_DETAIL_ADD(detail, prefault_cycles,
					      MD_TIME_END(prefault));
				MD_DETAIL_INC(detail, prefault_ranges);
				MD_DETAIL_ADD(detail, prefault_pages,
					      (end - start) >> PAGE_SHIFT);
				if (ret)
					break;
				pte_modified = true;
				in_prefault_run = false;
			}
		}

		if (ret)
			break;
		chunk++;
	}

	if (!ret && in_run) {
		unsigned long start = arena_base +
			(unsigned long)run_start * PAGE_SIZE;
		unsigned long end = arena_base +
			(unsigned long)nr_pages * PAGE_SIZE;
		MD_TIME_START(unmap, "pte_sync_unmap");

		md_unmap_range_locked(mm, start, end);
		MD_DETAIL_ADD(detail, unmap_cycles, MD_TIME_END(unmap));
		MD_DETAIL_INC(detail, unmap_ranges);
		MD_DETAIL_ADD(detail, unmap_pages, (end - start) >> PAGE_SHIFT);
		pte_modified = true;
		in_run = false;
	}

	if (!ret && in_prefault_run) {
		unsigned long start = arena_base +
			(unsigned long)prefault_run_start * PAGE_SIZE;
		unsigned long end = arena_base +
			(unsigned long)nr_pages * PAGE_SIZE;
		MD_TIME_START(prefault, "pte_sync_prefault");

		ret = md_prefault_range_locked(mm, start, end);
		MD_DETAIL_ADD(detail, prefault_cycles, MD_TIME_END(prefault));
		MD_DETAIL_INC(detail, prefault_ranges);
		MD_DETAIL_ADD(detail, prefault_pages,
			      (end - start) >> PAGE_SHIFT);
		if (!ret)
			pte_modified = true;
		in_prefault_run = false;
	}
	MD_DETAIL_ADD(detail, scan_cycles, MD_TIME_END(scan));

	if (!ret && target_seq > READ_ONCE(ctx->last_seen_gen))
		WRITE_ONCE(ctx->last_seen_gen, target_seq);

out_put_ctx:
	md_mm_ctx_put(ctx);
	if (!ret && out_pte_modified)
		*out_pte_modified = pte_modified;
	return ret;
}

int memory_delegation_sync_mm(struct mm_struct *mm, unsigned int cpu,
			      unsigned long arena_base)
{
	return md_memory_delegation_sync_mm(mm, cpu, arena_base, NULL, NULL);
}
EXPORT_SYMBOL_GPL(memory_delegation_sync_mm);

/* 功能：把父进程已注册的 arena 基址继承到子进程的新 mm；调用时机：fork 创建新 mm 后调用。 */
int memory_delegation_fork_mm(struct task_struct *task, struct mm_struct *new_mm,
			      struct mm_struct *old_mm)
{
	unsigned int cpu;
	int ret = 0;
	bool stats_enabled;
	MD_TIME_START(fork, "fork");

	if (!task || !new_mm || !old_mm)
		return -EINVAL;

	if (!md_mm_has_active_ctx(old_mm))
		return 0;

	stats_enabled = MD_DEBUG_STATS_ENABLED();
	if (unlikely(stats_enabled))
		MD_TIME_RESUME(fork);

	for_each_possible_cpu(cpu) {
		struct md_mm_ctx *old_ctx;
		struct md_mm_ctx *new_ctx;
		unsigned long arena_base;

		old_ctx = md_mm_ctx_lookup_get(old_mm, cpu);
		if (!old_ctx)
			continue;

		arena_base = READ_ONCE(old_ctx->arena_base);
		md_mm_ctx_put(old_ctx);

		if (!arena_base)
			continue;

		new_ctx = md_mm_ctx_get_or_create(new_mm, cpu, GFP_KERNEL);
		if (!new_ctx) {
			ret = -ENOMEM;
			goto out_stats;
		}

		/*
		 * Carry over the parent's write-once arena_base.  If userspace
		 * later tries to register a different base for this (mm,cpu),
		 * memory_delegation_register_ring() will reject it.
		 */
		WRITE_ONCE(new_ctx->arena_base, arena_base);
		WRITE_ONCE(new_ctx->last_seen_gen, 0);
		md_mm_ctx_put(new_ctx);
	}

out_stats:
	if (unlikely(stats_enabled)) {
		u64 delta = MD_TIME_END(fork);
		struct md_switch_cycle_stats *stats;

		stats = MD_DEBUG_GET_CPU_STATS();
		MD_STATS_INC(stats, fork_calls);
		MD_STATS_ADD_MAX(stats, fork_cycles, max_fork_cycles, delta);
		MD_DEBUG_PUT_CPU_STATS();
	}
	return ret;
}
EXPORT_SYMBOL_GPL(memory_delegation_fork_mm);

/* 功能：释放 mm 在所有 arena 中的页面所有权并销毁对应 ctx；调用时机：mm_struct 退出释放时调用。 */
void memory_delegation_mm_release(struct mm_struct *mm)
{
	unsigned int cpu;
	unsigned int bkt;
	struct md_mm_ctx *ctx;
	struct hlist_node *tmp;
	bool had_active_ctx;
	u64 total_released_pages = 0;

	if (!mm)
		return;

	had_active_ctx = md_mm_has_active_ctx(mm);

	if (had_active_ctx) {
		for_each_possible_cpu(cpu) {
			struct md_mm_ctx *ctx;

			ctx = md_mm_ctx_lookup_get(mm, cpu);
			if (!ctx)
				continue;

			if (READ_ONCE(ctx->arena_base))
				md_drain_log_ring(ctx, NULL);
			md_mm_ctx_put(ctx);
		}

		for_each_possible_cpu(cpu) {
			struct md_arena_meta *arena;
			struct md_page_run *runs;
			struct page *user_lock_page;
			unsigned long arena_base = 0;
			u64 cpu_released_pages = 0;
			u32 nr_pages;
			u32 run_count = 0;
			u32 inserted_runs = 0;
			u32 page;
			bool in_run = false;
			u32 run_start = 0;
			int ret;

			ctx = md_mm_ctx_lookup_get(mm, cpu);
			if (ctx) {
				arena_base = READ_ONCE(ctx->arena_base);
				md_mm_ctx_put(ctx);
			}

			mutex_lock(&md_arena_table_lock);
			arena = rcu_dereference_protected(md_arenas[cpu], 1);
			if (!arena) {
				mutex_unlock(&md_arena_table_lock);
				continue;
			}
			nr_pages = arena->nr_pages;

			runs = kvcalloc(nr_pages, sizeof(*runs), GFP_KERNEL);
			if (!runs) {
				mutex_unlock(&md_arena_table_lock);
				continue;
			}

			spin_lock(&arena->lock);
			for (page = 0; page < nr_pages; page++) {
				if (md_page_belongs_to_mm_locked(arena, page, mm)) {
					if (!in_run) {
						in_run = true;
						run_start = page;
					}
					continue;
				}

				if (in_run) {
					runs[run_count].start = run_start;
					runs[run_count].nr_pages = page - run_start;
					run_count++;
					in_run = false;
				}
			}
			if (in_run) {
				runs[run_count].start = run_start;
				runs[run_count].nr_pages = nr_pages - run_start;
				run_count++;
			}
			spin_unlock(&arena->lock);

			if (!run_count) {
				kvfree(runs);
				mutex_unlock(&md_arena_table_lock);
				continue;
			}

			if (!arena_base) {
				pr_warn_ratelimited(
					"memory_delegation: mm_release mm=%p cpu=%u has %u owner runs but no arena_base\n",
					mm, cpu, run_count);
				kvfree(runs);
				mutex_unlock(&md_arena_table_lock);
				continue;
			}

			ret = md_user_arena_lock(mm, arena_base, &user_lock_page);
			if (ret) {
				pr_warn_ratelimited(
					"memory_delegation: mm_release mm=%p cpu=%u failed to lock userspace arena ret=%d runs=%u\n",
					mm, cpu, ret, run_count);
				kvfree(runs);
				mutex_unlock(&md_arena_table_lock);
				continue;
			}

			for (inserted_runs = 0; inserted_runs < run_count;
			     inserted_runs++) {
				struct md_page_run *run = &runs[inserted_runs];

				ret = md_user_freelist_insert_locked(mm, arena_base,
								     run->start,
								     run->nr_pages);
				if (ret)
					break;
			}
			md_user_arena_unlock(user_lock_page, arena_base);

			if (ret)
				pr_warn_ratelimited(
					"memory_delegation: mm_release mm=%p cpu=%u freelist insert failed ret=%d inserted=%u/%u\n",
					mm, cpu, ret, inserted_runs, run_count);

			if (inserted_runs) {
				DECLARE_BITMAP(dirty_chunks, MD_SHARED_ARENA_NR_CHUNKS_MAX);
				u32 run;
				u64 pending_seq;

				bitmap_zero(dirty_chunks, arena->nr_chunks);

				spin_lock(&arena->lock);
				pending_seq = arena->global_commit_seq + 1;

				for (run = 0; run < inserted_runs; run++) {
					u32 start_page = runs[run].start;
					u32 end_page = start_page + runs[run].nr_pages;

					for (page = start_page; page < end_page; page++) {
						u16 slot;

						if (!md_page_belongs_to_mm_locked(arena,
										  page, mm))
							continue;

						slot = arena->page_slot[page];
						arena->page_slot[page] = MD_INVALID_SLOT;
						arena->page_free[page] = (page == start_page);
						arena->page_prefault[page] = 0;
						arena->page_owner_gen[page] = 0;
						if (slot < arena->owner_slots &&
						    arena->owner_table[slot].ref_pages)
							arena->owner_table[slot].ref_pages--;
						md_put_owner_slot_locked(arena, slot);
						cpu_released_pages++;
					}
					md_mark_dirty_chunks(dirty_chunks, start_page,
							     end_page);
					md_record_pte_delta_locked(arena,
						pending_seq,
						MD_PTE_DELTA_PREFAULT_FREE,
						MD_INVALID_SLOT, 0,
						start_page, 1);
				}
				md_commit_dirty_chunks_locked(arena, dirty_chunks);
				spin_unlock(&arena->lock);
			}

			kvfree(runs);
			mutex_unlock(&md_arena_table_lock);
			if (cpu_released_pages) {
				total_released_pages += cpu_released_pages;
				pr_info_ratelimited(
					"memory_delegation: mm_release mm=%p cpu=%u released_pages=%llu released_bytes=%llu\n",
					mm, cpu, cpu_released_pages,
					cpu_released_pages << PAGE_SHIFT);
			}
			}
		}

	spin_lock(&md_mm_ctx_table_lock);
	hash_for_each_safe(md_mm_ctx_table, bkt, tmp, ctx, node) {
		if (ctx->key.mm != mm)
			continue;
		hash_del(&ctx->node);
		ctx->dead = true;
		if (ctx->active) {
			ctx->active = false;
			md_active_mm_put_locked(mm);
		}
		md_mm_ctx_put(ctx);
	}
	spin_unlock(&md_mm_ctx_table_lock);

	if (had_active_ctx)
		pr_info_ratelimited(
			"memory_delegation: mm_release done mm=%p released_pages=%llu released_bytes=%llu\n",
			mm, total_released_pages,
			total_released_pages << PAGE_SHIFT);
}
EXPORT_SYMBOL_GPL(memory_delegation_mm_release);

/* 功能：判断普通 fault 地址是否允许当前 mm 访问 delegation arena 页面；调用时机：通用 fault 路径遇到可能的 arena 地址时调用。 */
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

	/* Fast path: this mm never registered a delegation context. */
	if (!md_mm_has_active_ctx(mm))
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

		/*
		 * Keep the generic fault hook consistent with md_arena_vm_ops:
		 * free pages are intentionally accessible because userspace stores
		 * intrusive freelist metadata there.  Only pages owned by another
		 * mm should be rejected.
		 */
		if (READ_ONCE(arena->page_slot[page_idx]) == MD_INVALID_SLOT)
			allowed = true;
		else
			allowed = md_page_belongs_to_mm(arena, page_idx, mm);
		rcu_read_unlock();
		md_mm_ctx_put(ctx);
		return allowed;
	}

	return true;
}
EXPORT_SYMBOL_GPL(memory_delegation_fault_allowed);
