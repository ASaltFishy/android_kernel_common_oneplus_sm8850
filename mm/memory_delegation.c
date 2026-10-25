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
#include <linux/seqlock.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/task_work.h>
#include <linux/vmalloc.h>
#include <linux/shmem_fs.h>
#include <linux/uaccess.h>

#include "memory_delegation_debugfs.h"

#define MD_INVALID_SLOT 0xff
#define MD_RECLAIM_PENDING_SLOT 0xfe
#define MD_MM_CTX_HASH_BITS 10
#define MD_ACTIVE_MM_HASH_BITS 10
/* External payload only; the legacy allocator's VA layout is unchanged. */
#define MD_EXTERNAL_ARENA_CAPACITY	(2UL * 1024 * 1024 * 1024)
#define MD_ARENA_NR_PAGES_MAX\
	(MD_EXTERNAL_ARENA_CAPACITY / PAGE_SIZE)
#define MD_DEFAULT_OWNER_SLOTS		254
#define MD_PTE_DELTA_RUN_CAPACITY	4096U
#define MD_ARENA_MODE_EXTERNAL		2UL

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
	seqcount_t ownership_seq;
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
	/* Registration-time scratch; reset/use only while holding arena->lock. */
	unsigned long *dirty_chunks;
	u64 *page_allocation;
	u64 *page_block;
	u64 *page_generation;
	u64 last_allocation;
	u64 id;
	/* Published once; immutable until unregister + synchronize_rcu(). */
	struct md_shared_log *logs;
	struct page **log_pages;
	u32 log_nr_pages;
	u32 log_capacity;
	u32 log_flags;
	u64 normal_head;
	u64 remote_head;
	unsigned long shared_base;
	struct inode *payload_inode;
	pgoff_t payload_pgoff;
	struct md_owner_entry *owner_table;
	struct md_pte_delta_run *delta_runs;
};

struct md_mm_ctx_key {
	struct mm_struct *mm;
	u16 cpu;
};

struct md_mm_ctx {
	struct hlist_node node;
	struct hlist_node session_node;
	refcount_t refs;
	struct md_mm_ctx_key key;
	struct callback_head sync_work;
	unsigned long arena_base;
	u64 session;
	u64 arena_id;
	u64 last_seen_gen;
	unsigned long registration_mode;
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
static DEFINE_HASHTABLE(md_session_table, MD_MM_CTX_HASH_BITS);
static atomic64_t md_next_session = ATOMIC64_INIT(0);
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
	u64 arena_id;
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
static int md_mm_ctx_activate(struct md_mm_ctx *ctx, u64 initial_last_seen,
			      gfp_t gfp)
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
		WRITE_ONCE(ctx->last_seen_gen, initial_last_seen);
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
	if (refcount_dec_and_test(&ctx->refs))
		kfree(ctx);
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

/* 功能：打印 arena fault 被拒绝时的 ownership 快照；调用时机：RCU 读侧 fault 权限判断失败时调用。 */
static void md_log_fault_deny_rcu(const struct vm_area_struct *vma,
				  const struct md_vma_wrap *wrap,
				  const struct md_arena_meta *arena,
				  unsigned long address, u32 page_idx,
				  u8 slot)
{
	struct mm_struct *owner_mm = NULL;
	u64 global_commit_seq = 0;
	u32 page_owner_gen = 0;
	u32 owner_gen = 0;
	u32 owner_ref_pages = 0;
	u8 page_free = 0;
	u8 page_prefault = 0;
	bool owner_valid = false;

	if (arena && arena->id == wrap->arena_id && page_idx < arena->nr_pages) {
		global_commit_seq = READ_ONCE(arena->global_commit_seq);
		page_owner_gen = READ_ONCE(arena->page_owner_gen[page_idx]);
		page_free = READ_ONCE(arena->page_free[page_idx]);
		page_prefault = READ_ONCE(arena->page_prefault[page_idx]);
		if (slot < arena->owner_slots) {
			const struct md_owner_entry *owner;

			owner = &arena->owner_table[slot];
			owner_valid = READ_ONCE(owner->valid);
			owner_mm = READ_ONCE(owner->mm);
			owner_gen = READ_ONCE(owner->gen);
			owner_ref_pages = READ_ONCE(owner->ref_pages);
		}
	}

	pr_warn_ratelimited(
		"memory_delegation: deny arena access pid=%d tgid=%d comm=%s mm=%p addr=0x%lx cpu=%u arena_base=0x%lx page=%u page_off=0x%lx slot=%u page_owner_gen=%u page_free=%u page_prefault=%u owner_valid=%d owner_mm=%p owner_gen=%u owner_ref_pages=%u global_commit_seq=%llu active_mms=%d\n",
		current->pid, current->tgid, current->comm, vma ? vma->vm_mm : NULL,
		address, wrap ? wrap->cpu : UINT_MAX,
		wrap ? wrap->arena_base : 0UL, page_idx,
		wrap ? address - wrap->arena_base : 0UL, slot, page_owner_gen,
		page_free, page_prefault, owner_valid, owner_mm, owner_gen,
		owner_ref_pages, global_commit_seq,
		atomic_read(&md_active_mms));
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
	if (arena && arena->id == wrap->arena_id && page_idx < arena->nr_pages) {
		unsigned int seq;

		do {
			struct md_owner_entry *owner = NULL;
			struct mm_struct *owner_mm = NULL;
			u32 page_owner_gen = 0;
			u32 owner_gen = 0;
			bool owner_valid = false;

			seq = read_seqcount_begin(&arena->ownership_seq);
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
				continue;
			}

			if (slot >= arena->owner_slots) {
				allowed = false;
				continue;
			}

			owner = &arena->owner_table[slot];
			page_owner_gen = READ_ONCE(arena->page_owner_gen[page_idx]);
			owner_valid = READ_ONCE(owner->valid);
			owner_mm = READ_ONCE(owner->mm);
			owner_gen = READ_ONCE(owner->gen);
			allowed = owner_valid && owner_mm == vma->vm_mm &&
				owner_gen == page_owner_gen;
		} while (read_seqcount_retry(&arena->ownership_seq, seq));
		if (unlikely(!allowed))
			md_log_fault_deny_rcu(vma, wrap, arena, address,
					      page_idx, slot);
	} else {
		md_log_fault_deny_rcu(vma, wrap, arena, address, page_idx,
				      MD_INVALID_SLOT);
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
			      unsigned long arena_base, u32 nr_pages, u64 arena_id)
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
		if (wrap->cpu != cpu || wrap->arena_base != arena_base ||
		    wrap->arena_id != arena_id)
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
	wrap->arena_id = arena_id;
	wrap->arena_base = arena_base;
	wrap->owner_vma = vma;
	refcount_set(&wrap->refs, 1);

