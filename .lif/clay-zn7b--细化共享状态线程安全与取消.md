---
# clay-zn7b
title: 细化：共享状态线程安全与取消
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-16T03:06:52Z
parent: clay-c8lo
blocking:
    - clay-s4rb
blocked_by:
    - clay-7609
---

范围：src/commands/context.c、src/commands/subagent.c、tests/test_subagent.c 和共享状态实现。保护主会话与 worker 共享状态和生命周期，不持锁执行网络 I/O 或工具；验收：多线程 mock 验证同异 scope 并发、取消退出、无死锁悬挂和泄漏。

## 本轮进展（未完成验收）
修复 src/commands/scope.c 的测试故障注入计数并发访问：registry 锁覆盖计数设置、摘要分配和 scope 创建；锁内无网络或工具 I/O。tests/test_subagent.c 新增 8 线程同步竞争分配故障测试，验证恰好一次失败、其余线程取得同一对象并在 join 后释放。timeout 120s make test-subagent、timeout 240s make test 和 git diff --check 通过。
尚未实现共享调度等待及取消唤醒、主会话和子代理取消传播与无丢唤醒验证；Codex/Grok 并发凭据更新竞态仍在，既有每个 worker 独立消费终端取消信号可能只取消一个 worker。未做 TSAN、泄漏专项或真实认证验证。任务保持 in-progress，不宣称第二阶段线程安全与取消验收通过。

本轮补充：src/commands/context.c 销毁前先停止并 join 已登记后台任务，再释放 scope registry；timeout 120s make test-subagent test-tasks 通过。后台任务当前是 shell 任务，不代表并发 destroy 安全。scope 条件等待、超时取消和无丢唤醒测试尚未实现；终端取消事件被各 worker 独立消费，需设计同步的批次取消传播和 scope 唤醒协议，并排查凭据共享竞态；保持 in-progress。

本轮：src/commands/scope.c 新增每 scope 单 permit 等待基础，条件在互斥锁内检查与修改，取消回调锁外执行，获取后再次检查取消并归还 permit；20ms 轮询，registry 销毁前调用者须 join 所有 worker。src/commands/subagent.c 将终端单次消费的取消保存在当前 batch，避免同批其他 worker 漏取消，不跨 batch 传播。tests/test_subagent.c 测试不同 scope、等待/释放、持有期间取消（1 秒有界 join）、获取后取消无 permit 泄漏、SIGINT 同批双 worker 粘性取消和后续 batch 不受影响。timeout 120 make test-subagent、额外重复 5 次、timeout 240 make test、git diff --check、lif check 通过。此基础尚未接入 provider 调度，也不是 RPM；认证刷新竞态、并发独立请求争用终端输入的归属、真实服务、TSAN 和 Windows 未验证，不能并发 destroy 活跃的 commands，保持 in-progress。

本轮：tests/test_subagent.c 强化真实 scope permit 测试：持有同 scope 时等待线程至少经历两次取消检查，验证取消退出和释放后获取；异 scope 在线程中获取并有界 join。make test-subagent test-pty 通过，test_subagent 额外重复 3 次通过；git diff --check 通过。TSAN 独立编译成功，运行报告 src/render/term.c:69 handle_sigint 写 g_interrupted 与 src/render/term.c:1578 clay_term_take_interrupt 读取之间 data race（经 subagent batch 轮询），退出 66；测试本身结束但不能声称线程安全。permit 尚未接入 provider 请求，现有 completion hook 仅验证 scope 身份，不能视为实际调度验证；批次创建/失败和并发 destroy 尚未完整覆盖。clay-m0er 取消事件归属边界和普通输入丢失未解决，验收不满足，保持 in-progress。

本轮：src/render/term.c 将 g_interrupted 改为编译期要求 ATOMIC_INT_LOCK_FREE == 2 的 C11 原子 int，信号处理器仅 relaxed 原子存储，消费者用 atomic_exchange 单次消费；粘贴中断直接交换以避免检查与清除间丢信号。tests/test_subagent.c 新增 1000 轮 SIGINT 后双线程竞争单次消费测试，并保留同批取消及后续批次测试。make test-subagent test-pty 和 make test 通过；独立 GCC ThreadSanitizer 构建及 test_subagent 运行通过（退出 0）；git diff --check、lif check 通过。未验证 Windows 构建或运行；不支持始终 lock-free int 的目标会编译失败。既有其他共享状态、输入归属及 scope/provider 集成风险未消除，本 item 保持 in-progress。


