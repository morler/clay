---
# clay-s4rb
title: 二阶段：进程内共享配额作用域和生命周期
status: todo
type: task
created_at: 2026-11-15T03:51:47Z
updated_at: 2026-11-15T03:51:47Z
parent: clay-c8lo
blocked_by:
    - clay-7609
---

范围：src/commands/context.c、src/commands/context.h、src/commands/subagent.c、src/commands/message.c、provider 入口。核实 clay_commands_run_completion 调用链；设计进程内线程安全共享状态，主会话和并发子代理共用；按 provider、凭据身份/端点及模型隔离，敏感密钥不明文记录。可信信号表明更宽范围时保守合并，未知不视作无限额度。共享对象寿命覆盖 worker，不持锁执行网络 I/O 或工具；模型切换、取消与释放不串扰。验收：多线程 mock 验证同 scope 共享、不同 scope 隔离、无死锁悬挂；Codex、Grok 认证和公开接口兼容。