	vma->vm_private_data = wrap;
	vma->vm_ops = &md_arena_vm_ops;

	/* Ensure open() is run for consistency with other vm_ops users. */
	md_arena_vma_open(vma);
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
				     bool external_backend,
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
			if (!external_backend)
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
				arena->page_free[i] = !external_backend &&
					(i == start);
				arena->page_prefault[i] = 0;
				arena->page_owner_gen[i] = 0;
				if (slot < arena->owner_slots &&
				    arena->owner_table[slot].ref_pages)
					arena->owner_table[slot].ref_pages--;
				md_put_owner_slot_locked(arena, slot);
			}
		}
		md_mark_dirty_chunks(dirty_chunks, start, end);
		if (!external_backend)
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

/* arena->lock and ctx-table lock jointly serialize identity expiry and apply. */
static int md_apply_shared_locked(struct md_arena_meta *arena, unsigned int cpu,
		const struct md_shared_record *record, bool remote,
		unsigned long *dirty, struct md_switch_cycle_stats *stats)
{
	struct md_mm_ctx *ctx;
	struct md_shadow_log entry;
	u32 start = record->start_page, end, i;
	int ret = -ESTALE;

	if (record->cpu != cpu || record->reserved ||
	    (record->flags & ~MD_LOG_F_PREFAULT) || !record->nr_pages ||
	    !record->session || !record->block_id || !record->generation ||
	    !record->allocation_id ||
	    (remote && record->op != MD_LOG_FREE) ||
	    check_add_overflow(start, record->nr_pages, &end) ||
	    end > arena->nr_pages)
		return -EINVAL;

	spin_lock(&md_mm_ctx_table_lock);
	hash_for_each_possible(md_session_table, ctx, session_node, record->session) {
		if (ctx->session == record->session && ctx->key.cpu == cpu &&
		    ctx->active && !ctx->dead && ctx->arena_id == arena->id)
			goto found;
	}
	goto out;
found:
	if (record->op == MD_LOG_ALLOC) {
		/* An allocation incarnation is never reused, even after merge/split. */
		if (record->allocation_id <= arena->last_allocation)
			goto out;
	} else if (record->op == MD_LOG_FREE) {
		for (i = start; i < end; i++) {
			if (arena->page_allocation[i] != record->allocation_id ||
			    arena->page_block[i] != record->block_id ||
			    arena->page_generation[i] != record->generation)
				goto out;
		}
		/* Reject partial/extended releases of a registered allocation. */
		if ((start && arena->page_allocation[start - 1] == record->allocation_id) ||
		    (end < arena->nr_pages &&
		     arena->page_allocation[end] == record->allocation_id))
			goto out;
	} else {
		ret = -EINVAL;
		goto out;
	}

	entry.op = record->op;
	entry.src_cpu = 0; /* v2 CPU is u16; apply uses only op/range/flags. */
	entry.flags = record->flags;
	entry.start_page = start;
	entry.nr_pages = record->nr_pages;
	ret = md_apply_log_entry_locked(arena, &entry, ctx->key.mm,
			!(arena->log_flags & MD_SHARED_LOG_F_INTRUSIVE), dirty,
			arena->global_commit_seq + 1, stats);
	if (!ret && record->op == MD_LOG_ALLOC) {
		arena->last_allocation = record->allocation_id;
		memset64(arena->page_allocation + start, record->allocation_id,
			 record->nr_pages);
		memset64(arena->page_block + start, record->block_id, record->nr_pages);
		memset64(arena->page_generation + start, record->generation,
			 record->nr_pages);
	}
	/* Keep allocation identity after FREE to reject delayed/partial requests. */
out:
	spin_unlock(&md_mm_ctx_table_lock);
	return ret;
}

