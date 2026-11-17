---
id: 79
title: Codex 不再把可变 system 块提升到 instructions 顶部
status: closed
priority: high
labels:
    - type:task
    - area:cache
    - parent:76
relations:
    blocks:
        - 76
    depends-on:
        - 78
        - 99
        - 100
        - 101
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

现状：src/providers/openai_codex.c:645-659 把所有 role=system 的消息按顺序拼进顶层 instructions；而 src/commands/context.c:1690-1717 的环境块（日期、cwd、git 分支、顶层文件列表）和 notes 块是“变了就在对话尾部追加一条 system”。在 Codex 上每追加一次就改写请求开头，整个前缀缓存失效。这是本计划里影响最大的一处，应最先做。

killi 对照：system prompt 会话内冻结，变化只作为尾部 user 消息追加（crates/killi/src/agent/turn.rs:42-82、plan.rs:365-368）。

注意：Codex 的请求体是 Responses 协议，后续 system 消息未必被接受；实现前先用 tests/test_openai_codex*.c 的 mock 确认协议形状。

任务：
1. Codex 请求只在 instructions 里放冻结的系统提示（对话 index 0 + 子代理后缀），后续 system 块按原顺序保留在 input 数组中。
2. 如果协议不接受后续 system 消息，则改写成尾部 user 包裹的 context 文本，但同一历史在多次请求间必须字节一致。
3. 红测试先行：构造 system + 两条尾部 context 块，断言第二次序列化后 instructions 与第一次逐字节相同。

验收：环境块变化后 codex 请求的 instructions 不变；现有 codex 测试与 make test 通过；测试加入 UNIT_TEST_TARGETS。

## 2026-11-17 实现与验证

改动：src/providers/openai_codex.c 的 responses_input 只把 index 0 的 system 放进 instructions，后续 system 块（环境、notes）按原顺序留在 input。

测试：新增 tests/test_openai_codex_prefix.c（#99 红测试，已转绿），断言
- instructions 逐字节等于冻结的系统提示，不含环境/notes；
- 两个 context 块仍在 input 中按序出现；
- 同一历史两次序列化的请求体逐字节相同。
实测：instructions 12 B（拆分前 82 B，即每次环境/notes 变化都会改写这 70 B 的请求开头）。

验证：make test（34 个测试全部通过）、make test-openai-codex、test-subagent、test-grok 通过。
未验证：真实 Codex 端点的命中率——需要 OAuth 凭据，属 #75 一类真实网络验证；离线部分只能证明前缀不再变化。
