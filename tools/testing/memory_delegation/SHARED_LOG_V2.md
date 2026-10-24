**历史 ABI v2 文档：普通回收规则已由 [SHARED_LOG_V3.md](SHARED_LOG_V3.md) 覆盖，当前同名测试文件实际测试 v3。**

# 每 Arena 双共享日志 ABI v2 实施记录

依据：内核根目录 `DESIGN.md` §2.3–2.10（2026-10-22 修订）。本记录说明实现与验证范围，不替代或修改最终设计文档。

## 接口与兼容性

注册调用：

```c
struct md_shared_registration req = {
    .size = sizeof(req),
    .version = MD_SHARED_LOG_VERSION,
    .log_address = (uintptr_t)shared_log,
    .log_bytes = log_bytes,
};
prctl(PR_SET_MEMORY_DELEGATION_LOG, &req,
      PR_MEMORY_DELEGATION_F_EXTERNAL_BACKEND |
      PR_MEMORY_DELEGATION_F_SHARED_RINGS,
      owner_cpu, payload_base);
/* 成功后 req.session 是当前 mm、CPU、Arena 实例绑定的身份标识。 */
```

每次调用前令 `req.session = 0`。同一 mm 重复注册相同 Arena 返回相同 session；fork 子进程必须重新注册，不能复用父 session。旧 per-mm ring、旧 ASYNC_EXTERNAL 注册均返回 `EPROTONOSUPPORT`；显式 DRAIN 返回 `EOPNOTSUPP`。原 ABI 结构体及内核 submit 符号保留便于编译/符号兼容，不保留旧消费行为。

broker 初始化一个独立 shmem 日志 backing：`md_shared_log` 头后依次为 `capacity` 个普通槽位和 `capacity` 个跨核 FREE 槽位。日志总大小按页向上取整，最多 `MD_SHARED_LOG_MAX_PAGES` 页。日志与 payload 必须使用不同 inode，均为 shared shmem 映射。所有参与者必须使用相同 payload VA、inode/offset、日志物理页面和布局。内核保存已验证的容量，不按运行期间可改写的用户态容量索引。

`MD_SHARED_LOG_F_INTRUSIVE` 表示空闲块首个 payload 页有 freelist 元数据，保留 FREE 的首页面 prefault；默认外部描述符模式不需该 prefault。两种模式保留相同 ownership、chunk/delta 和安全上下文 PTE 同步。

## 记录身份与代际

每条记录包含：`session / block_id / generation / allocation_id / start_page / nr_pages / cpu / op / flags`。

- session 由内核分配，不是 PID、mm 指针或 owner slot。内核按 session 表定位实际 mm；退出先使 session 失效，再隔离遗留 ownership。会话与 Arena 实例不匹配时拒绝，owner-slot generation 机制继续独立保留。
- block_id 和 generation 对应用户态描述符及其代际；allocation_id 是额外的 Arena 内分配实例序号，由本 Arena Mutex 下的普通 ALLOC 生产者单调递增，不能跨分配复用。它与描述符 generation、owner-slot generation 各有用途，不能混用。序号耗尽返回错误，不允许环绕。
- FREE 必须与整段现有分配的实例、block、generation、范围和 owner mm 全部一致；拒绝部分释放、重复释放、迟到释放、其他 Arena 记录和已退出会话。split/merge 只应在空闲结构内发生，重新 ALLOC 使用新 allocation_id。

session 用于生命周期关联与防复用，**不是发布者认证凭据**。共享 ring 的可写参与者可观察并伪造其他会话的记录；当前协议只有在分配器参与者遵守协议时才成立。若最终要求抵抗恶意发布者，需要先修改/补充可认证发布通道的设计，不能把本实现称为完成了该安全目标。

## 发布、消费与回收

普通路径在 owner CPU 的进程共享 Arena Mutex 下准备完整记录，写 `ready=ticket+1`，再 release 发布 `normal.reserved`（普通队列的已发布 tail）。不添加 append ProducerLock。

跨核生产者通过 CAS 预订唯一 ticket，只写日志；填完记录后 release 写每槽 `ready=ticket+1`。`remote.reserved` 仅是预订游标，不是发布边界。预订时 acquire 读取 reclaimed，容量不足返回 `ENOSPC`，不覆盖、不临时 drain、不等待空间。CAS 重试仅仲裁预订，不构成持锁轮询。

仅 owner CPU 调度钩子消费：

