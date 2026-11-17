---
id: 99
title: Codex 解包测试先行：instructions 与 input 分离断言
status: open
priority: high
labels:
    - type:task
    - area:cache
    - parent:79
    - step
relations:
    blocks:
        - 79
        - 100
created: "2026-11-17"
updated: "2026-11-17"
---

改动：tests/test_openai_codex.c 增加红测试——给定 system + 两条尾部 context 块，断言 instructions 只含冻结提示、后续块在 input 里按序出现；当前实现下失败。
验证：该测试在当前 HEAD 失败，作为 #99 的前提。
