---
id: 77
title: 逐请求 usage 记录、/usage 与 -p --json
status: open
priority: high
labels:
    - type:task
    - area:tokens
    - parent:76
relations:
    blocks:
        - 76
        - 79
        - 82
        - 83
    depends-on:
        - 92
        - 93
        - 94
        - 95
created: "2026-11-17"
updated: "2026-11-17"
---

现状：usage 只在每轮结束时汇总（src/commands/message.c:1342-1350 on_usage_details、1604-1612 写回 commands），没有逐请求记录、没有 provider/模型/窗口归因，也没有 /usage 命令或 --json 输出；CHANGELOG.md:87-95 的 97% 是手工观察值。killi 对照：usage 记录带 model/prompt/cached/completion/reasoning/finish_reason/ms（crates/killi/src/agent/request.rs:182-214），session 记录 version 与 context_window（crates/killi/src/agent/lifecycle.rs:64-73），--json 汇总（crates/killi/src/main.rs:363-386）。

任务：
1. 在 src/providers/openai.c 的 on_usage 回调链上增加一个逐请求记录点（provider id、model、round 序号、prompt/cached/completion、上下文预算），落到 clay 现有的 chat journal 或新的 ~/.clay/logs/<chat>.jsonl；不能污染发给 provider 的 messages。
2. 新增 /usage 命令（src/commands/register.c 注册），打印本会话请求数、工具调用数、输入/缓存比例/输出，以及未命中连续次数。
3. -p 增加 --json，输出 requests/tool_calls/input_tokens/cached_tokens/output_tokens/duration_ms。
验收：一次双工具轮次后 /usage 数字与 provider 返回一致；--json 在测试中稳定断言；journal 旧格式可读。
