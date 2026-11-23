---
id: 156
title: 工作流可见：todowrite 列表带标题、失败/跳过状态与进行中详情
status: closed
priority: high
labels:
    - type:feature
    - area:ui
created: "2026-11-23"
updated: "2026-11-23"
closed: "2026-11-23"
---

## 现状

- `todowrite`（src/commands/message.c:759）只有 content+status，渲染只打印**变动的**步骤（render_plan，src/commands/message.c:1179），没有标题、没有进度头部。
- 状态只有 pending/in_progress/completed：被跳过的步骤只能删掉，失败的步骤只能留在 in_progress。
- 进行中的步骤不携带"这个步骤正在做什么"，用户只能从上面的工具行猜。
- 状态行有 `2/3 步骤`，但 transcript 里没有整体进度概览。

参考 ../neo/ internal/workflow + internal/tui/blocks.go 的 workflowBlock。

## 任务

1. `todowrite` 增加可选 `title`，plan 保留它（ClayPlan.title）。
2. 状态增加 failed/skipped（valid_todo_status + schema enum）。
3. 首次渲染打印标题行 + `N/M complete · x failed · y skipped`；之后仍只打印变动步骤（保持现有行为）。
4. 进行中步骤显示最近一次工具活动作为 detail（失败/跳过时清空）。
5. `clay_plan_step` 增加失败（✗）与跳过（-）状态。

## 验收

- `make test test-todowrite` 通过；新增断言覆盖 title 保留/failed/skipped 接受/非法状态拒绝。
- 状态行与 transcript 在步骤失败后仍显示整体进度。
- 提示词预算测试仍在 CLAY_PROMPT_AND_TOOL_BUDGET 内。

## 不做

- 不新增 workflow 工具，不改 todowrite 的整表语义。
- 不做原地重绘的清单块（追加式输出保持不变）。

## 规模

预计 6-10 轮；涉及 src/commands/{context.h,message.c}、src/render/list.c、include/clay/list.h、tests/test_todowrite.c。
