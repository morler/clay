---
id: 161
title: 计划清单块实时刷新：header 统计、运行项活动与失败收尾
status: open
priority: high
labels:
    - type:feature
    - area:ui
relations:
    blocks:
        - 160
        - 163
        - 164
created: "2026-11-23"
updated: "2026-11-23"
---

## 现状

- `todowrite` 保存整表（context.h ClayPlan），`render_plan`（src/commands/message.c:1318）首次变化时打印 header，之后只打印状态变化的步骤行。
- header 的 done/failed/skipped 是首次渲染那一刻的快照（`clay_plan_header`，src/render/list.c:391）；此后再变化不会更新，屏幕最终留下 `0/N complete` 的历史行。
- running 项的最近工具活动写进 `item->detail`（on_tool_call，message.c:1400 附近），只用于 below 的 `plan` 状态行（render_plan 末尾），清单本身不显示。
- `clay_plan_step` 打印一次后不会再改；失败/跳过有图标，但没有终态汇总。
- below 的 plan 模块只有 1-2 行（render/below.c module_row_count 上限 2），不适合承载整张清单。

参考 ../neo internal/tui/blocks.go 的 workflowBlock 与 workflow_events.go：清单是 transcript 里的稳定块，创建时一次分配全部行，之后原地更新 glyph、detail、header。

## 任务

1. 在 src/render 新增最小的 live block 渲染契约，例如：
   - `clay_live_block_begin(rows)`：记录块首行，之后所有重绘都回到这里；
   - `clay_live_block_row(block, index, text)` / `clay_live_block_redraw(block)`：逐行清除并重写；
   - `clay_live_block_settle(block)`：块留在屏幕上，光标移到块后。
   只在 `clay_term_is_interactive()` 时启用；非交互（`--prompt`、管道、Windows 无 VT 时）退化为逐行追加，不输出光标移动序列。
2. todowrite 渲染改用该契约：创建/替换计划时分配 `1 + items` 行并整块绘制；后续 todowrite 只改变化的行；header 每次重绘都按当前 done/failed/skipped 显示 `N/M complete · x failed · y skipped`。
3. running 项在块内显示最近一次工具活动的 detail（单行截断）；离开 running 时清空，避免把上一步的活动留给下一步。
4. 生命周期收尾：turn 结束、取消、`/new`、`/resume`、新计划替换旧计划时都要 settle 旧块；settle 后后续输出不能被写进块里。与 render/task.c 的 `\r` spinner 行、below 状态行、流式 response 交错时保持正确：重绘前暂停 spinner 行，重绘后恢复。
5. 窄终端按 `clay_term_width()` 裁剪，单行不换行；块行数超过屏幕可用高度时显示前几行加 `… +k` 汇总行，仍保持行数稳定。
6. 更新 ARCHITECTURE.md 的 render/commands 描述。

## 验收

- `tests/test_todowrite.c`：header 计数随每次 todowrite 更新；running detail 写入与清空；失败/跳过的行保留。
- 新增 pty 用例（参考 tests/test_subagent_pty.py 的驱动方式）：创建 3 步计划 → 依次 in_progress/completed/failed，断言最终屏幕上只有一块清单、header 为最终计数、失败行仍在、块后输出完整。
- 非交互运行不出现 ANSI 光标移动序列（grep 输出断言）。
- `make test-todowrite test-pty test-message` 通过；`make test` 通过。

## 不做

- 不改 todowrite 的整表语义和状态枚举。
- 不把清单搬进 below 或新增面板。
- 不保证用户滚动历史后块仍刷新（settle 后不再触碰）。

## 规模

预计 15-20 轮；涉及 src/render（新文件或 list.c）、src/commands/message.c、src/commands/context.h、tests/test_todowrite.c、tests 下 pty 用例。
