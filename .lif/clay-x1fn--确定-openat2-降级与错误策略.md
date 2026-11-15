---
# clay-x1fn
title: 确定 openat2 降级与错误策略
status: completed
type: task
priority: normal
created_at: 2026-11-15T01:38:37Z
updated_at: 2026-11-15T01:46:18Z
parent: clay-1m1e
blocked_by:
    - clay-h7s8
---

针对 src/commands/workspace_fd.c、include/clay/workspace_fd.h 和 tests/test_workspace_fd.c，确定不可用或 seccomp 限制时的降级 errno、失败关闭与逐组件 openat 的目录移出限制。验收：覆盖不可用、受限、安全拒绝和普通 I/O 错误，安全拒绝不静默降级。

## Summary of Changes

openat2 仅在 ENOSYS、EINVAL、E2BIG 回退；EPERM、EACCES、ELOOP、EXDEV、EAGAIN 及其他错误原样失败，避免将安全拒绝误判为 seccomp 限制。头文件说明 EPERM seccomp 不兼容及逐组件遍历的目录移出限制。seccomp 注入测试覆盖降级、拒绝和 I/O 错误，验证回退拒绝符号链接；make test-workspace-fd、禁用 openat2 编译运行、git diff --check 与 lif check 通过。seccomp 可伪造任意 errno；兼容回退不保证目录始终在根内。
