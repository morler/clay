---
id: 113
title: 记录 last_prompt_tokens 与 accounted_messages
status: open
priority: high
labels:
    - type:bug
    - area:context
    - parent:83
    - step
relations:
    blocks:
        - 83
        - 115
        - 121
    depends-on:
        - 112
created: "2026-11-17"
updated: "2026-11-17"
---

改动：src/commands/context.h:237-244 增加状态；message.c:1604-1612 写回时只保存最后一次 round 的 prompt_tokens；修正注释。
验证：上述红测试转绿；单次超过预算仍折叠。