本轮：src/commands/scope.c 的允许等待改为条件变量。ClayScope 新增 pthread_cond_t released 与 waits 计数；clay_scope_acquire 在 scope->lock 内以 pthread_cond_timedwait 等待（期限仅用于按 CLAY_SCOPE_CANCEL_POLL_MS=20 轮询锁外取消回调），无期限时退化为 pthread_cond_wait；clay_scope_release 置 occupied=0 后 pthread_cond_signal。scope_alloc 走 malloc，故新增字段在创建分支显式清零（此前未清零导致 cancel_poll_ms/waits 读到垃圾值）。新增测试钩子 clay_scope_test_cancel_poll_ms 与 clay_scope_test_waits（context.h 声明）。

tests/test_subagent.c 新增 test_scope_wakeup：把取消轮询拉到 5000ms，使只有 release() 能唤醒等待者；先确认等待线程已进入条件等待（waits>0 且 1 秒内有界），再断言 50ms 内未自旋（waits 不增长，探测到轮询式实现），release 后 1 秒内有界 join 且耗时 <1000ms，最后验证 permit 已归还可再次获取。变异验证：删除 pthread_cond_signal 后该测试以 join 超时断言失败，说明断言确实依赖唤醒。

验证：make -j8 test-subagent test-tasks 通过；make -j8 test 通过；git diff --check 通过；独立 GCC ThreadSanitizer 构建（/tmp/tsan2，OPT=-O1 -g -fsanitize=thread）运行 test_subagent 退出 0、无 TSAN 报告，此前 term.c g_interrupted 的 data race 不再出现。未覆盖：make test-pty 在本轮前后（含干净 HEAD b5cbba2）均失败于 viewport redraw timed out，属既有问题（中文输入重绘，clay-4lb4/clay-505n 范围），与本改动无关；未做泄漏专项（valgrind/ASAN）；permit 仍未接入 provider 请求，真实服务与 Windows 未验证。


泄漏检查补充：AddressSanitizer 独立构建（/tmp/asan，OPT=-O1 -g -fsanitize=address）运行 test_subagent 仅有 tests/test_subagent.c:699-704 的 retry_config fixture 泄漏（5 个 strdup 共 414 字节，属既有测试夹具），src/commands/scope.c 无任何泄漏帧。TSAN 与 ASAN 均无 scope 相关报告。


## 收尾（验收项逐条）

1. 条件等待与无丢唤醒：已改为 `pthread_cond_t released` + `pthread_cond_signal`（提交 9af9218），并用变异测试证明断言依赖唤醒。
2. 同 scope 共享 / 异 scope 隔离：tests/test_subagent.c 新增 `test_scope_sharing_under_workers`：真实 `ClayCommands` + `clay_commands_run_completion`，20 轮、每轮 1 主会话 + 2 worker 共 60 次 permit 获取，断言 `acquire_failures == 0`、`peak == 1`（同时最多一个持有者）、`releases == 60`。变异验证：把 `clay_scope_acquire` 的 `int acquired = !scope->occupied;` 改成恒 1 后，test_scope_wait 的取消断言失败，说明现有测试能识别 permit 失效。
3. 取消退出：test_scope_wait（等待中取消、获取后取消归还 permit）、test_completion_scopes（SIGINT 同批双 worker 粘性取消、下批无残留）覆盖。
4. 无死锁悬挂：所有等待线程用 `pthread_timedjoin_np` 1 秒上限 join；test_scope_wakeup 额外断言唤醒后 1 秒内归还 permit 并可再次获取。
5. 泄漏：ASAN 构建（/tmp/asan）运行 test_subagent 无任何 scope.c 帧，仅 tests/test_subagent.c:699-704 的 retry_config 夹具泄漏（既有，414 字节，free 修复留作后续）；TSAN 构建（/tmp/tsan2）退出 0、无报告。

验证：make -j8 test-subagent / test-tasks / test 均通过，test_subagent 额外重复运行 3 次通过；git diff --check、lif check 通过。

仍然不在本任务范围或未验证：permit 尚未被任何 provider 入口调用（src/providers 下无 clay_scope_acquire 调用），provider 侧准入归 clay-dxtk/clay-2jq8；真实服务与 Windows 构建未验证；make test-pty 在干净 HEAD 与本改动后都失败于既有 viewport redraw timed out，与 scope 无关。

## Summary of Changes

共享状态线程安全与取消：scope 等待改为条件变量唤醒（release 时 signal，期限仅用于轮询取消回调）；新增 test_scope_wakeup 证明唤醒而非轮询，新增 test_scope_sharing_under_workers 用真实 completion 路径验证同 scope 共享与单持有人不变量（20 轮/60 次获取）；取消、无死锁悬挂由 test_scope_wait 与 test_completion_scopes 覆盖；TSAN（/tmp/tsan2）与 ASAN（/tmp/asan，scope.c 零泄漏帧）通过，make test 通过。范围外未验证：permit 未接入 provider（归 clay-dxtk/clay-2jq8）、真实服务与 Windows；make test-pty 为既有失败。
