---
id: 21
title: 梳理 write edit undo 检查提交链路
status: closed
priority: medium
labels:
    - source:lif
    - type:task
    - parent:15
relations:
    blocks:
        - 19
    depends-on:
        - 22
    related-to:
        - 15
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

审查 src/commands/fs_tools.c、src/commands/undo.c、src/commands/workspace_fd.c、tests/test_fs_tools.c、tests/test_undo.c。验收：记录检查、快照、临时文件、提交、删除、清理及残余窗口，建立确定性交错和失败清理测试。

## 调用链与确定性交错审查

- message.c 的 Write/Edit 包装器持 tool_lock，先检查 Plan/权限并 checkpoint，再调用 undo_prepare 读取 before；prepare 返回值未阻止操作。工具成功后 undo_commit 检查路径并读取 after，失败则 discard；该锁不约束外部进程。直接调用 fs 工具不经过这些包装。
- fs_tools.c 先规范化并逐组件 lstat 检查路径；Linux 每次读、写、删除都独立打开工作区根句柄。edit 先读取并检查替换文本、唯一性，然后另行写入；这不是条件更新。
- workspace_fd.c 写入先固定父目录、无跟随打开并检查目标为普通文件及 ACL，随机 O_EXCL 建同目录临时文件，复制部分元数据，写入、fsync、close，再 renameat 提交；失败时尝试 unlinkat 临时文件并关闭句柄。创建的父目录不回滚，清理 unlinkat 的错误被忽略；未 fsync 父目录，提交不保证崩溃持久。名称检查、元数据读取和 rename 不原子；可覆盖检查后的并发替换。硬链接、目录移出根、挂载和其他元数据限制见 README。
- undo_prepare/commit 分别捕获上限 4 MiB 的前后字节及存在性；失败时无 undo 记录（操作仍可成功）。undo 重新检查路径并比较当前快照，之前存在则重新写回，否则 fstatat 检查普通文件后 unlinkat；比较、检查和写/删均非原子，相同内容的中间变化也无法检测。提交后捕获的 after 可能是其他写入者的内容，导致误认为其为本次结果。删除时 fstatat 与 unlinkat 间目标可替换，删除的是当前目录项，且不跟随符号链接。
- tests/test_fs_tools.c 在 rename 前确定性注入并发修改，分别证明 write 和 edit 可覆盖修改；tests/test_undo.c 在恢复 rename 和删除 unlink 前注入修改，证明比较后的更改可丢失。它们刻画既有契约限制，并非修复。tests/test_workspace_fd.c 对 fsync 和 rename 注入 EIO，检查目标内容不变及临时文件清理；既有同步测试覆盖解析前和父目录固定后交换。未穷举所有写入、close、清理失败及所有快照阶段。
- 正常 make test-workspace-fd test-undo test-fs-tools 与 make test-fs-fallback 通过；后者包含 repo-map 并有意跳过 openat2 专项。测试日志中 grep outside exposure 和 repo-map LEAK 是既有基线输出，另有独立任务跟进，不在本条修复。

## Summary of Changes

完成 write/edit/undo 检查、快照、提交、删除与清理链路审查；补充确定性交错及失败清理测试，记录残余竞态和范围限制；正常及强制回退针对测试通过。

---

## 迁移来源（lif）

- 原 ID: `clay-mqrk`
- 原类型: `task`
- 原状态: `completed`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-1m1e` → #15
- 原阻塞于: `clay-mubf` → #22
- 原阻塞: 无
- 原创建时间: `2026-11-15T01:38:37Z`
- 原更新时间: `2026-11-15T01:59:46Z`
- 原文件路径: `.lif/clay-mqrk--梳理-write-edit-undo-检查提交链路.md`
