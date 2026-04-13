# Memory Delegation 内核实现审查意见

> 审查范围：`kernel_platform/common/` 下所有 memory delegation 相关改动
> 对照文档：`DESIGN.md` §2.3–§2.10
> 涉及文件：`mm/memory_delegation.c`、`include/linux/memory_delegation.h`、`include/uapi/linux/memory_delegation.h`、`kernel/sched/core.c`、`kernel/sys.c`、`mm/Kconfig`、`mm/Makefile`

---

## 问题 #1【P0 架构】同步模型倒置——应由内核在调度边界主动同步，而非依赖用户态 syscall

### 涉及代码

- `memory_delegation_on_context_switch()` — 仅设置 `needs_pt_sync = true`
- `memory_delegation_submit_log_and_sync()` — 仅由 `prctl` 用户态入口调用
- `prctl_set_memory_delegation_log()` — `kernel/sys.c` 中通过 `memdup_user` 拷贝日志

### 问题描述

当前实现将日志提交和 PTE 同步**全部**推给用户态主动发起的 `prctl(PR_SET_MEMORY_DELEGATION_LOG)` 调用。`context_switch` 中设置的 `needs_pt_sync` 标志在整个代码库中**从未被任何 `if` 语句检查**，是死代码。

这违背了 DESIGN.md §2.3 的核心安全约束：

> "当该核心发生上下文切换时，内核拦截调度路径，先处理 prev 进程尚未提交的修改日志并更新 page_slot[]/chunk_gen[]，再结合 next 进程上次已同步的代际号，只检查发生变化的 chunk。内核据此修改 next 的页表，取消其对已被其他进程拿走的内存块的映射权限。"

**安全后果**：进程 A 在 arena 上分配了内存，进程 B 被调度到同一核心后，如果 B 不主动调用 prctl，它的旧 PTE 映射不会被撤销——B 仍能读写已被 A 拿走的页面。

### 修复方案

该问题涉及三个子问题，需要一起解决：

**（a）日志传递：prctl + copy_from_user → 共享环形缓冲区**

DESIGN.md §2.4 明确说"用户态热路径仅追加轻量修改日志"。当前每次提交都要走 syscall + `memdup_user`（kmalloc + copy_from_user + kfree），延迟和开销完全不符合"轻量"的定位。

应改为 per-arena 共享 ring buffer，mmap 到用户态和内核态：
- 用户态：`log[tail++] = entry`，纯用户态写入，零 syscall
- 内核态：在 `context_switch` 中直接读 `log[head..tail]`，零拷贝
- 参考内核已有范式：`perf_event` ring buffer、io_uring SQ/CQ

**（b）日志消费时机：调度路径中消费 prev 的 log**

`context_switch` 原子路径中应直接消费 prev 进程的 ring buffer 日志、更新 `page_slot[]/owner_table[]/chunk_gen[]` 真值表。这些操作仅涉及 spinlock + 内存写入，不涉及 mmap_lock 或页表操作，可以在原子上下文完成。

**（c）PTE 撤销时机：TIF flag + 返回用户态前强制同步**

§2.8 明确 `zap_page_range` 需要 `mmap_write_lock`，不能在原子路径执行。正确做法是利用 TIF（Thread Info Flag）机制：

```c
/* context_switch 原子路径 */
if (latest_seq > ctx->last_seen_gen)
    set_tsk_thread_flag(next, TIF_MD_SYNC_NEEDED);

/* arch/arm64 返回用户态前 (类似 TIF_SIGPENDING 的处理) */
if (test_and_clear_thread_flag(TIF_MD_SYNC_NEEDED)) {
    mmap_write_lock(current->mm);
    memory_delegation_sync_mm(...);
    mmap_write_unlock(current->mm);
}
```

这样 next 进程在返回用户态执行任何代码之前，PTE 就已被撤销。无需用户态配合，无需额外 syscall。

**（d）不保留 prctl 入口**


---

## 问题 #2【P0 安全】UAPI 暴露内核私有概念，日志提交缺乏 ownership 验证

### 涉及代码

- `include/uapi/linux/memory_delegation.h` — `struct md_shadow_log` 定义
- `md_apply_log_entry()` — 日志消费逻辑

### 问题描述

**（a）UAPI 结构体泄漏内核实现细节**

```c
struct md_shadow_log {
    __u8 op;
    __u8 arena_id;
    __u16 owner_slot;   /* ← 内核内部索引，用户态不应知道 */
    __u32 start_page;
    __u32 nr_pages;
    __u32 owner_gen;    /* ← 内核内部代际，用户态不应知道 */
    __u64 seq;
};
```