/* Sole owner-CPU consumer. The first fatal normal error survives slot reuse.
 * STOPPED rejects new publication; successful head never crosses this request.
 */
static void md_stop_normal_log(struct md_shared_log *logs, u64 ticket,
                               const struct md_shared_record *record, int error)
{
	if (smp_load_acquire(&logs->state) == MD_SHARED_LOG_STOPPED)
		return;
	if (record)
		logs->normal_error.record = *record;
	WRITE_ONCE(logs->normal_error.ready, record ? ticket + 1 : 0);
	WRITE_ONCE(logs->normal_error.result, error);
	WRITE_ONCE(logs->normal_error.reserved, 0);
	smp_store_release(&logs->state, MD_SHARED_LOG_STOPPED);
}

static bool md_drain_shared_arena(unsigned int cpu,
				 struct md_switch_cycle_stats *stats)
{
	unsigned long *dirty;
	struct md_arena_meta *arena;
	struct md_shared_log *logs;
	struct md_shared_slot *normal, *remote;
	u64 normal_tail, remote_tail, reserved, ticket;
	u32 capacity;
	bool drained = false;

	/* Sole consumer: called only by this CPU's IRQ-disabled scheduler hook. */
	lockdep_assert_irqs_disabled();
	rcu_read_lock();
	arena = md_get_arena_rcu(cpu);
	if (!arena)
		goto out;
	logs = smp_load_acquire(&arena->logs);
	if (!logs || smp_load_acquire(&logs->state) != MD_SHARED_LOG_RUNNING)
		goto out;
	capacity = arena->log_capacity; /* never trust mutable shared layout */
	normal = logs->slots;
	remote = normal + capacity;

	/* Snapshot remote READY prefix FIRST, normal published tail SECOND. */
	remote_tail = arena->remote_head;
	reserved = smp_load_acquire(&logs->remote.reserved);
	if (reserved < remote_tail || reserved - remote_tail > capacity)
		goto corrupt;
	while (remote_tail != reserved) {
		if (smp_load_acquire(&remote[remote_tail % capacity].ready) != remote_tail + 1)
			break; /* reservation hole: never wait, never skip */
		remote_tail++;
	}
	normal_tail = smp_load_acquire(&logs->normal.reserved);
	if (normal_tail < arena->normal_head ||
	    normal_tail - arena->normal_head > capacity)
		goto corrupt;
	if (normal_tail == arena->normal_head && remote_tail == arena->remote_head)
		goto out;

