---
id: 28
title: 确定 openat2 降级与错误策略
status: closed
priority: medium
labels:
    - source:lif
    - type:task
    - parent:15
relations:
    blocks:
        - 27
    depends-on:
        - 18
    related-to:
        - 15
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

针对 src/commands/workspace_fd.c、include/clay/workspace_fd.h 和 tests/test_workspace_fd.c，确定不可用或 seccomp 限制时的降级 errno、失败关闭与逐组件 openat 的目录移出限制。验收：覆盖不可用、受限、安全拒绝和普通 I/O 错误，安全拒绝不静默降级。

## Summary of Changes

openat2 仅在 ENOSYS、EINVAL、E2BIG 回退；EPERM、EACCES、ELOOP、EXDEV、EAGAIN 及其他错误原样失败，避免将安全拒绝误判为 seccomp 限制。头文件说明 EPERM seccomp 不兼容及逐组件遍历的目录移出限制。seccomp 注入测试覆盖降级、拒绝和 I/O 错误，验证回退拒绝符号链接；make test-workspace-fd、禁用 openat2 编译运行、git diff --check 与 lif check 通过。seccomp 可伪造任意 errno；兼容回退不保证目录始终在根内。

---

## 迁移来源（lif）

- 原 ID: `clay-x1fn`
- 原类型: `task`
- 原状态: `completed`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-1m1e` → #15
- 原阻塞于: `clay-h7s8` → #18
- 原阻塞: 无
- 原创建时间: `2026-11-15T01:38:37Z`
- 原更新时间: `2026-11-15T01:46:18Z`
- 原文件路径: `.lif/clay-x1fn--确定-openat2-降级与错误策略.md`
