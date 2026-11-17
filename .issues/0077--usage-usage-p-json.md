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
        - 88
        - 130
    depends-on:
        - 92
        - 93
        - 94
        - 95
created: "2026-11-17"
updated: "2026-11-17"
---

现状：usage 只在每轮结束时汇总（src/commands/message.c:1342-1350 on_usage_details、1604-1612 写回 commands），没有逐请求记录、没有 provider/模型/窗口归因，也没有 /usage 命令或 --json 输出；CHANGELOG.md:87-95 的 97% 是手工观察值。

killi 对照：usage 记录带 model/prompt/cached/completion/reasoning/finish_reason/ms（crates/killi/src/agent/request.rs:182-214）；session 记录 version 与 context_window（crates/killi/src/agent/lifecycle.rs:64-73）；--json 汇总（crates/killi/src/main.rs:363-386）。

任务（步骤见 #92-95）：
1. #92 逐请求记录到 ~/.clay/chats/<id>/requests.jsonl，一次 write() 整行。
2. #93 三条完成路径（openai / openai_codex / grok）都接上，不得写入发给 provider 的 messages。
3. #94 /usage 命令汇总本会话。
4. #95 -p --json 单行汇总。

本项只做“记录与展示”，不做命中率断言：真实命中数字由 #76 的手动运行验收，不做 CI 断言。

验收：一次双工具轮次后 /usage 与 jsonl 一致；--json 可解析；并发写入不错行；测试加入 UNIT_TEST_TARGETS。
