---
# clay-6n35
title: 实现 undo 乐观冲突检测
status: completed
type: task
priority: normal
created_at: 2026-11-15T01:38:37Z
updated_at: 2026-11-15T02:04:51Z
parent: clay-1m1e
blocked_by:
    - clay-imjp
---

修改 src/commands/undo.c、src/commands/fs_tools.c、tests/test_undo.c、tests/test_fs_tools.c；按安全契约检测快照后目标变化。验收：修改、替换、删除冲突和正常恢复有确定性测试；拒绝冲突并记录检查至提交间残余窗口。

## Summary of Changes

评估现有乐观检查：按存在性和内容拒绝不匹配，保留 undo 记录；写入成功后捕获到目标缺失时不再创建不可信 undo 记录。补充确定性原位修改、内容不同的替换、删除冲突及捕获前删除测试；沿用正常恢复和 clay-mqrk 比较后修改丢失测试。正常 test-undo/test-fs-tools/test-workspace-fd、CLAY_NO_OPENAT2 fallback 套件及 git diff --check 通过。比较到提交非原子、相同内容替换及快照捕获竞争仍不可检测；旧历史在新记录丢弃后仍保留。grep outside exposure 和 repo-map LEAK 为既有基线。
