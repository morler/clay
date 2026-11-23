---
id: 150
title: 系统提示词加入轮次经济规则并修正 todo/subagent 描述
status: closed
priority: high
labels:
    - type:task
    - area:prompt
    - parent:148
relations:
    blocks:
        - 148
    related-to:
        - 151
        - 152
created: "2026-11-20"
updated: "2026-11-20"
closed: "2026-11-20"
---

## 现状

`CLAY_SYSTEM_PROMPT_BASE`（`src/commands/context.c:14-206`）缺少“一轮可以放多个工具调用”的规则，反而有几处鼓励单步：

- `context.c:88-90`：“work through them one at a time, keeping exactly one step in_progress and marking it completed before starting the next.”
- `context.c:159-160`：“Keep commands focused and summarize what came back.”
- `context.c:146-148` 让模型用 read/grep 代替 shell 的 cat/sed/find/grep，但 read/grep 都是一次一个目标，模型因此回退到 shell。
- 工具描述 `src/commands/message.c:1490-1494` 说 subagent 是“one self-contained step”，schema 却支持最多 4 个分支（`subagent.c:434-440`）。

实测：472 个工具轮中 459 个只发 1-2 个调用；633/769 次调用是 shell_exec；shell grep 162 次 vs grep 工具 14 次。

## 任务

1. 在系统提示词增加 `# Round economy` 段：
   - 一条 assistant 消息 = 一轮；互不依赖的 read/grep/glob/shell 调用放在同一条消息里，3-6 个一起发。
   - 相关只读检查合并进一条 shell 命令；一条命令只做一个逻辑检查。
   - 不要用一轮只复述/宣布下一步；把说明留给工具结果之后的总结。
2. 改写 todo 规则：完成上一步 + 开始下一步可以在同一次 `todowrite` 里完成；状态更新与该步的第一个/最后一个工具调用同一条消息；少于约 5 步的任务不写 todo。
3. 改写 `Keep commands focused` 为“合并相关只读检查；独立检查放在同一条消息”，并注明 shell 已在 workspace，不要每条命令前缀 `cd`（cwd 的代码支持由 #152 处理）。
4. 把 subagent 描述改成“hand one or more independent branches（最多 4 个）”，与 schema 一致。
5. 若提示词字节预算测试失败，更新其预期值，不删减安全/验证规则。

## 验收

- `CLAY_PROMPT_BREAKDOWN=1` 输出显示新增段落字节数；总预算仍在测试允许范围内。
- `make test-token-budget test-message` 通过；必要时同步 `tests/test_token_budget.c` 的期望字节数。
- 新增一条断言/测试保证基座提示词包含批处理规则，防止后续改动删掉。
- 人工检查：提示词不再出现“one at a time”“Keep commands focused”这类单步措辞。

## 2026-11-20 执行结果

- 基座提示词新增 `# Round economy`：一条 assistant 消息是一轮，可携带多个独立 read/grep/glob/shell 调用；相关只读检查合并为一条命令；不要用一轮只叙述下一步。
- todo 规则改为：保持一个 in_progress，完成上一步与开始下一步在同一次 `todowrite` 中完成，并与该步首个工具调用同一条消息；少于约 5 步不写 todo。
- `Keep commands focused` 改为合并相关只读检查，并注明 shell 已在 `/workspace`、不要每条命令前缀 `cd`。
- `subagent` 工具描述改为 “one or more independent branches”，与最多 4 个任务的 schema 一致。
- 预算：system prompt 9129 B；prompt+tools 17323 B（≤17400）；subagent fixed cost 16776 B（≤16800）。
- `tests/test_token_budget.c` 新增基座提示词断言，防止删掉批处理规则或恢复单步措辞。
- `make test`、`git diff --check` 通过。

## 不做

- 不在本项内改 read/grep/shell/edit 的 schema 或执行层（分别见 #151、#152）。
- 不引入新的提示词模板/配置系统。
- 不改变安全、验证、git、memory 等既有规则。

## 规模

预计 10-15 轮；涉及 `src/commands/context.c`、`src/commands/message.c`、`tests/test_token_budget.c`。
