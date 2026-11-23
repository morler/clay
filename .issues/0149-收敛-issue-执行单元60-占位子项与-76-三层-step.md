---
id: 149
title: '收敛 issue 执行单元：#60 占位子项与 #76 三层 step'
status: closed
priority: high
labels:
    - type:task
    - parent:148
relations:
    blocks:
        - 148
    related-to:
        - 76
        - 60
created: "2026-11-20"
updated: "2026-11-20"
closed: "2026-11-20"
---

## 现状

- 147 个 issue 中 51 个带 `step`，全部在 `#76` 下；`#76` 子树 67 个，占 45.6%。
- 43 个已关闭 step 中 19 个与兄弟 step 在同一个提交关闭（最多一次 5 个）；说明它们不是独立执行单元。
- `#60` 下 8 个 open 子项正文只有 `--label`，没有范围/验收，`#60` 因此无法推进。
- `#82` 下 `#108/#109` 是父子三层，父 issue 已包含子项任务。
- 父 issue 常写 `depends-on` 全部子项；`.issues/.agent.md:93` 又规定“只修改自己 claim 的 issue”，导致一个会话无法批量关闭父子项。

## 任务

0. 本 issue 自身是显式 batch：claim `#149` 后，允许修改 `#60`、`#82`、`#108`、`#109` 及其子项；完成后把该例外写进 `.issues/.agent.md`。

1. 处理 `#60`：把 8 个占位子项合并回 `#60` 或按 wontfix 关闭并写明理由；`#60` 正文补范围、验收、已完成的 2026-11-15/16 进展。
2. 处理 `#82`：把 `#108/#109` 合并回 `#82` 作为 checklist；关闭子项，保留一个可执行 issue。
3. 在 `.issues/.agent.md` 增加执行单元规则：
   - 一个可执行 issue = 一个可独立验收的行为、20-40 轮、单独合并后测试绿、一个提交。
   - 最多两层：Epic 只跟踪；Executable issue 才是会话单元；父 issue 用 checklist。
   - 不为红测试、加字段、接回调、加 Makefile target、补文档单独建 issue。
   - `step` 只表示可并行分配或确实超过一个会话；不表示实现顺序。
4. 放宽工作规则：允许 claim 父 issue 或显式 batch，并在同一提交批量关闭子项；用户已指定 issue 时跳过 `next/blocked`；把 `claim+show`、`done+diff+commit` 各合成一次调用。
5. 不重写已关闭的 51 个 step 历史；只建立新规则并清理仍 open 的占位项。

## 验收

- `issues show 60` 不再有正文只有占位符的 open 子项；`#60` 有明确范围/验收/进展。
- `issues show 82` 包含原 `#108/#109` 的任务 checklist，两个子项已关闭或不再作为独立执行单元。
- `.issues/.agent.md` 包含上述规则；新建 issue 按 DoR 写“行为/验收/不做什么/预计轮次”。
- `issues check --fix` 通过；`git diff --check` 通过；`.issues/` 变更与相关代码一起提交。

## 2026-11-20 执行结果

- `#60` 下 8 个占位子项 `#68`-`#75` 已合并为 `#60` 的 2026-11-20 checklist，并以 `wontfix` 关闭；`#60` 的 `depends-on` 已移除这 8 项。
- `#82` 下 `#108/#109` 已合并为 `#82` 的 2026-11-20 checklist，并以 `wontfix` 关闭；`#82` 的 `depends-on` 已移除。
- `.issues/.agent.md` 增加 Execution Units、Batch Work，更新 Recommended Workflow，并允许显式 batch 修改列出的子项。
- 未重写已关闭的 51 个 step 历史。
- `issues check --fix` 通过；`git diff --check` 通过。`issues next` 现在指向 `#60`（合并后的 OAuth 修复项），说明占位子项不再阻塞它。

## 不做

- 不批量删除或重写 51 个已关闭 step。
- 不给 Clay 增加 git-issues 专用工具；`shell_exec` 足够。
- 不在本项内改轮次循环或工具实现。

## 规模

预计 10-20 轮；涉及 `.issues/*.md`、`.issues/.agent.md`。
