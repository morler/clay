---
# clay-h7s8
title: 定义 Linux 竞态威胁模型与安全契约
status: completed
type: task
priority: normal
created_at: 2026-11-15T01:38:37Z
updated_at: 2026-11-15T01:44:27Z
parent: clay-1m1e
blocked_by:
    - clay-w8rj
---

审查 src/commands/workspace_fd.c、include/clay/workspace_fd.h、src/commands/fs_tools.c、src/commands/repo_map.c、src/commands/undo.c。验收：明确根句柄固定时点、目录移出、硬链接、乐观 undo 冲突及遗留窗口和各模式边界；不承诺抵御恶意本地进程或零竞态。

## Summary of Changes

审查 workspace_fd、fs_tools、repo_map、undo 和 README；README 记录 Linux 根句柄固定时点、openat2/回退、目录移出、硬链接、写入与乐观 undo 残留竞态及 Sandbox/Unleashed、Ask/Auto、Plan 和非 Linux 边界，明确非目标；修正 Linux repo_map ctags 描述。git diff --check 与 lif check 通过；仅改文档，未重跑源码测试。