`owner_slot` 和 `owner_gen` 是内核真值层的实现细节。用户态分配器只知道"我在 arena X 上分配/释放了 page Y 到 Z"，不知道也不应该知道自己对应哪个 slot 或当前 generation。暴露这些字段：

1. 违反 DESIGN.md §2.3"用户态不直接修改这份真值表"的原则
2. 创建安全攻击面——恶意进程可伪造 slot/gen 值

**（b）日志消费无 ownership 校验**

`md_apply_log_entry` 中：
- `MD_LOG_ALLOC`：直接用用户提供的 `owner_slot/owner_gen` 覆写 `owner_table[slot]`，不验证目标页是否空闲
- `MD_LOG_FREE`：直接将页标为空闲，不验证页是否属于 `current->mm`

任何进程都可以通过伪造日志抢占其他进程的页面或释放不属于自己的页面。

**（c）`seq` 字段定义但从未使用——乱序/重放无检测**

UAPI 结构体中定义了 `__u64 seq`，但 `md_apply_log_entry()` 和 `memory_delegation_submit_log()` 从未读取该字段。日志按数组下标顺序盲目处理，无排序、无去重、无间隙检测。用户态可以重复提交同一批日志（重放攻击）或乱序提交，内核无法察觉。

### 修复方案

**UAPI 精简为最小信息集**：

```c
struct md_shadow_log {
    __u8  op;           /* MD_LOG_ALLOC / MD_LOG_FREE */
    __u8  reserved;
    __u32 start_page;   /* arena 内页偏移 */
    __u32 nr_pages;     /* 页数 */
};
```

- `arena_id` 在共享 ring buffer 方案中由 buffer 本身隐含（每个 arena 一个 buffer）。
- `owner_slot`、`owner_gen`、`seq` 全部移除——前两者由内核自管，后者由 ring buffer 的 FIFO 语义和 head/tail 指针天然保证。

**内核侧自行管理 slot 和 gen**：
- `MD_LOG_ALLOC`：内核查找或分配 `current->mm` 对应的 slot，校验目标页当前为 `MD_INVALID_SLOT`，自增并记录 gen
- `MD_LOG_FREE`：内核校验目标页 owner 是 `current->mm`，然后清除

---

## 问题 #3【P0 安全】Page fault 路由未实现——安全模型不闭环

### 涉及代码

- `memory_delegation_fault_allowed()` — 已实现但从未被调用
- `mm/memory.c` / `arch/arm64/mm/fault.c` — 未挂接

### 问题描述

DESIGN.md §2.6 定义了安全红线：

> "被 unmap 的 Arena 页在当前 mm 上访问时，必然触发缺页异常。fault helper 必须先查 page_slot[] + owner_table[] 真值：若 owner 已是其他 mm，必须拒绝访问并返回 SIGSEGV；若页为空闲 slot，同样拒绝访问。"

当前实现中：
1. `memory_delegation_fault_allowed` 函数存在但**从未被 page fault 路径调用**
2. 函数签名要求调用者提供 `(cpu, arena_id, arena_base)` 参数，但 page fault handler 只知道 `(mm, address)`，**无法提供这些参数**

后果：arena 页被 unmap 后，fault 走默认匿名页路径分配新物理页，进程绕过 ownership 检查获得对 arena 地址的访问。

### 修复方案

**在 VMA 上携带 arena 元数据**：

arena mmap 时，内核在 VMA 上设置标识（`vm_flags` 新增 `VM_MEMORY_DELEGATION` 标记位，或使用 `vm_private_data` 存储 arena 描述符），包含 `cpu` 和 `arena_base` 信息。

**fault 路径挂接**：

在 `do_anonymous_page` 或 `handle_pte_fault` 中增加判断：检测 VMA 的 `VM_MEMORY_DELEGATION` 标记后，提取 arena 元数据，调用 `memory_delegation_fault_allowed`。若返回 false，返回 `VM_FAULT_SIGSEGV`。

函数签名简化（cpu 和 arena_base 从 VMA 获取）：

```c
bool memory_delegation_fault_allowed(struct mm_struct *mm,
                                     unsigned long address,
                                     const struct vm_area_struct *vma);
```

---

## 问题 #4【P1 安全】`submit_log()` 与 `sync_mm()` 之间无一致性快照——generation 协议被并发写穿

### 涉及代码

- `memory_delegation_submit_log()` — 持 `arena->lock` 写入真值表
- `memory_delegation_sync_mm()` — **不持 `arena->lock`**，仅靠 `READ_ONCE()` 裸读真值表

### 问题描述

两个函数访问同一份真值数据（`page_slot[]`、`page_owner_gen[]`、`owner_table[]`、`chunk_gen[]`），但使用完全不同的锁：

