面向智能时代的智能终端极致内存资源利用系统设计
一、 设计背景与破局思路
随着端侧大模型和智能体的规模化部署，终端内存常面临突发性的 XG/s 量级申请。传统的内存管理存在两个致命瓶颈：
1. 进程间资源孤岛与路径冗长：原生 Scudo 分配器中，各进程的 Secondary 分配器及其暂存大块内存的 MapAllocatorCache 是严格隔离的。当进程 B 有大量 Cache 空闲，而进程 A 突发大内存需求时，A 只能触发内核的 mmap 和繁重的页面回收（Evict/Swap），无法直接复用 B 的闲置资源。
2. 内核语义断层：面对 AI 应用庞大的 KV Cache，内核的通用 LRU 算法无法识别其业务价值，容易导致关键冷数据被误杀，而可重算的 KV Cache 却占用宝贵空间。
3. 基于此，本系统从用户态内存委托（横向跨进程共享）与内核态语义协同（纵向跨层级回收）两个维度进行架构创新。

---
二、 子课题一：基于 Scudo 扩展的用户态内存委托机制
核心目标：打破进程间的 MapAllocatorCache 孤岛，实现内存页所有权的极速转移（零拷贝委托）。

2.1 核心架构：每核共享预映射内存池 (Per-Core Shared Arena)
我们将原本属于单个进程的 MapAllocatorCache 升级为跨进程共享的更大 MapAllocatorPool。
- 物理内存池化：为系统的每一个 CPU 核心维护一个全局共享的内存池（Arena）。使用每核心存储（Per-CPU Storage）保存该内存池的 head 指针。在secondary分配大页面时，直接从这个Pool中分配。初始化时，每个进程都通过shared memory接口map好arena；Android 侧 shared memory backing 由常驻系统服务 `memory_delegation_broker` 创建并持有，应用进程只向 broker 获取 fd 并 attach，禁止由任意应用进程创建 memfd、抢 creator lock 或 fork 临时 broker。当系统内存压力达到一个水位线后，分配器开始放弃原本的MapAllocatorCache路径，转为直接在MapAllocatorPool上进行分配，后续物理内存通过实际访问时的page fault获得。
- 统一虚拟地址预留：为了解决不同进程获取到的内存块在虚拟地址上不连续的问题，在每个进程的虚拟地址空间中，预先划分出一段足够大且固定大小（arena_size 作为全局常量）的连续虚拟地址区间，专门用于映射这些 Arena。实现上，先通过按需分配页面的方式创建shared memory，虚拟地址vma在此时生效，分配时走pagefault方式生成页面。
- 缓存交接：当进程的 Secondary 分配器释放大块内存时，不再私有化成为 MapAllocatorCache，而是直接加入该块虚拟地址归属的 Arena（owner arena）中，供其他进程借用。跨核迁移后释放仍必须回到原 owner arena，不能改投当前执行核心的 Arena。若 Arena 暂存的数据量超过 arena_size，则直接调用 unmap 交还给内核。

