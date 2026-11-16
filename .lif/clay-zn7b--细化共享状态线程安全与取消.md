---
# clay-zn7b
title: 细化：共享状态线程安全与取消
status: in-progress
type: task
priority: normal
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-16T02:14:14Z
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