| 操作 | `arena->lock` | `md_arena_table_lock` |
|------|--------------|----------------------|
| `submit_log` 写真值 | ✅ 持有 | ❌ |
| `sync_mm` 读真值 | ❌ **不持有** | ✅（但此锁不保护真值） |

`sync_mm` 扫描真值时与 `submit_log` 写入真值**完全不互斥**。`READ_ONCE()` 只保证单变量原子性，不保证跨数组的快照一致性。

**攻击时序**（进程 B 在 sync，进程 D 在另一 CPU 并发 submit）：

```
CPU 0: 进程 B sync_mm                 CPU 1: 进程 D submit_log
─────────────────────────────         ─────────────────────────────
target_seq = READ_ONCE(seq) → 5
扫描 chunk C...
读 page P: slot=slot_b, mm=B
→ owned=true, 跳过 unmap              spin_lock(arena->lock)
                                      FREE page P → slot=INVALID
                                      ALLOC page P → slot=slot_d, mm=D
                                      seq → 6, chunk_gen[C] → 6
                                      spin_unlock(arena->lock)
扫描完成
last_seen_gen = 5
```

此刻 page P 真值属于 D，但 B 仍有 PTE 映射——**越权访问窗口成立**。

chunk_gen 机制提供了**最终一致性**（下次 sync 时 `chunk_gen[C]=6 > last_seen=5` 会触发重扫），但安全窗口的长度取决于"下次 sync 何时发生"。在当前 prctl-only 模型下，该窗口**可能无限长**。

### 修复方案

**最小修复**：`sync_mm` 在扫描 chunk 页面时持有 `arena->lock`（读侧），与 `submit_log`（写侧）互斥。由于 `arena->lock` 是 spinlock 而 `sync_mm` 需要调用 `zap_page_range_single`（可睡眠），应拆分为：

```c
spin_lock(&arena->lock);
/* 快照当前 chunk 的 page_slot/page_owner_gen 到本地数组 */
memcpy(local_slot, &arena->page_slot[start_page], chunk_pages);
memcpy(local_gen, &arena->page_owner_gen[start_page], chunk_pages * sizeof(u32));
snapshot_seq = arena->global_commit_seq;
spin_unlock(&arena->lock);

/* 基于本地快照决定 unmap 范围（可睡眠） */
for (page ...) { ... md_unmap_range_locked(...); }
```

**配合 #1 的整体修复**：当日志消费（submit）和 PTE 同步（sync）都由内核在可控时机执行时，可通过将两者串行化在同一调用链中来消除竞态——先持 `arena->lock` 消费 log 更新真值，释放锁后基于刚写入的确定性状态做 unmap。

---

## 问题 #5【P1 资源】`md_mm_ctx` 永不释放——内存泄漏 + 悬空指针

### 涉及代码

- `md_mm_ctx_get_or_create()` — 分配 ctx
- `md_mm_ctx_table` 全局哈希表 — 无清理路径

### 问题描述

每个 `(mm, cpu)` 二元组会分配一个 `md_mm_ctx`，插入全局哈希表。但不存在任何清理路径：

- 进程退出（`mm` 释放）后，哈希表条目仍在，`ctx->key.mm` 变成悬空指针
- `mm_struct` 经 slab 复用后，旧 ctx 可能错误匹配新进程，导致 `last_seen_gen` 值错乱
- 长期运行后哈希表无限增长

### 修复方案

注册 `mmu_notifier` 或在 `exit_mm` / `__mmput` 路径中增加回调，遍历哈希表删除该 `mm` 的所有 ctx 条目并 kfree。

---

## 问题 #6【P2 设计】arena:cpu 为 1:1，`arena_id` 维度冗余

### 涉及代码

- `MD_MAX_ARENAS_PER_CPU = 4`
- `md_arenas[NR_CPUS][MD_MAX_ARENAS_PER_CPU]` 二维数组
- `md_mm_ctx_key` 三元组 `(mm, cpu, arena_id)`
- `memory_delegation_on_context_switch` 中 `for (arena_id = 0..3)` 循环

### 问题描述

DESIGN.md §2.1 明确定义"为系统的每一个 CPU 核心维护**一个**全局共享的内存池"，即 arena:cpu = 1:1。代码中引入的 `arena_id` 维度无设计依据，且导致 context_switch 热路径做 4 次无用循环（每次含 RCU 查找 + 可能的 GFP_ATOMIC kzalloc）。

### 修复方案

- `md_arenas` 从 `[NR_CPUS][4]` 降为 `[NR_CPUS]`（或使用 `DEFINE_PER_CPU`）
- `md_mm_ctx_key` 从三元组 `(mm, cpu, arena_id)` 降为二元组 `(mm, cpu)`
- 所有 API 移除 `arena_id` 参数
- context_switch 中消除循环，直接单次访问

