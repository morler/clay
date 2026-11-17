---
id: 62
title: 新增可复现的 ThreadSanitizer 测试目标
status: closed
priority: medium
labels:
    - parent:60
    - type:task
    - area:test
    - source:lif
relations:
    blocks:
        - 60
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

背景：#60 的并发路径目前只有离线 barrier/mock 覆盖；Makefile 的默认测试目标与 .github/workflows/ci.yml 都没有 -fsanitize=thread 入口，历史 TSAN 验证只能手工改 CFLAGS，无法复现。

范围：Makefile 增加独立的 sanitizer 构建与运行目标（例如 bin-tsan/test-tsan-subagent），不改变默认 make test 的行为；必要时补充 CI 或文档说明。

验收：一条明确命令可用 -fsanitize=thread -O1 -g 构建并运行 test_subagent 及必要的 provider refresh 测试；覆盖 Codex/Grok 单飞交错时零 TSAN 报告，目标可重复执行且失败时返回非零。

备注：本步骤只建立可复现入口，不要求在此修复竞态；后续并发步骤落地后需复跑该目标。

## 完成记录

Makefile 新增 `test-tsan`：用 clang 在独立的 `build-tsan/`、`bin-tsan/` 中以 `-O1 -g -fsanitize=thread -fno-omit-frame-pointer` 构建并运行 `test_subagent`、`test_openai_codex`、`test_openai_codex_refresh`、`test_grok`、`test_grok_refresh`，设置 `TSAN_OPTIONS=halt_on_error=1`；默认 `make test` 行为不变。`clean` 同步清理 TSAN 目录，`.github/workflows/ci.yml` 新增独立 tsan job，README 记录命令。

验证：删除 `build-tsan/`、`bin-tsan/` 后从零执行 `make test-tsan` 返回 0，五个测试全部通过且无 ThreadSanitizer 报告；`make test` 通过，`git diff --check` 通过。
