---
# clay-guq0
title: 文件工具防止符号链接竞态
status: in-progress
type: bug
priority: normal
created_at: 2026-11-14T02:44:09Z
updated_at: 2026-11-15T01:38:37Z
parent: clay-1m1e
blocked_by:
    - clay-uska
---

逐组件 lstat 与路径操作存在 TOCTOU；需设计并实现基于目录句柄的读写、遍历、grep 和 undo 安全链路，并覆盖并发测试。

Linux read 已改为 openat2/逐组件 openat 并拒绝特殊文件；测试通过。write/edit/glob/grep/undo 仍需目录句柄改造；兼容回退的已打开目录可被移出工作区。

Linux workspace_fd 共享读取（edit/undo 快照及 repo-map）优先使用 openat2 一次性解析并拒绝符号链接；旧内核或被限制时仍回退逐组件 openat。已验证 openat2、编译期回退和目录/符号链接交换并发测试。写入、undo 删除、grep 等仍存在竞态；已打开文件或目录之后被移出工作区不受此改进约束。
