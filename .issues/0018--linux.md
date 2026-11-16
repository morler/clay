---
id: 18
title: 定义 Linux 竞态威胁模型与安全契约
status: closed
priority: medium
labels:
    - source:lif
    - type:task
    - parent:15
relations:
    blocks:
        - 28
    depends-on:
        - 26
    related-to:
        - 15
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

审查 src/commands/workspace_fd.c、include/clay/workspace_fd.h、src/commands/fs_tools.c、src/commands/repo_map.c、src/commands/undo.c。验收：明确根句柄固定时点、目录移出、硬链接、乐观 undo 冲突及遗留窗口和各模式边界；不承诺抵御恶意本地进程或零竞态。

## Summary of Changes

审查 workspace_fd、fs_tools、repo_map、undo 和 README；README 记录 Linux 根句柄固定时点、openat2/回退、目录移出、硬链接、写入与乐观 undo 残留竞态及 Sandbox/Unleashed、Ask/Auto、Plan 和非 Linux 边界，明确非目标；修正 Linux repo_map ctags 描述。git diff --check 与 lif check 通过；仅改文档，未重跑源码测试。

---

## 迁移来源（lif）

- 原 ID: `clay-h7s8`
- 原类型: `task`
- 原状态: `completed`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-1m1e` → #15
- 原阻塞于: `clay-w8rj` → #26
- 原阻塞: 无
- 原创建时间: `2026-11-15T01:38:37Z`
- 原更新时间: `2026-11-15T01:44:27Z`
- 原文件路径: `.lif/clay-h7s8--定义-linux-竞态威胁模型与安全契约.md`
