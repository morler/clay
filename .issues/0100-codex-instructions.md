---
id: 100
title: Codex 只把冻结系统提示放进 instructions
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
        - 101
    depends-on:
        - 99
created: "2026-11-17"
updated: "2026-11-17"
---

改动：src/providers/openai_codex.c:645-659 只取 index 0 的 system 进 instructions；其余 system 按原顺序转成要求的 input 项（若协议不接受 system，则包成尾部 developer/user 文本）。
验证：99 的红测试转绿；同一历史两次序列化逐字节一致。
