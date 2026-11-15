---
# clay-zn7b
title: 细化：共享状态线程安全与取消
status: todo
type: task
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-15T03:55:04Z
parent: clay-c8lo
blocking:
    - clay-s4rb
blocked_by:
    - clay-7609
---

范围：src/commands/context.c、src/commands/subagent.c、tests/test_subagent.c 和共享状态实现。保护主会话与 worker 共享状态和生命周期，不持锁执行网络 I/O 或工具；验收：多线程 mock 验证同异 scope 并发、取消退出、无死锁悬挂和泄漏。
