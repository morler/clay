---
# clay-x1q4
title: 全量测试与 Linux 正常回退竞态矩阵
status: todo
type: task
priority: normal
created_at: 2026-11-15T01:38:37Z
updated_at: 2026-11-15T01:38:37Z
parent: clay-1m1e
blocked_by:
    - clay-6n35
    - clay-u610
    - clay-jk7c
---

检查 Makefile、.github/workflows/ci.yml、tests/test_workspace_fd.c、tests/test_fs_tools.c、tests/test_repo_map.c、tests/test_undo.c；运行全量及正常/强制回退矩阵，按需更新 CI。验收：记录命令、平台、失败及每项跳过原因；CI 覆盖可执行矩阵，跳过不算通过。