2.2 分配机制：当前核心获取与 Mutex 保护的共享状态更新
- rseq 与用户态互斥：rseq 可用于辅助当前 CPU/Arena 的选择及短序列提交，但不能把包含阻塞 Mutex 的整个分配路径放入 rseq 临界区，也不能把 rseq 理解为禁止抢占。本核正常分配/释放、普通日志发布和本核回收遵守 §2.10 的进程共享 Mutex 协议；跨核 FREE 仅向独立 ring 原子发布请求，不拿 Arena metadata 锁、不修改描述符。迁核后的 Arena 选择须重新校验，释放始终按块的 owner Arena 路由。
- 极速委托获取：当某线程急需大块内存时，分配器优先从当前核心的 Arena 中摘取内存块分配给它（不再受 MapAllocatorCache 最大条目尺寸限制）。若 Arena 的 freelist 与 bump 均无法满足请求，再回退到原有路径（Cache / mmap）。在 Arena 命中的情况下全程无需内核 mmap 介入，实现真正的应用间资源极速传递。
- 虚拟内存碎片管理——Bump 指针 + 侵入式空闲链表混合方案：Arena 内部的内存分配采用两阶段设计，兼顾按需 page fault 和高效碎片管理。
  - 阶段一（首次分配——Bump 指针）：Arena 数据区维护一个单调递增的 BumpOffset。首次分配时直接推进 Bump 指针，被分配的页面尚无物理页，通过实际访问时的 page fault 按需提交，全程零系统调用、零元数据写入。
  - 阶段二（释放后再分配——侵入式空闲链表）：被释放的块已拥有物理页，在块头部原地写入 prev/next 偏移和 size（侵入式，不额外占用 Header 空间），按 VA 地址顺序插入双向链表。分配时对链表做 best-fit 查找，支持尾部切割；释放时检查前后相邻块并合并，维持链表有序。
  - 分配优先级：先查 freelist（best-fit）→ 未命中则 bump 分配 → 均无可用则返回失败，上层回退 mmap。
  - 该方案的核心优势：（1）首次分配保留按需 page fault 特性，不因写元数据而提前触发物理页分配；（2）侵入式链表无固定容量上限，彻底消除外部元数据数组满后 VA 泄漏的问题；（3）VA 有序插入天然支持相邻块合并，减少碎片；（4）为 §2.3 Bitmap 按页追踪提供天然基础——VA 连续的空闲区间可直接映射到 Bitmap 的连续位段，更新高效。

2.3 安全与隔离机制 (Security & Isolation) 
内存委托必须确保空间隔离与原子性。我们在内核调度层引入协同：
- 内核独占归属表：分配器为每个 Arena 维护一份仅由内核修改的页面归属表 `page_slot[]`，以页为粒度记录当前页面属于哪个 owner slot；`owner_table` 再将 slot 映射到具体进程地址空间（`mm/tgid`）。其中空闲页用保留 slot 值表示。用户态不直接修改这份真值表，只能通过轻量化日志上报“分配/释放”意图。
- Per-arena 日志有序性：目标协议中，每个 Arena 由所有参与进程共用一个普通 FIFO ring，承载 ALLOC 和本核 FREE；另设一个归属于同一 owner Arena 的跨核 FREE ring。两者均不按进程划分，也不允许混入其他 Arena 的日志。普通 ring 内保持 FIFO；跨 ring 的先后关系通过先提交前置普通日志、再处理跨核 FREE，以及 FREE 成功回执后才允许复用来建立，详见 §2.10。
- Chunk 代际追踪：Arena 按固定大小分为多个 chunk（建议 64 页，即约 256KB），内核为每个 chunk 维护 `chunk_gen[u64]`。每次处理完一批用户态上报的分配/释放日志后，内核递增全局提交序号，并把受影响 chunk 的 `chunk_gen` 更新为最新代际号，用于标识该 chunk 自上次同步以来发生过映射变化。
- 上下文切换时的强制解绑：某个 CPU 发生调度切换时，只消费该 CPU 对应 Arena 的两个共享 ring，覆盖全部参与进程的记录，不局限于 prev_mm，也不遍历其他 Arena。不论 prev/next 是否参与委托，只要本 Arena 已注册且有待处理日志，就应有消费机会；不能沿用仅按 prev_active/next_active 跳过的判断而遗漏其他进程的 pending FREE。消费后更新本 Arena 的 page_slot[]/chunk_gen[]，再按既有安全上下文机制为 next 安排必要的页表同步。日志消费回执不等于所有旧 PTE/TLB 撤权完成，不因消费范围收窄而直接删掉仍有必要的页表同步。
- PTE Delta Run 快路径：当前实现保留 `chunk_gen[]` 作为正确性兜底，但在日志提交时同步记录 bounded delta run ring。`ALLOC` 成功后记录 `REVOKE_NON_OWNER(start,nr_pages,owner_slot,owner_gen)`，`FREE` 成功后只记录释放块首个 freelist 元数据页的 `PREFAULT_FREE`。`memory_delegation_sync_mm()` 在安全上下文中优先按 delta run 直接执行 unmap/prefault，避免对 dirty chunk 做逐页 snapshot；当 delta ring 被覆盖、ctx 过旧或遇到不完整 delta 时，自动回退到 `chunk_gen[]` 扫描慢路径，保证正确性不依赖 delta ring。

