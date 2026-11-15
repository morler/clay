---
# clay-mubf
title: 构建确定性交错与强制回退测试
status: completed
type: task
priority: normal
created_at: 2026-11-15T01:38:37Z
updated_at: 2026-11-15T01:53:04Z
parent: clay-1m1e
blocked_by:
    - clay-wnop
---

在 tests/test_workspace_fd.c、tests/test_fs_tools.c、tests/test_undo.c、tests/test_repo_map.c 和 Makefile 按需增加同步及 CLAY_NO_OPENAT2 构建。验收：交换可确定重现、不依赖 sleep；正常和强制回退验证安全边界，清晰记录跳过。

## Summary of Changes

测试专用编译同步点确定性执行路径解析前及父目录句柄固定后的目录/逃逸符号链接交换，覆盖读取、写入和删除；断言外部文件不变，生产对象不含同步点。新增隔离构建的 test-fs-fallback（CLAY_NO_OPENAT2），普通及强制回退下 workspace-fd、fs-tools、undo、repo-map 测试通过；强制回退有意跳过 openat2 专项，其他跳过未出现。git diff --check 和 lif check 通过。已打开目录被移至根外仍属既定限制；其他调用层原有压力测试非确定性交错。
