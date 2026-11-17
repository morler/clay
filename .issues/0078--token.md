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

现状：仓库没有任何 token 基准或预算断言。CHANGELOG.md:87-95 只说“第三个 turn 97%”，代码无测量（只有 tests/test_chat.c、test_grok.c 的 usage 解析级断言）。killi 对照：crates/killi/src/tests.rs:5-16 断言 prompt + 工具 <= 12,000 字符；scripts/eval.py 比每任务 input/cached；scripts/bench.py 取 first_prompt_tokens。

单位：killi 的上限是 12,000 字符（约 3k token，按 4 字符/token）。本项与 #96/#89 一律写字节，不写“3k 字符”。

任务：
1. tests/test_token_budget.c：序列化 CLAY_SYSTEM_PROMPT_BASE + 工具 schema，断言总字节 <= 常量上限；失败打印实测总值与逐项字节。先按现状设上限，随 #89 收紧。
2. 离线基准：固定 conversation fixture（read/bash/grep 结果各一条），输出序列化后的请求体字节数，以及按 src/commands/scope.c:343-346 既有口径（请求字节 ÷ 2 + 输出预留）估算的 tokens；不联网，不新增第三种估算。
3. 把基线数字写进本项进展，供后续子项对比。

验收：预算断言在 make test 中生效（测试加入 UNIT_TEST_TARGETS）；基准三次运行同值；#86/#89 改动后能看到差值。