2.4 边界场景处理：跨核与线程迁移
- 并发访问：正常获取和描述符 metadata 修改由 owner CPU 上的用户态分配器完成。即使只有本核线程修改，也可能被抢占并由另一线程接续操作，因此本核正常分配/释放与回执回收仍用进程共享 Mutex；竞争时允许阻塞，不编写轮询自旋等待。远端线程只向跨核 FREE ring 原子发布完整请求，不修改 freelist、描述符状态或 generation，不争抢该 Mutex。内核真值 page_slot[]/chunk_gen[] 仍只由内核更新。
- 线程跨核迁移：线程从 Core 1 迁到 Core 2 后，释放 Core 1 的块时，在完成必要的原分配器释放清理后，只向 Core 1/Arena 的独立跨核 FREE ring 原子投递请求，不写 Core 2 的普通 ring，不修改委托后端描述符。描述符保留原有不可分配状态；待 Core 1 内核消费并反馈成功后，Core 1 上的用户态分配器择机拿本 Arena Mutex，核对记录、generation 与回执，再更新 metadata 并发布为空闲。

2.5 当前拍板的内核实现约束（已确认）
- owner slot 防复用：接受 generation 防 ABA 方案。slot 不仅是索引，还要带代际校验。
- switch-out 日志提交：不引入异步消费 worker。仅由 owner CPU 的调度路径消费本 Arena 的普通 ring 和跨核 FREE ring；均不是 per-(mm,cpu) ring。先 acquire 确定跨核 FREE ring 中连续完整发布的记录边界，再快照普通 ring 的已发布 tail；先消费普通快照范围，再消费跨核 FREE 快照范围。原子预订游标不是发布边界，遇到未完成槽位必须停止且不等待，详见 §2.10。
- switch-in 脏区选择：第一版线性扫描 `chunk_gen[]`，不引入额外 dirty 索引结构。
- 页表失效策略：第一版采用 `unmap`（清 PTE）路径，不采用 `PROT_NONE`。
- `mm_ctx` 存储位置：只能使用外部 `arena_mm_ctx` 私有表，不改 `mm_struct`/`task_struct` 布局。
- 跨核 free 延迟：允许等待 owner CPU 后续调度消费以及本核用户态分配器后续回收机会，不承诺固定墙钟时间。回执被本核回收前，描述符保持原有不可分配状态；pending 由日志和回执表达，不由远端线程写描述符 Pending。暂不实现通知阈值、IPI 或 worker；没有消费或回收机会时允许暂不复用该块。

2.6 破局方案：定制 `vm_ops->fault` + 尽可能无锁（Lockless）查表（安全红线）
目标：实现“硬件拦截 + 自动 SIGSEGV”，同时**绝对不修改** Linux 核心的 generic page fault 锁与匿名页缺页路径；把所有权校验与补页决策下沉到专用 VMA 的自定义 fault 慢路径中。

- 硬件拦截（真正的零开销隔离）
  - 在上下文切换等“安全时机”，针对“被借走/不再属于当前 mm”的页，直接清空对应 PTE（`pte_clear` / unmap PTE entry）。
  - 这一步**不需要**做 VMA 分裂/改写（避免极其昂贵的 VMA 操作与 `mmap_write_lock`），也不触碰 generic fault 的全局锁机制。
  - 只要 PTE 为空，任何越权访问必然被 MMU 打断并陷入内核，隔离成本为 0（在用户态热路径上）。

- 自定义 fault 路由（避开通用匿名页补页）
  - 将 Arena 的虚拟地址区间映射为“特殊 shared memory VMA”，并挂载自定义 `vm_operations_struct`（`arena_vma_ops`）。
  - 缺页异常发生时，内核识别该 VMA 后，不走默认匿名页缺页分配逻辑，而是直接调用 `arena_vma_ops->fault`（这是我们允许变慢的 Slow Path）。

