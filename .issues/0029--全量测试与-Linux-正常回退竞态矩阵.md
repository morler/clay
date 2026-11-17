---
id: 29
title: 全量测试与 Linux 正常回退竞态矩阵
status: closed
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
closed: "2026-11-17"
---

检查 Makefile、.github/workflows/ci.yml、tests/test_workspace_fd.c、tests/test_fs_tools.c、tests/test_repo_map.c、tests/test_undo.c；运行全量及正常/强制回退矩阵，按需更新 CI。验收：记录命令、平台、失败及每项跳过原因；CI 覆盖可执行矩阵，跳过不算通过。

## 执行记录（2026-11-17）

平台：Linux fedora 7.3.3-200.fc44.x86_64 x86_64，Homebrew clang 23.1.5。命令：`make -j4 test`、`make -j4 test-workspace-fd test-fs-tools test-undo test-repo-map`、`make -j4 test-fs-fallback`，以及正常/回退二进制的 `CLAY_ASSERT_GREP_RACE_SAFETY=1 ./bin/test_fs_tools`、`CLAY_ASSERT_GREP_RACE_SAFETY=1 ./bin-fs-fallback/test_fs_tools`。所有命令返回 0，`git diff --check` 通过；grep 起始目录与打开文件后路径交换在 unleashed、本机 sandbox、正常和强制回退构建中均为 `outside content not observed`。

跳过与噪声：`CLAY_NO_OPENAT2` 构建按设计打印 `openat2-specific tests skipped`；本机 sandbox 可用，未跳过 sandbox grep 集成；本机非 root，权限测试未跳过；`LEAK function 1 /outside/secret.c` 来自 repo-map 的假 ctags 脚本输出，不是泄漏断言。跳过项不计为通过，CI 仍要求正常与回退矩阵和安全断言全部执行。

CI 与入口：Makefile 新增 `test-fs-matrix`，串联正常文件工具测试、grep 安全断言、强制回退测试及回退安全断言；`.github/workflows/ci.yml` 在 `make test` 后运行该目标。

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
