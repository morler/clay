---
id: 135
title: 工具 schema 瘦身
status: open
priority: medium
labels:
    - type:task
    - area:prompt
    - parent:89
    - step
relations:
    blocks:
        - 89
    depends-on:
        - 134
created: "2026-11-17"
updated: "2026-11-17"
---

改动：src/commands/message.c:1361-1443 缩短描述、去掉与系统提示重复的内容、统一 required/optional 说明；不改参数语义。
验证：预算测试下降；工具调用相关测试全绿。
