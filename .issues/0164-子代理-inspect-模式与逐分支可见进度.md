---
id: 164
title: 子代理 inspect 模式与逐分支可见进度
status: open
priority: high
labels:
    - type:feature
    - area:subagent
relations:
    blocks:
        - 160
    depends-on:
        - 161
created: "2026-11-23"
updated: "2026-11-23"
---

## 现状

- subagent 工具一次接收 tasks[]（最多 4），全部 pthread 并发；每个分支拿完整工具集且可写（subagent.c run_subagent → clay_commands_tools_build(..., 0)），写操作靠 tool_lock 串行。
- UI 只有一个 spinner task，report_progress 把它 relabel 成 `Delegating N branches: X done, Y tool calls`；看不到每个分支在做什么，也看不出哪个分支失败或完成。
- neo：agent 工具有 mode=work|inspect；inspect 只有 read_file/grep/glob，可并行；每个分支在组块中一行，显示描述 + 最新活动 + 耗时；父结果按源顺序。

## 任务

1. schema：每个 task 增加可选 `mode`（`work` 默认、`inspect`），未知值失败关闭，不退回 work。
2. inspect 分支工具集只保留 read/grep/glob/repo_map；不带 shell_exec、write/edit、task_*、mcp、skill、subagent、ask_user、todowrite。work 分支保持现状。
3. 进度：复用 #160 的 live block，每分支一行：描述、最新工具活动、耗时、glyph（●/✓/✗/-）。按 tasks 源顺序，不按完成顺序重排；失败分支行保留。
4. 取消：父取消传播到所有分支（已有 should_abort），组块结算。
5. 结果：每个 entry 增加 `mode`；summary 仍按现有 16 KiB 等额裁剪；父代理收到的 output 顺序不变。transcript 记录 mode 与工具集。
6. 工具描述与系统提示更新：说明 inspect 适合独立调查，work 一次一个变更。

## 验收

- `tests/test_subagent.c`：inspect 分支工具集断言（无写/shell/agent 工具）、未知 mode 失败、work+inspect 混合批次、mode 进入 transcript 与结果。
- 扩展 `tests/test_subagent_pty.py`：两个分支一成功一失败，断言两行、各自活动、最终 ✓/✗。
- `make test-subagent test-pty` 通过；`make test` 通过；提示词预算在 CLAY_PROMPT_AND_TOOL_BUDGET 内。

## 不做

- 不做嵌套代理、worktree 隔离、后台/跨进程子代理。
- 不改现有 4 分支上限和 16 KiB summary 预算。

## 规模

预计 15-20 轮；涉及 src/commands/subagent.c、message.c（schema）、context.h、src/render、tests。
