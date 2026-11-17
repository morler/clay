---
id: 96
title: 固定预算测试：系统提示与工具 schema
status: closed
priority: high
labels:
    - type:task
    - area:tokens
    - parent:78
    - step
relations:
    blocks:
        - 78
        - 97
        - 134
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

改动：新增 tests/test_token_budget.c：序列化 CLAY_SYSTEM_PROMPT_BASE + 工具 schema（src/commands/message.c:1361-1443），断言总字节 <= 常量上限，失败打印实测总值与逐项字节。

口径：killi 的上限是 12,000 字符（crates/killi/src/tests.rs:5-16，约 3k token）。这里一律写字节，并且上限先设在“现状 + 少量余量”，随 #89 收紧；不要在文档里写“3k 字符”。

验证：测试加入 Makefile 的 UNIT_TEST_TARGETS（Makefile:186）；改动 #89 时能看到差值。