---

## 问题 #7【P2 性能】全局 mutex 持锁期间执行 unmap——可扩展性瓶颈

### 涉及代码

- `memory_delegation_sync_mm()` — 持 `md_arena_table_lock` 遍历所有 chunk + `zap_page_range_single`

### 问题描述

`md_arena_table_lock` 是全局 mutex，被 `arena_register/unregister/sync_mm` 共享。`sync_mm` 在持锁期间线性扫描所有 chunk 并调用 `zap_page_range_single`（涉及 TLB flush），阻塞时间与 arena 页数成正比。所有 CPU 上所有进程的 sync 操作被这一把锁串行化。

### 修复方案

`md_arena_table_lock` 仅保护 arena 注册/注销（结构变更）。`sync_mm` 改为：在 mutex 保护下获取 arena 指针并验证存在性，然后释放 mutex，在 per-arena spinlock（或 RCU + arena 引用计数）保护下执行 chunk 扫描和 unmap。

---

## 问题 #8【P2 性能】`GFP_ATOMIC` 分配用于可睡眠的进程上下文

### 涉及代码

- `memory_delegation_submit_log()` — `bitmap_zalloc(arena->nr_chunks, GFP_ATOMIC)`

### 问题描述

`submit_log` 从 prctl（进程上下文，可睡眠）调用，但因整段逻辑被 `rcu_read_lock()` 包裹而被迫使用 `GFP_ATOMIC`。在内存紧张时（恰恰是 memory delegation 需要发挥作用的时候）更容易分配失败。

### 修复方案

重构为：先在 RCU 下读取 `arena->nr_chunks`，释放 RCU，用 `GFP_KERNEL` 分配 bitmap，再重新获取 arena 并校验 `nr_chunks` 未变。或在共享 ring buffer 方案下，dirty_chunks bitmap 可作为 arena_meta 的一部分预分配。

---

## 问题 #9【P3 质量】哈希函数分布不均

### 涉及代码

- `md_mm_ctx_hash()` — 自写 XOR 哈希

### 问题描述

`mm_struct` 指针低 6+ 位为零（slab 对齐），`cpu` 左移 8 位后与指针 XOR，在 `MD_MM_CTX_HASH_BITS=10`（1024 桶）下取低 10 位分布不均匀，导致哈希冲突集中。

### 修复方案

使用内核标准哈希函数 `hash_long()` 或 `hash_ptr()`。

---

## 问题 #10【P3 质量】`md_mm_ctx_get_or_create` 竞争路径的脆弱假设

### 涉及代码

```c
kfree(ctx);
spin_lock(&md_mm_ctx_table_lock);
ctx = md_mm_ctx_lookup_locked(&key);  /* 假设一定非 NULL */
spin_unlock(&md_mm_ctx_table_lock);
return ctx;
```

### 问题描述

double-check locking 的失败路径中，kfree 自己的 ctx 后再次 lookup 获取竞争者创建的 ctx。当前因为没有 ctx 销毁路径，此处不会返回 NULL。但一旦实现问题 #4 的清理逻辑，竞争者的 ctx 可能在两次 spin_unlock 之间被清理，导致返回 NULL 并在调用方触发空指针解引用。

### 修复方案

在问题 #5 引入 ctx 生命周期管理时，使用引用计数（`kref`）保护 ctx，确保持有引用期间不会被释放。

---

## 修改优先级总览

| 优先级 | 问题 | 核心改动 |
|--------|------|----------|
| **P0** | #1 同步模型倒置 | 共享 ring buffer + context_switch 消费 log + TIF flag + 返回用户态前 unmap |
| **P0** | #2 UAPI 泄漏 + 无校验 + seq 死代码 | 精简 UAPI、内核自管 slot/gen/seq、增加 ownership 校验 |
| **P0** | #3 fault 路由缺失 | VMA 标记 + fault path 挂接 |
| **P1** | #4 submit/sync 一致性撕裂 | sync 扫描前快照真值（持 arena->lock），基于快照做 unmap |
| **P1** | #5 ctx 内存泄漏 | exit_mm 清理回调 + kref 引用计数 |
| **P2** | #6 arena_id 冗余 | 去除 arena_id 维度，1:1 CPU:arena |
| **P2** | #7 全局 mutex | 缩小 mutex 持锁范围，unmap 在锁外执行 |
| **P2** | #8 GFP_ATOMIC | 重构 RCU 持有范围，改用 GFP_KERNEL |
| **P3** | #9 哈希函数 | 换用 `hash_long()` |
| **P3** | #10 ctx 竞争路径 | 配合 #5 引入 kref |
