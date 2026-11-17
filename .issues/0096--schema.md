---
id: 96
title: 固定预算测试：系统提示与工具 schema
status: open
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
---

改动：新增 tests/test_token_budget.c：序列化 CLAY_SYSTEM_PROMPT_BASE + 18 个工具 schema（src/commands/message.c:1361-1443），断言总字节 <= 常量上限，失败打印实测与逐项字节；先按现状基线设定，留收紧空间。
验证：make test 中生效；改动 #89 时能看到差值。