	spin_lock(&arena->lock);
	dirty = arena->dirty_chunks;
	bitmap_zero(dirty, arena->nr_chunks);
	write_seqcount_begin(&arena->ownership_seq);
	for (ticket = arena->normal_head; ticket != normal_tail; ticket++) {
		struct md_shared_slot *slot = &normal[ticket % capacity];
		struct md_shared_record record;
		int ret;

		if (smp_load_acquire(&slot->ready) != ticket + 1) {
			md_stop_normal_log(logs, ticket, NULL, -EPROTO);
			break;
		}
		record = slot->record;
		ret = md_apply_shared_locked(arena, cpu, &record, false, dirty, stats);
		if (ret) {
			md_stop_normal_log(logs, ticket, &record, ret);
			break;
		}
		if (record.op == MD_LOG_ALLOC) {
			MD_STATS_INC(stats, drain_alloc_entries);
			MD_STATS_ADD(stats, drain_alloc_pages, record.nr_pages);
		} else if (record.op == MD_LOG_FREE) {
			MD_STATS_INC(stats, drain_free_entries);
			MD_STATS_ADD(stats, drain_free_pages, record.nr_pages);
		}
		MD_STATS_INC(stats, drain_entries);
		MD_STATS_ADD(stats, drain_pages, record.nr_pages);
	}
	arena->normal_head = ticket;
	/* Never process remote FREE if a preceding normal publication is corrupt. */
	if (ticket == normal_tail) {
		for (ticket = arena->remote_head; ticket != remote_tail; ticket++) {
			struct md_shared_slot *slot = &remote[ticket % capacity];
			struct md_shared_record record = slot->record;
			int ret = md_apply_shared_locked(arena, cpu, &record, true, dirty, stats);

			WRITE_ONCE(slot->result, ret);
			MD_STATS_INC(stats, drain_free_entries);
			MD_STATS_ADD(stats, drain_free_pages, record.nr_pages);
			MD_STATS_INC(stats, drain_entries);
			MD_STATS_ADD(stats, drain_pages, record.nr_pages);
		}
		arena->remote_head = ticket;
	}
	md_commit_dirty_chunks_locked(arena, dirty);
	write_seqcount_end(&arena->ownership_seq);
	spin_unlock(&arena->lock);
	/* No normal success receipt: committed head directly frees normal slots.
	 * All prior normal requests succeeded, even if a later one stopped the log.
	 */
	smp_store_release(&logs->normal.head, arena->normal_head);
	smp_store_release(&logs->remote.completed, arena->remote_head);
	drained = true;
	goto out;
corrupt:
	md_stop_normal_log(logs, arena->normal_head, NULL, -EPROTO);
	pr_err_ratelimited("memory_delegation: cpu=%u corrupt shared queue; stopped consumption\n", cpu);
out:
	rcu_read_unlock();
	return drained;
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

/* Caller has stopped all producers; busy queues/owners retain their pins. */
static bool md_shared_can_unregister(struct md_arena_meta *arena)
{
	struct md_shared_log *logs = arena->logs;
	struct md_mm_ctx *ctx;
	unsigned int bkt;
	u32 i;
	bool idle = true;

	if (!logs)
		return true;
	if (smp_load_acquire(&logs->state) != MD_SHARED_LOG_STOPPED)
		return false;
	spin_lock_irq(&arena->lock);
	/* Existing registered mm contexts must detach by exit before teardown. */
	spin_lock(&md_mm_ctx_table_lock);
	hash_for_each(md_mm_ctx_table, bkt, ctx, node) {
		if (ctx->active && !ctx->dead && ctx->arena_id == arena->id) {
			idle = false;
			break;
		}
	}
	spin_unlock(&md_mm_ctx_table_lock);
	if (READ_ONCE(logs->normal.reserved) != arena->normal_head ||
	    READ_ONCE(logs->remote.reserved) != arena->remote_head ||
	    smp_load_acquire(&logs->remote.reclaimed) != arena->remote_head)
		idle = false;
	for (i = 0; i < arena->nr_pages && idle; i++) {
		if (arena->page_slot[i] != MD_INVALID_SLOT)
			idle = false;
	}
	spin_unlock_irq(&arena->lock);
	return idle;
}

static void md_shared_unpin(struct md_arena_meta *arena)
{
	u32 i;

	if (!arena->logs)
		return;
	vunmap(arena->logs);
	for (i = 0; i < arena->log_nr_pages; i++) {
		set_page_dirty_lock(arena->log_pages[i]);
		unpin_user_page(arena->log_pages[i]);
	}
	kvfree(arena->log_pages);
	iput(arena->payload_inode);
}

/* 功能：为指定 CPU 注册 arena 所有权元数据；调用时机：内核或 register_ring 初始化 per-CPU SharedArena 时调用。 */
int memory_delegation_arena_register(unsigned int cpu, unsigned int nr_pages,
				     unsigned int owner_slots)
{
	struct md_arena_meta *arena;
	struct md_arena_meta *old_arena;

	if (cpu >= nr_cpu_ids)
		return -EINVAL;
	if (!nr_pages || nr_pages > MD_ARENA_NR_PAGES_MAX ||
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

	arena->id = atomic64_inc_return(&md_next_session);
	if (!arena->id || arena->id > S64_MAX) {
		kfree(arena);
		return -EOVERFLOW;
	}
	arena->nr_pages = nr_pages;
	arena->nr_chunks = DIV_ROUND_UP(nr_pages, md_current_chunk_pages());
	arena->owner_slots = owner_slots;
	arena->delta_next_serial = 1;
	spin_lock_init(&arena->lock);
	seqcount_init(&arena->ownership_seq);

	arena->page_slot = kvzalloc(nr_pages, GFP_KERNEL);
	arena->page_free = kvzalloc(nr_pages, GFP_KERNEL);
	arena->page_prefault = kvzalloc(nr_pages, GFP_KERNEL);
	arena->page_owner_gen = kvcalloc(nr_pages, sizeof(*arena->page_owner_gen),
					 GFP_KERNEL);
	arena->page_allocation = kvcalloc(nr_pages, sizeof(u64), GFP_KERNEL);
	arena->page_block = kvcalloc(nr_pages, sizeof(u64), GFP_KERNEL);
	arena->page_generation = kvcalloc(nr_pages, sizeof(u64), GFP_KERNEL);
	arena->chunk_gen = kvcalloc(arena->nr_chunks, sizeof(*arena->chunk_gen),
				    GFP_KERNEL);
	/* Keep the larger arena bitmap off scheduler/exit kernel stacks. */
	arena->dirty_chunks = bitmap_zalloc(arena->nr_chunks, GFP_KERNEL);
	arena->owner_table = kvcalloc(owner_slots, sizeof(*arena->owner_table),
				      GFP_KERNEL);
	arena->delta_runs = kvcalloc(MD_PTE_DELTA_RUN_CAPACITY,
				     sizeof(*arena->delta_runs), GFP_KERNEL);
	if (!arena->page_slot || !arena->page_free || !arena->page_prefault ||
	    !arena->page_owner_gen || !arena->chunk_gen || !arena->dirty_chunks ||
	    !arena->owner_table || !arena->delta_runs ||
	    !arena->page_allocation || !arena->page_block || !arena->page_generation) {
		kvfree(arena->page_allocation);
		kvfree(arena->page_block);
		kvfree(arena->page_generation);
		kvfree(arena->delta_runs);
		kvfree(arena->owner_table);
		kvfree(arena->chunk_gen);
		bitmap_free(arena->dirty_chunks);
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
		int ret = (old_arena->nr_pages == nr_pages &&
			   old_arena->owner_slots == owner_slots) ? -EEXIST : -EBUSY;

		mutex_unlock(&md_arena_table_lock);
		kvfree(arena->page_allocation);
		kvfree(arena->page_block);
		kvfree(arena->page_generation);
		kvfree(arena->delta_runs);
		kvfree(arena->owner_table);
		kvfree(arena->chunk_gen);
		bitmap_free(arena->dirty_chunks);
		kvfree(arena->page_owner_gen);
		kvfree(arena->page_prefault);
		kvfree(arena->page_free);
		kvfree(arena->page_slot);
		kfree(arena);
		return ret;
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
	if (arena && !md_shared_can_unregister(arena)) {
		pr_warn_ratelimited("memory_delegation: cpu=%u unregister busy; logs retained\n", cpu);
		mutex_unlock(&md_arena_table_lock);
		return;
	}
	RCU_INIT_POINTER(md_arenas[cpu], NULL);
	if (arena)
		atomic_dec(&md_active_arenas);
	mutex_unlock(&md_arena_table_lock);

	if (!arena)
		return;

	synchronize_rcu();
	md_shared_unpin(arena);
	kvfree(arena->page_allocation);
	kvfree(arena->page_block);
	kvfree(arena->page_generation);
	kvfree(arena->delta_runs);
	kvfree(arena->owner_table);
	kvfree(arena->chunk_gen);
	bitmap_free(arena->dirty_chunks);
	kvfree(arena->page_owner_gen);
	kvfree(arena->page_prefault);
	kvfree(arena->page_free);
	kvfree(arena->page_slot);
	kfree(arena);
}
EXPORT_SYMBOL_GPL(memory_delegation_arena_unregister);

/* New registrations negotiate v3 and return an mm-lifetime session token. */
int memory_delegation_register_ring(unsigned int cpu, unsigned long arena_base,
				     unsigned long request_addr,
				     unsigned long flags)
{
	struct md_shared_registration req;
	struct md_shared_log *logs = NULL;
	struct md_arena_meta *arena;
	struct md_mm_ctx *ctx = NULL;
	struct page **pages = NULL;
	struct vm_area_struct *payload, *log_vma;
	struct inode *inode = NULL;
	pgoff_t pgoff;
	unsigned long log_addr, log_end, payload_end;
	u32 nr_pin = 0, i, bytes, capacity, nr_pages, log_flags;
	long pinned;
	int ret;
	bool installed = false;

	if (flags == PR_MEMORY_DELEGATION_F_SHARED_RINGS && !request_addr &&
	    !cpu && arena_base == MD_SHARED_LOG_QUERY)
		return MD_SHARED_LOG_VERSION; /* read-only; no Arena/backing created */
	if (flags != (PR_MEMORY_DELEGATION_F_EXTERNAL_BACKEND |
		      PR_MEMORY_DELEGATION_F_SHARED_RINGS))
		return -EPROTONOSUPPORT;
	if (cpu >= nr_cpu_ids || !cpu_possible(cpu) || !arena_base ||
	    !PAGE_ALIGNED(arena_base) || !request_addr)
		return -EINVAL;
	if (copy_from_user(&req, (void __user *)request_addr, sizeof(req)))
		return -EFAULT;
	if (req.size != sizeof(req) || req.version != MD_SHARED_LOG_VERSION ||
	    req.reserved || req.session || req.log_address > ULONG_MAX ||
	    !req.log_bytes || !PAGE_ALIGNED(req.log_bytes) ||
	    req.log_bytes / PAGE_SIZE > MD_SHARED_LOG_MAX_PAGES)
		return -EINVAL;
	log_addr = req.log_address;
	bytes = req.log_bytes;
	if (!log_addr || !PAGE_ALIGNED(log_addr) ||
	    check_add_overflow(log_addr, (unsigned long)bytes, &log_end))
		return -EINVAL;
	nr_pin = bytes / PAGE_SIZE;
	pages = kvcalloc(nr_pin, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;
	pinned = pin_user_pages_fast(log_addr, nr_pin,
				    FOLL_WRITE | FOLL_LONGTERM, pages);
	if (pinned != nr_pin) {
		ret = pinned < 0 ? (int)pinned : -EFAULT;
		nr_pin = pinned > 0 ? pinned : 0;
		goto out;
	}
	logs = vmap(pages, nr_pin, VM_MAP, PAGE_KERNEL);
	if (!logs) {
		ret = -ENOMEM;
		goto out;
	}
	capacity = READ_ONCE(logs->capacity);
	nr_pages = READ_ONCE(logs->arena_nr_pages);
	log_flags = READ_ONCE(logs->flags);
	if (logs->magic != MD_SHARED_LOG_MAGIC ||
	    logs->version != MD_SHARED_LOG_VERSION || logs->log_bytes != bytes ||
	    (log_flags & ~MD_SHARED_LOG_F_INTRUSIVE) || !capacity ||
	    capacity > (bytes - sizeof(*logs)) / (2 * sizeof(struct md_shared_slot)) ||
	    !nr_pages || nr_pages > MD_ARENA_NR_PAGES_MAX ||
	    smp_load_acquire(&logs->state) != MD_SHARED_LOG_RUNNING ||
	    check_add_overflow(arena_base,
		(unsigned long)nr_pages * PAGE_SIZE, &payload_end) ||
	    (log_addr < payload_end && arena_base < log_end)) {
		ret = -EINVAL;
		goto out;
	}
	ret = memory_delegation_arena_register(cpu, nr_pages,
					     MD_DEFAULT_OWNER_SLOTS);
	if (ret && ret != -EEXIST)
		goto out;
	ctx = md_mm_ctx_get_or_create(current->mm, cpu, GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto out;
	}

	/* Serialize registration/unregister, never used by the scheduler consumer. */
	mutex_lock(&md_arena_table_lock);
	arena = rcu_dereference_protected(md_arenas[cpu],
				lockdep_is_held(&md_arena_table_lock));
	if (!arena || arena->nr_pages != nr_pages) {
		ret = -ESTALE;
		goto unlock;
	}
	if (READ_ONCE(ctx->arena_base) && READ_ONCE(ctx->arena_base) != arena_base) {
		ret = -EINVAL;
		goto unlock;
	}
	if (ctx->session && ctx->arena_id != arena->id) {
		ret = -ESTALE;
		goto unlock;
	}
	if (arena->logs) {
		if (arena->log_nr_pages != nr_pin || arena->log_capacity != capacity ||
		    arena->log_flags != log_flags || arena->shared_base != arena_base) {
			ret = -EXDEV;
			goto unlock;
		}
		for (i = 0; i < nr_pin; i++) {
			if (arena->log_pages[i] != pages[i]) {
				ret = -EXDEV;
				goto unlock;
			}
		}
	} else {
		/* Only an empty broker-initialized log can become this Arena's log. */
		if (logs->normal.reserved || logs->normal.head || logs->normal_error.ready ||
		    logs->normal_error.result ||
		    logs->remote.reserved || logs->remote.completed || logs->remote.reclaimed) {
			ret = -EBUSY;
			goto unlock;
		}
		for (i = 0; i < 2 * capacity; i++) {
			if (logs->slots[i].ready) {
				ret = -EINVAL;
				goto unlock;
			}
		}
	}

	mmap_write_lock(current->mm);
	payload = find_vma(current->mm, arena_base);
	log_vma = find_vma(current->mm, log_addr);
	ret = -EINVAL;
	if (!payload || payload->vm_start > arena_base || payload->vm_end < payload_end ||
	    !(payload->vm_flags & VM_SHARED) || !(payload->vm_flags & VM_WRITE) ||
	    !payload->vm_file || !shmem_mapping(payload->vm_file->f_mapping) ||
	    !log_vma || log_vma->vm_start > log_addr || log_vma->vm_end < log_end ||
	    !(log_vma->vm_flags & VM_SHARED) || !log_vma->vm_file ||
	    !shmem_mapping(log_vma->vm_file->f_mapping))
		goto mmap_unlock;
	inode = file_inode(payload->vm_file);
	pgoff = payload->vm_pgoff + ((arena_base - payload->vm_start) >> PAGE_SHIFT);
	if (inode == file_inode(log_vma->vm_file))
		goto mmap_unlock; /* log storage cannot alias payload at another VA */
	if (arena->logs && (arena->payload_inode != inode || arena->payload_pgoff != pgoff)) {
		ret = -EXDEV;
		goto mmap_unlock;
	}
	ret = md_install_vma_ops(current->mm, cpu, arena_base, arena->nr_pages, arena->id);
	if (!ret && !arena->logs) {
		arena->payload_inode = igrab(inode);
		if (!arena->payload_inode) {
			ret = -ESTALE;
			goto mmap_unlock;
		}
		arena->payload_pgoff = pgoff;
		arena->shared_base = arena_base;
		arena->log_pages = pages;
		arena->log_nr_pages = nr_pin;
		arena->log_capacity = capacity;
		arena->log_flags = log_flags;
		/* All layout and kernel-private cursors initialized before publication. */
		smp_store_release(&arena->logs, logs);
		installed = true;
	}
 mmap_unlock:
	mmap_write_unlock(current->mm);
	if (ret)
		goto unlock;
	WRITE_ONCE(ctx->arena_base, arena_base);
	WRITE_ONCE(ctx->registration_mode, MD_ARENA_MODE_EXTERNAL);
	ret = md_mm_ctx_activate(ctx, 0, GFP_KERNEL);
	if (ret)
		goto unlock;
	spin_lock_irq(&md_mm_ctx_table_lock);
	if (!ctx->session) {
		/* Never use PID, a raw mm pointer, or an owner slot as a session ID. */
		u64 session = atomic64_inc_return(&md_next_session);

		if (!session || session > S64_MAX) {
			ret = -EOVERFLOW;
		} else {
			ctx->session = session;
			ctx->arena_id = arena->id;
			hash_add(md_session_table, &ctx->session_node, session);
		}
	}
	req.session = ctx->session;
	spin_unlock_irq(&md_mm_ctx_table_lock);
 unlock:
	mutex_unlock(&md_arena_table_lock);
	if (!ret && copy_to_user((void __user *)request_addr, &req, sizeof(req)))
		ret = -EFAULT; /* retry registration returns the same session */
 out:
	if (ctx)
		md_mm_ctx_put(ctx);
	if (!installed) {
		if (logs)
			vunmap(logs);
		for (i = 0; i < nr_pin; i++)
			unpin_user_page(pages[i]);
		kvfree(pages);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(memory_delegation_register_ring);

/* Retained kernel symbol for ABI stability; no explicit drain entry remains. */
int memory_delegation_submit_log(unsigned int cpu, unsigned long arena_base,
				 const struct md_shadow_log *log,
				 unsigned int nr_entries)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(memory_delegation_submit_log);

/* Owner CPU drains its shared Arena queues; next keeps all necessary PTE sync. */
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

	/* Consume this CPU's Arena even when neither task participates. */
	if (!atomic_read(&md_active_arenas))
		return;
	cpu = smp_processor_id();
	{
		MD_TIME_START(drain, "drain");
		u64 delta;

		switch_drained_nonempty = md_drain_shared_arena(cpu, stats);
		delta = MD_TIME_END(drain);
		MD_STATS_INC(stats, drain_calls);
		MD_STATS_ADD_MAX(stats, drain_cycles, max_drain_cycles, delta);
		if (switch_drained_nonempty) {
			MD_STATS_INC(stats, drain_nonempty_calls);
			MD_STATS_ADD_MAX(stats, drain_nonempty_cycles,
				max_drain_nonempty_cycles, delta);
		} else {
			MD_STATS_INC(stats, drain_empty_calls);
			MD_STATS_ADD_MAX(stats, drain_empty_cycles,
				max_drain_empty_cycles, delta);
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
	{
		unsigned int arena_cpu;

		for_each_possible_cpu(arena_cpu) {
			struct md_mm_ctx *ctx;
			struct md_arena_meta *arena;

			if (arena_cpu == cpu)
				continue;
			ctx = md_mm_ctx_lookup_get(next->mm, arena_cpu);
			if (!ctx)
				continue;
			if (READ_ONCE(ctx->registration_mode) == MD_ARENA_MODE_EXTERNAL) {
				rcu_read_lock();
				arena = md_get_arena_rcu(arena_cpu);
				if (arena)
					switch_queued_pte_sync |= md_queue_sync_if_needed(
						ctx, next,
						READ_ONCE(arena->global_commit_seq));
				rcu_read_unlock();
			}
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

/* Revoke operations commute until a prefault: merge their contiguous union
 * without expanding over holes or moving an unmap across a prefault barrier.
 */
static void md_flush_delta_unmap(struct mm_struct *mm,
				 unsigned long *start, unsigned long *end,
				 bool *pte_modified,
				 struct md_pte_sync_detail *detail)
{
	if (*end > *start) {
		MD_TIME_START(unmap, "pte_delta_unmap");
		md_unmap_range_locked(mm, *start, *end);
		MD_DETAIL_ADD(detail, unmap_cycles, MD_TIME_END(unmap));
		MD_DETAIL_INC(detail, unmap_ranges);
		MD_DETAIL_ADD(detail, unmap_pages, (*end - *start) >> PAGE_SHIFT);
		*pte_modified = true;
	}
	*start = *end = 0;
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
	unsigned long unmap_start = 0, unmap_end = 0;
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

	/* Serial order has nondecreasing commit seq. Inspect only the retained
	 * suffix newer than this mm, rather than rescanning all 4096 old runs.
	 */
	for (serial = next_serial; serial > first_serial;) {
		const struct md_pte_delta_run *run;

		serial--;
		run = &arena->delta_runs[serial % MD_PTE_DELTA_RUN_CAPACITY];

		if (run->serial != serial)
			continue;
		if (run->seq <= last_seen)
			break;
		if (run->seq > target_seq)
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

	for (serial = next_serial; serial > first_serial;) {
		const struct md_pte_delta_run *run;

		serial--;
		run = &arena->delta_runs[serial % MD_PTE_DELTA_RUN_CAPACITY];

		if (run->serial != serial)
			continue;
		if (run->seq <= last_seen)
			break;
		if (run->seq > target_seq)
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
		/* Backward lookup, chronological replay (including prefaults). */
		runs[wanted - 1 - count++] = *run;
	}
	spin_unlock(&arena->lock);
	rcu_read_unlock();
	if (count != wanted) {
		kfree(runs);
		MD_DETAIL_INC(detail, delta_fallbacks);
		MD_DETAIL_ADD(detail, scan_cycles, MD_TIME_END(scan));
		return -EAGAIN;
	}

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

			if (unmap_end > unmap_start &&
			    (start > unmap_end || end < unmap_start))
				md_flush_delta_unmap(mm, &unmap_start, &unmap_end,
						     pte_modified, detail);
			if (unmap_end > unmap_start) {
				unmap_start = min(unmap_start, start);
				unmap_end = max(unmap_end, end);
			} else {
				unmap_start = start;
				unmap_end = end;
			}
			continue;
		}

		if (run->op == MD_PTE_DELTA_PREFAULT_FREE) {
			MD_DETAIL_INC(detail, delta_prefault_runs);
			md_flush_delta_unmap(mm, &unmap_start, &unmap_end,
					     pte_modified, detail);
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
	md_flush_delta_unmap(mm, &unmap_start, &unmap_end, pte_modified, detail);

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
		unsigned long registration_mode;

		old_ctx = md_mm_ctx_lookup_get(old_mm, cpu);
		if (!old_ctx)
			continue;

		arena_base = READ_ONCE(old_ctx->arena_base);
		registration_mode = READ_ONCE(old_ctx->registration_mode);
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
		WRITE_ONCE(new_ctx->registration_mode, registration_mode);
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

/* Expire sessions before quarantining; never consume or free shared log storage. */
void memory_delegation_mm_release(struct mm_struct *mm)
{
	struct md_mm_ctx *ctx;
	struct hlist_node *tmp;
	unsigned int cpu, bkt;
	bool had_active;

	if (!mm)
		return;
	had_active = md_mm_has_active_ctx(mm);
	spin_lock(&md_mm_ctx_table_lock);
	hash_for_each_safe(md_mm_ctx_table, bkt, tmp, ctx, node) {
		if (ctx->key.mm != mm)
			continue;
		ctx->dead = true;
		if (!hlist_unhashed(&ctx->session_node))
			hash_del(&ctx->session_node);
		if (ctx->active) {
			ctx->active = false;
			md_active_mm_put_locked(mm);
		}
		hash_del(&ctx->node);
		md_mm_ctx_put(ctx);
	}
	spin_unlock(&md_mm_ctx_table_lock);

	if (!had_active)
		return;

	/* Exit is lifecycle cleanup, not a second log-consumption entry point. */
	for_each_possible_cpu(cpu) {
		unsigned long *dirty;
		struct md_arena_meta *arena;
		unsigned long flags;
		u32 i;
		u64 pending_seq;

		rcu_read_lock();
		arena = md_get_arena_rcu(cpu);
		if (!arena) {
			rcu_read_unlock();
			continue;
		}
		spin_lock_irqsave(&arena->lock, flags);
		dirty = arena->dirty_chunks;
		bitmap_zero(dirty, arena->nr_chunks);
		pending_seq = arena->global_commit_seq + 1;
		write_seqcount_begin(&arena->ownership_seq);
		for (i = 0; i < arena->nr_pages; i++) {
			u16 slot = arena->page_slot[i];

			if (!md_page_belongs_to_mm_locked(arena, i, mm))
				continue;
			/* Descriptors cannot be reconstructed safely after owner death. */
			arena->page_slot[i] = MD_RECLAIM_PENDING_SLOT;
			arena->page_free[i] = 0;
			arena->page_prefault[i] = 0;
			arena->page_owner_gen[i] = 0;
			if (arena->owner_table[slot].ref_pages)
				arena->owner_table[slot].ref_pages--;
			md_put_owner_slot_locked(arena, slot);
			md_mark_dirty_chunks(dirty, i, i + 1);
			md_record_pte_delta_locked(arena, pending_seq,
				MD_PTE_DELTA_REVOKE_NON_OWNER,
				MD_RECLAIM_PENDING_SLOT, 0, i, 1);
		}
		md_commit_dirty_chunks_locked(arena, dirty);
		write_seqcount_end(&arena->ownership_seq);
		spin_unlock_irqrestore(&arena->lock, flags);
		rcu_read_unlock();
	}
}
EXPORT_SYMBOL_GPL(memory_delegation_mm_release);
