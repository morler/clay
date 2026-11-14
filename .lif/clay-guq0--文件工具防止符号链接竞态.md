---
# clay-guq0
title: 文件工具防止符号链接竞态
status: in-progress
type: bug
priority: normal
created_at: 2026-11-14T02:44:09Z
updated_at: 2026-11-14T03:00:07Z
---

逐组件 lstat 与路径操作存在 TOCTOU；需设计并实现基于目录句柄的读写、遍历、grep 和 undo 安全链路，并覆盖并发测试。

Linux read 已改为 openat2/逐组件 openat 并拒绝特殊文件；测试通过。write/edit/glob/grep/undo 仍需目录句柄改造；兼容回退的已打开目录可被移出工作区。
