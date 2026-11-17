---
id: 79
title: Codex 不再把可变 system 块提升到 instructions 顶部
status: open
priority: high
labels:
    - type:task
    - area:cache
    - parent:76
relations:
    blocks:
        - 76
    depends-on:
        - 77
        - 78
        - 99
        - 100
        - 101
created: "2026-11-17"
updated: "2026-11-17"
---

现状：src/providers/openai_codex.c:645-659 把所有 role=system 的消息按顺序拼进顶层 instructions；而 src/commands/context.c:1690-1717 的环境块（日期、cwd、git 分支、顶层文件列表）和 notes 块是“变了就在对话尾部追加一条 system”。在 Codex 上每追加一次就改写请求开头，整个前缀缓存失效。killi 对照：system prompt 会话内冻结，变化只作为尾部 user 消息追加（crates/killi/src/agent/turn.rs:42-82、plan.rs:365-368），decisions 9/25/49。

任务：
1. 让 Codex 请求只在 instructions 里放冻结的系统提示（对话 index 0 + 子代理后缀），后续 system 块按原顺序保留在 input 数组中（role/system 或 developer 消息）。
2. 如果 Responses 协议不接受后续 system 消息，则改写为尾部 user 包裹的 context 文本，但必须保证同一历史在多次请求间字节一致。
3. 加测试：构造 system + 两条尾部 context 块，断言第二次序列化后 instructions 与第一次逐字节相同（tests/test_openai_codex*.c）。
验收：环境块变化后 codex 请求的 instructions 不变；现有 codex 测试与 make test 通过。
