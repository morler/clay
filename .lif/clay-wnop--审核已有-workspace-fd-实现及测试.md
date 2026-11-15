---
# clay-wnop
title: 审核已有 workspace_fd 实现及测试
status: completed
type: task
priority: normal
created_at: 2026-11-15T01:38:37Z
updated_at: 2026-11-15T01:48:03Z
parent: clay-1m1e
blocked_by:
    - clay-x1fn
---

审查 src/commands/workspace_fd.c、include/clay/workspace_fd.h、tests/test_workspace_fd.c、Makefile 及调用者。验收：对照安全契约记录 openat2、回退、特殊文件拒绝的覆盖与缺陷，保留已有改动。

## Summary of Changes

审查 workspace_fd、Makefile 和 fs_tools/repo_map/undo 调用链：openat2 使用 BENEATH、NO_SYMLINKS、NO_MAGICLINKS；仅 ENOSYS/EINVAL/E2BIG 降级，其他错误失败，seccomp 注入覆盖拒绝与普通 I/O 错误。补充 NULL 路径、根句柄跨 chdir、FIFO 非阻塞与目录拒绝、中间符号链接和降级路径测试。普通及 CLAY_NO_OPENAT2 测试、fs-tools、repo-map、undo、git diff --check、lif check 通过。已知边界：回退和写入/删除的已打开目录可移出根；根可移动，硬链接及挂载不受约束；特殊文件在 fstat 拒绝前已被 open，设备打开可能产生副作用，未测试真实设备；根在各操作时打开而非会话固定，repo_map 遍历与读取根分别打开。seccomp 可伪造降级 errno，EPERM 不降级；非 Linux 未实测。保留既有工作区改动。
