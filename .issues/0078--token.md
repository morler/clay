---
id: 78
title: 离线 token 基线基准与固定预算测试
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
        - 80
        - 81
        - 86
        - 89
        - 105
    depends-on:
        - 96
        - 97
        - 98
created: "2026-11-17"
updated: "2026-11-17"
---

现状：仓库没有任何 token 基准或预算断言。CHANGELOG.md:87-95 只说“第三个 turn 97%”，代码无测量（grep cache/usage 只有 tests/test_chat.c、test_grok.c 的解析级断言）。killi 对照：system prompt + 工具 11,714 字符由 tests.rs:5-16 强制；scripts/eval.py 跑 14 个任务比 input/cached/输出（docs/eval.md:11-22）；scripts/bench.py 取 first_prompt_tokens。

任务：
1. 新增 tests/test_token_budget.c 或复用现有测试：断言 CLAY_SYSTEM_PROMPT_BASE 长度 + 18 个工具 schema 序列化长度 <= 上限（先按现状记录 4.5k tokens 折算，目标降至 3k 字符预算内），失败时打印实测值。
2. 增加一个离线可复现的基准：给定固定 conversation（含 read/bash/grep 结果），统计序列化后的请求体字节数与按 provider 估算的 prompt_tokens，写入 tests 输出或 scripts/tokens.sh；不联网。
3. 记录基线到 .issues/#76 评论或 docs，供后续子项对比。
验收：make test 中预算断言生效；基准在三次运行下同值；改动后能看出每阶段节省。
