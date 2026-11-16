---
id: 29
title: 全量测试与 Linux 正常回退竞态矩阵
status: open
priority: medium
labels:
    - source:lif
    - type:task
    - parent:15
relations:
    blocks:
        - 24
    depends-on:
        - 17
        - 23
        - 20
    related-to:
        - 15
created: "2026-11-17"
updated: "2026-11-17"
---

检查 Makefile、.github/workflows/ci.yml、tests/test_workspace_fd.c、tests/test_fs_tools.c、tests/test_repo_map.c、tests/test_undo.c；运行全量及正常/强制回退矩阵，按需更新 CI。验收：记录命令、平台、失败及每项跳过原因；CI 覆盖可执行矩阵，跳过不算通过。

---

## 迁移来源（lif）

- 原 ID: `clay-x1q4`
- 原类型: `task`
- 原状态: `todo`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-1m1e` → #15
- 原阻塞于: `clay-6n35` → #17, `clay-u610` → #23, `clay-jk7c` → #20
- 原阻塞: 无
- 原创建时间: `2026-11-15T01:38:37Z`
- 原更新时间: `2026-11-15T01:38:37Z`
- 原文件路径: `.lif/clay-x1q4--全量测试与-linux-正常回退竞态矩阵.md`
