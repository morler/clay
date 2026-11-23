---
id: 162
title: turn 完成收据对齐：plan 统计、失败态与轮次预算提示
status: open
priority: high
labels:
    - type:feature
    - area:ui
relations:
    blocks:
        - 160
created: "2026-11-23"
updated: "2026-11-23"
---

## 现状

- `set_status`（src/commands/message.c:1023）只打印 `N tools · 12.3s`，成功绿色、失败红色；所有调用点只传秒数、成功标志和工具数。
- plan 的完成/失败没有进入收据；below 的 plan 模块在没有 in_progress 时直接禁用，最终状态从屏幕上消失。
- 轮次预算耗尽时另行打印一条黄色提示，收据本身仍和普通结束一样。
- neo 的 resultSummaryBlock：`✓ Done · 3/3 verified · 12s`、`✗ Finished with issues · 2/3 complete · 1 failed · 1 skipped`，标签回落到 workflow 标题，max turns 时 `Paused · reply to continue`。

## 任务

1. `set_status` 接受 plan 统计（total/done/failed/skipped）与 turn 结束原因；成功且全部完成显示 `N/N verified`，未完成显示 `x/y complete`，有失败/跳过时追加计数。
2. 有 failed 项时收据整体为失败态（红色 ✗），与 neo 一致；turn 因 `CLAY_RUN_OUT_OF_ROUNDS` 结束时显示 `Paused · reply to continue`，替代当前单独的黄色提示（提示文字缩短或删除）。
3. 无 plan 时保持现有 `N tools · 12.3s`，不出现空统计。
4. plan 完成或失败后隐藏 below 的 plan 模块，最终状态只由收据承载，不同时在两处显示。
5. 收据只写屏幕，不进 chat journal，不改变 `/resume` 恢复的内容。

## 验收

- `tests/test_message.c`：有 plan 成功、有 failed、有 skipped、无 plan、OUT_OF_ROUNDS 五种收据文本；成功/失败颜色断言保持现有方式。
- `make test-message test-todowrite` 通过；`make test` 通过。
- 手工：跑一个 3 步计划全部完成，收据为 `✓ Done · 3/3 verified · Ns`；一个失败，收据为 `✗ Finished with issues · 2/3 complete · 1 failed`。

## 不做

- 不做 turn 状态持久化或 incomplete 元数据（#155 的剩余部分另算）。
- 不把收据写进 chat.json。

## 规模

预计 6-10 轮；涉及 src/commands/message.c、src/commands/context.h、tests/test_message.c。
