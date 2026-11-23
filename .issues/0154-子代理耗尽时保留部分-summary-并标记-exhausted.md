---
id: 154
title: 子代理耗尽时保留部分 summary 并标记 exhausted
status: closed
priority: high
labels:
    - type:bug
    - area:subagent
    - parent:148
relations:
    blocks:
        - 155
        - 148
created: "2026-11-20"
updated: "2026-11-23"
closed: "2026-11-23"
---

## 现状

- 子代理每次 run 最多 32 轮（`src/commands/context.h:465`、`subagent.c:200-203`）。
- 结果判定是 `run->rc == 0 && run->summary.len > 0`（`subagent.c:347-358`）。
- `rc == CLAY_RUN_OUT_OF_ROUNDS` 时，即使 summary 非空也判 `ok=false`，输出只写 `(no summary came back)`（`subagent.c:378-381`），父代理拿到错误文本“do this branch yourself or split it further”，只能重做整个分支。
- 现有的无工具补摘要只在 `rc == 0 && summary.len == 0` 时触发（`subagent.c:206-215`），不覆盖 `rc == 2`。

## 任务

1. `rc == 2` 且 `summary.len > 0`：视为部分成功，返回 summary；entry 增加 `exhausted: true` 和 `rounds: 32`；顶层 `ok` 可仍为 false（有分支未完整完成），但 `output` 必须包含 partial summary。
2. `rc == 2` 且 summary 为空、`tool_calls > 0`：触发一次无工具补摘要（复用现有 `tools=NULL, max_rounds=1` 路径）；成功则按 partial 返回，失败则返回 transcript 路径 + 明确“32 轮预算耗尽”的错误。
3. 错误文本不再对所有失败统一写“no summary came back”；区分取消、预算耗尽、真实无输出。
4. transcript 始终保存并返回；父代理能据此接手，不必重跑。
5. 测试 `tests/test_subagent.c` 新增 mock：32 轮全部返回 tool call 后停止；断言 partial summary 可见、`exhausted` 存在、transcript 可读；无 summary 分支断言补摘要与错误文本。

## 验收

- `make test-subagent` 通过；`make test` 通过。
- 新用例覆盖：有 summary 的 rc==2、无 summary 的 rc==2、rc==1 取消、rc==0 无 summary 的旧兜底。
- 父代理 `subagent` 结果中不再把“有 summary 但耗尽”误报成“没有 summary”。

## 不做

- 不在本项内做子代理自动续跑（#155）。
- 不改 32 轮上限。
- 不改 transcript 的 JSON schema（只加字段）。

## 规模

预计 10-15 轮；涉及 `src/commands/subagent.c`、`tests/test_subagent.c`。
