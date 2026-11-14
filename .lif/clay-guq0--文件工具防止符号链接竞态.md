---
# clay-guq0
title: 文件工具防止符号链接竞态
status: todo
type: bug
created_at: 2026-11-14T02:44:09Z
updated_at: 2026-11-14T02:44:09Z
---

逐组件 lstat 与路径操作存在 TOCTOU；需设计并实现基于目录句柄的读写、遍历、grep 和 undo 安全链路，并覆盖并发测试。