1. acquire 检查 remote ready，确定连续完整前缀，遇空洞立即结束该前缀。
2. 再 acquire 快照普通已发布 tail。
3. 在 Arena truth 锁下先消费普通快照，再消费已经确定的 remote 快照。不临时扩大 remote 快照。
4. 逐槽写 `result=0` 或负 errno；提交 truth/chunk/delta 后 release 发布 completed。

completed 只能证明已经处理，不能代替 result。owner CPU 用户态在 Arena Mutex 下 acquire completed，逐条匹配 ticket、范围和 generation；只有成功 FREE 才可更新描述符、推进 generation、加入空闲结构。处理/保留错误后，release 推进独立 reclaimed。生产者不得只按 completed 复用槽位。完整记录与结果保留到 reclaimed。参考 producer 在 `tests/shared_log_v2.h`，不包含分配器描述符回收实现。

调度钩子消费不再依赖 prev/next mm 是否活跃；只消费本 CPU Arena。其他 Arena 的 next-mm PTE 同步排队保留，它不消费其他 Arena 日志。不修改 generic fault，不在 scheduler 中执行 mmap 锁/PTE unmap，不删除 chunk fallback。

## 生命周期与已确认边界

- 日志 pin/vmap 归 Arena 持有，不归单个 ctx；进程退出不 drain、不 unpin，也不改用户态描述符。遗留已归属页面进入隔离状态，不自动发布为空闲。
- exported Arena unregister 仅在 broker 已停止全部生产者并发布 STOPPED、没有活跃会话、没有未完成预订/未消费/未回收记录，且没有遗留非空闲 ownership 时解除 RCU 发布；等待读者退出后才 unmap/unpin。条件不满足则保留存储并记录 busy。当前没有新增用户态强制 reset/unregister 入口。
- **用户已确认（2026-10-22）**：本次采用保守处理，不实现预订后异常退出空洞或共享 Mutex 持有者死亡的自动恢复。保留记录与隔离状态，不跳过空洞或凭空完成请求；尚无自动诊断/恢复服务。有空洞/隔离页时仍拒绝常规 unregister，停止全部参与者后的重建恢复仍需后续维护机制，当前没有一键恢复接口。
- **用户已确认（2026-10-22）**：恶意发布者伪造日志等安全边界问题留待后续统一处理，本次不扩展发布者认证机制。这是暂缓处理，不是安全性已经成立的结论；现有 ownership、代际校验和 PTE 同步仍保留。
- CPU 下线的消费权转移不实现；不引入 IPI、worker、overflow drain。所有生产者/回收者应遵守 owner CPU 约束。Mutex 获取后的 CPU 校验不能被写成整个临界区不会再迁核的保证；实际分配器接入还必须落实这一约束，本测试用线程 affinity 保证。
- 回执不等于所有旧 PTE/TLB 完成撤权。现有 task_work/PTE 同步和数据清理责任保留；不能从本套测试推导所有并发执行上下文的隔离已被证明。

## 可复现验证

构建内核（输出到独立目录，避免覆盖原设备产物）：

```bash
cd /home/lrc/patent/kernel/kernel_platform
tools/bazel run //common:kernel_aarch64_dist -- \
  --destdir=/home/lrc/patent/md-v2-work/dist
```

测试程序独立于旧 Scudo producer：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common
make -C tools/testing/memory_delegation/tests MODE=linux shared_log_v2_test
KERNEL_IMAGE=/home/lrc/patent/md-v2-work/dist/Image \
WORKDIR=/home/lrc/patent/md-v2-work/qemu-final \
  tools/testing/memory_delegation/scripts/run_shared_log_v2_qemu.sh
```

测试涉及：能力协商、重复注册、错误共享 backing、owner/nonowner fault、被暂停预订与连续前缀、ALLOC/FREE 次序、重复/过期/部分/跨 Arena 请求、逐条错误回执、普通/远端回执保留、环绕、多个远端 CPU 的唯一预订、进程退出与会话失效、跨进程同 VA 块复用，以及 owner CPU 上运行非参与 mm 时仍能消费。预期越权 SIGSEGV 仅发生在隔离子进程。

边界：不是 Scudo/jemalloc 完整接入测试，不测试 allocator 描述符回收、split/merge、calloc/realloc、Mutex 异常恢复、CPU 热插拔、broker 重启恢复、Arena unregister 的内核调用和完整 PTE/TLB 并发撤权。没有运行手机刷机、重启或替换分配器，也没有性能收益结论。旧 Scudo runner 尚使用旧 ABI，不能拿它的失败或回退当作 v2 全链路验收。