- 极速无锁真值校验（核心设计）
  - 在 `arena_vma_ops->fault` 中**禁止**引入 `spinlock`/`mutex`：首次访问与非法访问的区分必须靠无锁读真值表完成。
  - 仍沿用 §2.3 的内核独占归属表 `page_slot[]`（页粒度 owner slot）。在 x86/ARM64 上，对 word 大小读取天然原子，可直接无锁读取 `page_slot[page_idx]`。
  - 判定逻辑（返回语义必须严格）：
    - 若 `page_slot[page_idx] == current_slot`：合法的首次访问（或合法的恢复访问）。分配/获取物理页，建立 PTE 映射，返回继续执行。
    - 若 `page_slot[page_idx] == MD_INVALID_SLOT`：空闲页，访问合法，page fault放行，因为分配器需要访问free page中的元数据。
    - 若 `page_slot[page_idx] != current_slot`：越权访问。**直接返回 `VM_FAULT_SIGSEGV`**，由内核向用户态递送 `SIGSEGV`。
  - 结论：PTE 清空只是“硬件触发器”；真正的安全性与“首次/非法”判定完全由 `arena_vma_ops->fault` 内的无锁 ownership 校验保证。

2.7 内核侧架构图（第一版，`vm_ops->fault` 路由）
```mermaid
flowchart LR
    A[Per-Arena shared normal and remote-FREE rings] --> B[owner CPU switch: drain snapshots]
    B --> C[Kernel Truth: page_slot + owner_table]
    B --> D[chunk_gen + global_commit_seq]
    B --> R[bounded PTE delta run ring]
    R --> S{delta retained?}
    S -- yes --> F[unmap/prefault by delta runs]
    S -- no --> E[switch-in next: scan chunk_gen fallback]
    D --> E
    E --> F
    F --> G[Access fault]
    G --> H[arena_vma_ops->fault]
    H --> I{page_slot[page_idx] == current_slot?}
    I -- yes --> J[alloc page + map PTE]
    I -- no --> K[VM_FAULT_SIGSEGV]
    E --> L[arena_mm_ctx.last_seen_gen update]
```

2.8 ⚠ 调度路径实现约束（需严格遵守）
- `context_switch()` 处于原子上下文，不能执行可能睡眠的 `mmap_write_lock` 与 `zap_page_range`。
- 因此第一版内核实现拆为两段：
  - 调度路径：仅更新 generation 和标记 `needs_pt_sync`。
  - 安全上下文：在持有 `mmap_write_lock(mm)` 时调用 `memory_delegation_sync_mm()` 执行实际 `unmap`。针对free的页面（注意是bump指针之前的部分）做prefault操作防止用户态分配器访问时触发fault
- 该约束不改变安全模型，但决定了“撤销映射”的实际触发点必须在可睡眠上下文中。
- Delta run 快路径同样遵守该约束：调度路径仍只负责 drain log、推进代际和 queue task_work；实际 PTE unmap/prefault 仍在 `memory_delegation_sync_mm()` 持有 `mmap_write_lock(mm)` 后执行。

