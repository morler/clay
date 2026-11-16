---
id: 19
title: 加固 write 和 edit 提交及权限
status: closed
priority: medium
labels:
    - source:lif
    - type:task
    - parent:15
relations:
    blocks:
        - 17
    depends-on:
        - 21
        - 25
    related-to:
        - 15
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

修改 src/commands/fs_tools.c、src/commands/workspace_fd.c、tests/test_fs_tools.c；采用句柄相对操作并保留权限、现有 POSIX ACL 拒绝或失败关闭及错误清理。验收：正常及回退交换、权限、ACL、临时文件清理测试通过；说明无法原子条件替换及残余窗口。

## Summary of Changes

Linux 对检查时不存在的目标使用同目录 linkat 排他提交，避免检查后新出现的目录项被覆盖；失败时沿用临时文件清理。既有目标仍使用 renameat，权限、owner 与 ACL 检查路径保持不变。新增工具层确定性交错及清理测试；正常 test-workspace-fd、test-fs-tools、test-undo 和 CLAY_NO_OPENAT2 回退套件通过，git diff --check 通过。此措施也覆盖 edit 写入时目标缺失的情况，但不检测 edit 读取后到写入检查前的变更，不是一般条件替换；已有目标或检查后变化仍可丢失。linkat 需要文件系统支持硬链接，成功后临时名清理仍为尽力而为。grep outside exposure 与 repo-map LEAK 是已记录的独立基线。

---

## 迁移来源（lif）

- 原 ID: `clay-imjp`
- 原类型: `task`
- 原状态: `completed`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-1m1e` → #15
- 原阻塞于: `clay-mqrk` → #21, `clay-uxeh` → #25
- 原阻塞: 无
- 原创建时间: `2026-11-15T01:38:37Z`
- 原更新时间: `2026-11-15T02:02:09Z`
- 原文件路径: `.lif/clay-imjp--加固-write-和-edit-提交及权限.md`
