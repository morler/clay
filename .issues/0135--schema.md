---
id: 135
title: 工具 schema 瘦身
status: closed
priority: medium
labels:
    - type:task
    - area:prompt
    - parent:89
    - step
relations:
    blocks:
        - 89
    depends-on:
        - 134
created: "2026-11-17"
updated: "2026-11-18"
closed: "2026-11-18"
---

改动：src/commands/message.c:1361-1443 缩短描述、去掉与系统提示重复的内容、统一 required/optional 说明；不改参数语义。
验证：预算测试下降；工具调用相关测试全绿。

## 2026-11-18 完成记录

工具 schema：8535 → 7885 字节（-650）；`prompt + tools` 17707 → 17057。

先做度量：`serialized_tool_bytes()` 新增逐工具打印（`per tool, largest first:`），这是 #89 任务 1 要的东西，也让这次的每一处削减能对应到具体工具。

削掉的是三类重复：

1. `subagent`（961 → 805）：`tasks.description` 里的 "give each branch its own files: two of them editing the same file will fight" 与基座提示逐字重复；`prompt.description` 的 "as if briefing someone who has not seen this conversation" 同理。
2. `ask_user`（853 → 793）：`options.description` 的 "most likely first / Omit only when no option can be guessed" 与基座提示重复；`slug` 示例过长。
3. `todowrite`（622 → 517）：`todos.description` 里 "resending every task each call / exactly one in_progress" 与基座提示重复。

另外压缩措辞：`shell_exec` 的 `timeout_seconds`（去掉指向 task_run 的重复说明）、`repo_map` 工具描述、`glob` 的 pattern 说明、`write`/`edit` 的 path 与 oldString 说明、`memory_save` 的 slug/summary 说明。

**没动**：所有参数名、类型、required 列表、enum、additionalProperties —— schema 语义零变化，只改 description 字符串。

预算：`CLAY_PROMPT_AND_TOOL_BUDGET` 18000 → 17400（实测 17057）。

验证：
- `make test` 全绿；44 个 UNIT_TEST_TARGETS 逐个运行，failures=0。
- 工具调用相关专项：test_message / test_fs_tools / test_todowrite / test_ask_user / test_tasks / test_subagent / test_mcp 全部通过。
- 反证：给 `subagent` 的 prompt 描述塞 500 字节填充，预算断言立即失败（首个较小的 100 字节填充落在 343 B 余量内没触发，说明余量是真实的，不是断言没生效）。