2.9 memory delegation内核的性能优化
1. 减少锁使用
  - arena->lock：由于存在跨核线程迁移，可能同时有多个线程在修改同一arena中的kernel truth、另外修改PTE的时候也要读取
  - ctx->lock：last_seen_gen 改为 READ_ONCE() / WRITE_ONCE() 无锁访问; sync_queued 改为 READ_ONCE() / WRITE_ONCE() 无锁访问; ring_page：读取侧用 READ_ONCE(ctx->ring_page)发布侧用 cmpxchg(&ctx->ring_page, NULL, page)
  - md_arena_table_lock：只在arena元数据注册和注销时保护arena全局哈希表使用，不在热路径
  - md_mm_ctx_table_lock：
    - 仍用于保护 `md_mm_ctx_table` 的创建、查找加引用、删除以及 `ctx->dead` / `ctx->active` 状态转换，保证 per-(mm,cpu) 上下文生命周期不会在 lookup/get 路径中悬空。
    - 不再用于 `md_mm_has_active_ctx()` 的热路径查询。`md_active_mm_table` 作为“某个 mm 是否参与 delegation”的快速索引，读侧使用 RCU：`md_mm_has_active_ctx()` 通过 `rcu_read_lock()` + `hash_for_each_possible_rcu()` 查询，避免 context switch、fault、fork、mm_release 前置判断频繁竞争全局 spinlock。
    - `md_active_mm_table` 的写侧仍在 `md_mm_ctx_table_lock` 内串行化更新；新增记录用 `hash_add_rcu()` 发布，删除记录用 `hash_del_rcu()` 摘链，并通过 `kfree_rcu()` 延迟释放，保证 RCU 读侧看到旧节点时不会产生 use-after-free。
    - `md_active_mms` 继续作为全局空表 fast-path guard：为 0 时直接跳过 RCU 查表；非 0 时再按 mm 查询 active 记录。该计数只作为快速过滤，不单独表达某个 mm 的活跃状态。
    - 当前只对 active-mm 查询做 RCU 读侧优化，`md_mm_ctx_lookup_get()` 仍然使用 `md_mm_ctx_table_lock`。若后续将 `md_mm_ctx_table` 也 RCU 化，必须同时引入 RCU 延迟释放和 `refcount_inc_not_zero()`，不能简单把普通 `refcount_inc()` 移到锁外。
2. PTE Delta Run Ring 快路径
  - 目标：解决 `chunk_gen[]` 只能表达“chunk 变过”、无法表达“具体哪些页段需要 PTE 操作”的问题。旧路径在 PTE sync 时需要对 dirty chunk 逐页 snapshot，即使真实修改只是一个连续大块，也要扫描整个 chunk。
  - 元数据：每个 arena 维护固定容量 `md_pte_delta_run` ring，记录 `serial/seq/op/owner_slot/owner_gen/start_page/nr_pages`。ring 被覆盖时推进 `delta_loss_seq`，使落后的 `ctx->last_seen_gen` 自动回退 chunk 扫描。
  - 提交语义：同一批 drain 使用即将发布的 `global_commit_seq + 1` 作为 delta `seq`。`ALLOC` 记录 `REVOKE_NON_OWNER`，表示除新 owner 外的 mm 都应撤销这段旧 PTE；`FREE` 不记录 revoke，因为空闲页允许 allocator 访问，只记录首个 freelist header 页的 `PREFAULT_FREE`。
  - 同步语义：`memory_delegation_sync_mm()` 优先复制 `(last_seen, target_seq]` 内仍保留的 delta run，在释放 arena 锁后批量执行现有 `md_unmap_range_locked()` / `md_prefault_range_locked()`。`REVOKE_NON_OWNER` 在目标 mm 仍是该 run owner 时跳过；`PREFAULT_FREE` 执行前重新校验该页当前仍为 `MD_INVALID_SLOT && page_free`，避免 stale free delta 错误 prefault 已重新分配的页。
  - 正确性边界：delta ring 只是性能快路径，不是唯一真值。任何内存分配失败、delta 丢失、range 异常或未来出现无法完整生成 delta 的提交路径，都必须回退 `chunk_gen[]` 扫描慢路径。
  - 当前真机结果：在 4 子进程轮流持有 32 MiB arena 块的 revoke/free workload 中，`pte_sync avg` 从约 `8222 -> 402 cycles`（约 95.1% 优化），`dirty_chunks=0`、`snapshot_loop=0`、`delta_fallbacks=0`。


2.10 统一后端异步交接协议修订（2026-10-22，已确认设计，待实现）

本节及上文关联修订取代 2026-10-21 的 external 异步补充，并按 2026-10-22 后续讨论改为“跨核原子投递、本核加锁回收”。它们规定目标行为，不表示代码已完成或测试已通过。现有远端代码仍有 per-mm ring、每次追加的 ProducerLock、遍历同一 mm 所有 external Arena 的消费路径和 ctx drain_lock；需要按本节调整，不能把现有行为当作目标协议。

