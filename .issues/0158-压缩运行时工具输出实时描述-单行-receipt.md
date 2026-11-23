---
id: 158
title: 压缩运行时工具输出：实时描述 + 单行 receipt
status: closed
priority: high
labels:
    - type:task
    - area:tools
created: "2026-11-23"
updated: "2026-11-23"
closed: "2026-11-23"
---

参考 ../neo：进行中的工具只占实时状态行（现在时），完成后在 transcript 留一行灰色 receipt（例如 `· Ran make test (1.2s)`），成功结果正文不再打印；失败仍显示错误与输出。turn 结束时 status 行给出工具数与耗时。

## 执行结果

- `render/task.c` 新增 `clay_task_receipt`：完成行是灰点 `·` + 过去式标签 + 耗时；`finish` 改为接收已格式化的结果字符串，`finish_format` 保留原有 varargs 入口。
- `message.c`：成功调用只留 receipt，不再打印结果正文；失败仍输出 error 与前 8 行输出。`tool_detail_key` 让 `shell_exec` 的实时行显示命令（`Running $ make test`），内联 receipt 为 `Ran $ make test`。
- turn 结束的 status 行变为 `N tools · 12.3s`（失败仍是红色耗时）。
- `make test-message` 覆盖：成功 read 只有一行且不含正文；失败 read 保留 error 与输出；成功 shell 为 `· Ran $ ...`；失败 shell 为 `✗ Failed` + `exit 1` + 输出。
- 手工：`--prompt` 场景输出 `· Read: README.md (0.1s)`，无文件正文；pty 状态行显示 `✓ 1 tool · 0.1s`。
- 附带修正：`--prompt` 在 `CLAY_RUN_OUT_OF_ROUNDS` 后返回 0（此前 auto-continue 成功收尾但进程退出码为 1）。
