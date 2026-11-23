---
id: 152
title: shell_exec 支持 cwd 与结构化批量命令
status: open
priority: medium
labels:
    - type:task
    - area:tools
    - area:test
    - parent:148
relations:
    blocks:
        - 148
    related-to:
        - 150
        - 151
created: "2026-11-20"
updated: "2026-11-20"
---

## 现状

- `shell_exec` schema 只有 `command`、`args`、`timeout_seconds`（`src/commands/message.c:869-896`）；没有 `cwd`，也没有结构化批量命令。
- 沙箱模式在 `/workspace` 启动（`src/sandbox/linux.c:421-430`），但 unleashed 模式直接执行 shell、继承宿主 cwd；提示词却无条件说“shell_exec runs in /workspace”（`context.c:151`）。
- 真实日志：633 次 shell 调用中 391 次以 `cd` 开头（61.8%），其中 165 次去宿主路径、129 次去 `/workspace`、70 次去 `/tmp`。
- 545/633 次是复合命令；`args` 非空仅 42 次。批量能力存在，但没有结构化输出和失败语义。
- 每次 shell 调用都会 checkpoint 并走权限检查；批量命令需要明确“一次检查/逐条检查”的边界。

## 任务

1. 给 `shell_exec` 增加可选 `cwd`：
   - 只允许 workspace 内路径（以及明确允许的 `/scratch`）；拒绝逃逸、符号链接逃逸和宿主绝对路径。
   - 统一 sandbox / unleashed / Windows 的 cwd 语义；提示词同步更新。
2. 增加可选 `commands[]`：
   - 逐条执行并按命令分段输出；定义 `continue_on_error`（默认 false，首条失败即停）。
   - 总输出上限沿用 64 KiB；每段截断标记独立；总超时或每命令超时语义明确。
   - 权限检查逐条进行；拒绝的命令不执行，后续命令按 `continue_on_error` 决定是否继续。
   - checkpoint 每次调用一次；若中途失败，已执行命令的文件状态仍需可恢复。
3. 保留 `command`/`args` 旧路径，行为不变；新字段与旧字段互斥或定义优先级。
4. 更新工具描述，示例展示 `cwd` 与 `commands[]`；提示词去掉“所有命令都已在 /workspace，不要 cd”与实际模式不一致的表述。

## 验收

- `tests/test_message.c` 覆盖：cwd 正常/逃逸拒绝、commands 顺序执行、continue_on_error 两种取值、输出分段/总上限、超时、权限拒绝不执行后续。
- `tests/test_permissions.c` 覆盖逐条权限分类；`tests/test_sandbox.c` 覆盖 sandbox/unleashed cwd。
- `make test-message test-permissions test-sandbox` 通过；`make test` 通过。
- 手工在 sandbox 与 unleashed 各跑一次 `cwd` 批量命令，确认路径与输出一致。

## 不做

- 不做同轮只读工具并行执行（不减少轮次，只省墙钟）。
- 不改 provider 循环或轮次预算（#155）。
- 不引入新 shell 解析器；沿用现有 `clay_shell_authorize` 与权限路径。

## 规模

预计 20-30 轮；涉及 `src/commands/message.c`、`src/commands/permissions.c`、`src/sandbox/*`、`src/render/term.c`（如 cwd 解析需要）、测试。