1. 本核 metadata 仍用 Mutex，跨核 FREE 只原子投递
   - 每个 Arena 的正常分配、本核释放、描述符/freelist 更新以及成功回执回收，由 owner CPU 上的用户态分配器执行，使用跨参与进程共享、配置为 PTHREAD_PROCESS_SHARED 的 Mutex。它防止同核抢占后另一个线程读到半更新结构，不因“只有本核修改”而删除；竞争时允许阻塞，不编写轮询自旋锁。
   - 普通路径在同一临界区内校验租约和 generation、准备相关 metadata、写完整普通日志，再 release 更新普通 ring 的 tail，最后完成相应状态发布并解锁。不另叠加只保护 append 的 ProducerLock。内核不拿用户态 Mutex，故内核需要的准备必须在发布之前完成。
   - 跨核释放者只完成必要的原分配器释放清理，再使用已有租约中的块标识、范围、generation 和进程/会话身份构造 FREE 请求，原子投递到 owner Arena 的跨核 ring。不拿 Arena metadata Mutex，不改描述符状态、generation、freelist、空闲索引或回收游标，也不在发布后继续访问 payload。
   - 远端请求等待期间，描述符维持原有不可分配状态（如 InUse），不由远端写成 Pending。日志中存在待确认 FREE 并不表示调用者还可使用已释放对象，也不表示该块已经可重新分配；回执回收前禁止获取、split、merge。

2. 每 Arena 两个共享 FIFO 与原子发布
   - 普通 ring：同一 Arena 的所有参与进程共用，记录 ALLOC 和本核 FREE；追加由本核 Arena Mutex 串行化，按 FIFO 消费。
   - 跨核 FREE ring：同一 owner Arena 的所有参与进程共用，是多生产者、单内核消费者的原子队列。远端生产者原子预订唯一槽位，填完记录，再 release 发布带环绕序号的每槽位 ready/sequence；具体布局及原子算法待实现时落实。预订计数不表示记录已写完，不能仅 fetch_add 公开 tail 后让内核直接消费。
   - 内核 acquire 检查槽位，只消费连续完整发布的前缀，不能跳过未完成槽位。遇到生产者被抢占留下的空洞，立即停止本轮该 ring 的消费，不自旋等待；生产者恢复并完成发布后再处理。本方案不承诺某个生产者暂停时后面的记录仍能推进。
   - 记录必须携带可由内核验证的操作进程/会话身份、块范围和所需 generation，内核据此定位实际 mm。不得使用触发消费的 prev_mm 作为 owner，也不得信任用户态裸 mm 指针或仅凭可复用 PID 认定身份。身份令牌、校验与 mm 生命周期引用的具体 ABI 待实现时落实；注册须协商新能力，不能仅凭旧 ASYNC_EXTERNAL 标志认定兼容。
   - 一条跨核 FREE 完整发布前，其前置 ALLOC 必须已发布到普通 ring。内核先 acquire 确定跨核 FREE 的连续 ready 前缀边界，再 acquire 快照普通 tail；先消费普通快照，再消费已确定的跨核 FREE 快照。不把快照后才完成发布的 FREE 临时加入本轮，避免其前置 ALLOC 落在普通快照之外。

3. 内核回执后，本核用户态择机完成释放
   - 内核校验跨核 FREE 的身份、ownership 和 generation，成功更新 ownership 为空闲后，写入可关联到请求 ticket/槽位序号的结果，再 release 发布消费完成边界。内核不直接修改后端描述符。
   - owner CPU 上的用户态分配器可在后续分配、释放或批量维护时检查回执，无需释放者回来。它在本 Arena Mutex 下 acquire 读取完成边界、确认对应请求成功，校验当前描述符的块范围和 generation，再更新 metadata、推进 generation 并把块插入空闲结构；维护独立的用户态回收进度，避免重复入池。
   - 内核消费进度与本核用户态回执回收进度不同。共享日志/结果存储必须保留到本核完成回收；仅 kernel head 前进不能允许生产者覆盖尚未被用户态读取的记录或结果。槽位复用须同时满足内核已消费、本核已回收及环绕代际校验，不能只看消费 head。
   - 完成边界越过 ticket 仅证明已消费，不自动证明成功。被拒绝、重复或过期的 FREE 不得进入空闲索引；保留错误结果并隔离或按内核真值恢复。若采用 sticky last_error，不能清除错误后把此前被拒绝的 FREE 当成功处理。具体错误回执 ABI 待实现时落实。
   - 普通 ALLOC 不等待回执：现有 fault 与可访问性判断允许 MD_INVALID_SLOT 空闲页被所有参与 mm 访问；从内核已空闲的块，或仍归当前 mm 的块获取时，可以在追加 ALLOC 后直接使用。用户态 Mutex 保证合法获取者互斥，内核拒绝错误仍须反馈处理。
   - 普通本核 FREE 沿用调度边界保证：其他 mm 获取该块前，owner CPU 必须已消费前面的普通日志；其他 CPU 不能直接获取尚未提交普通 FREE 的候选。正常获取、metadata 修改与回执回收均须满足本核执行约束；阻塞 Mutex 可能导致迁核，必须校验 CPU/Arena 归属并正确重试，不能让旧 CPU 选择绕过此约束。
   - 回执仅表示 ownership 日志处理结果，不等于所有旧 PTE/TLB 已撤权。既有 PTE/task_work/TLB 同步与数据隔离责任继续保留，不据此宣称已验证所有执行上下文隔离。

4. owner CPU 单消费者、快照与生命周期
   - 仅 owner CPU 的调度路径消费本 Arena 两个 ring，不按 prev_mm 筛选，不遍历其他 Arena。不要求写入者在 owner CPU 上再次运行；回执后的 metadata 回收由本核用户态完成。
   - 去掉显式 DRAIN_LOG，包括 overflow 和 close 消费路径；关闭不得在非 owner CPU drain，也不得销毁未消费/未回收请求所需的存储。所有消费入口统一且确认不可重入后删除 ctx drain_lock，不在调度原子上下文换用会睡眠的 Mutex。
   - 普通 ring 使用 acquire tail 快照；跨核 ring 使用 acquire 确定的连续 ready 前缀快照。两者快照及消费次序遵守第 2 点，遇到未完成槽位不等待。处理后 release 发布内核完成边界；后续本核回收的发布与槽位复用遵守第 3 点。
   - ring 和回执位于 payload 外的 Arena 共享日志存储，由 Arena/broker 生命周期持有。单个进程退出不销毁它们；Arena 注销须停止新投递、妥善处理未完成预订与回收，并等待在途消费退出后再解除 pin/释放 backing。记录的身份和退出清理同样需要有效期保证。生命周期保护不等于恢复每次 drain 的消费锁。

阶段假设与验证边界：
- 当前先假设 ring 不会满，容量足够覆盖待消费以及内核已消费但用户态尚未回收的记录；暂不设计 overflow drain、扩容或阻塞等待空间。未完成预订、未消费或未回收的槽位都不得覆盖；槽位序号、容量和回执保留边界须在实现时落实，此假设不代表允许静默丢日志。
- 允许跨核 FREE 等到 owner CPU 后续调度时消费，再等本核用户态分配器后续回收时才真正发布空闲块；暂不为无调度或无用户态回收机会提供即时推进保证。CPU 热插拔/下线后的消费权转移未在本次约定中解决，后续实现不得让两个 CPU 同时成为消费者。
- 需要验证：多个远端 CPU 并发投递唯一槽位；生产者预订后被抢占时内核不读取半条记录、不等待；两个快照之间并发发布不反序；原释放进程不回 owner CPU 仍可处理；本核回收前块不可复用；同核抢占下 Mutex 正确；迁核后不误改非本核 metadata；重复/过期/拒绝 FREE 不重复入池；环绕时回执不被提前覆盖；进程退出不销毁共享日志。生产者预订后异常退出造成的空洞恢复、共享 Mutex 持有者异常退出恢复策略尚待落实，不能把异常退出当作普通抢占处理。
