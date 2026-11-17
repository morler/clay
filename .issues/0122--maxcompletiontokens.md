---
id: 122
title: 请求体发送 max_completion_tokens
status: open
priority: medium
labels:
    - type:task
    - area:context
    - parent:85
    - step
relations:
    blocks:
        - 85
    depends-on:
        - 121
created: "2026-11-17"
updated: "2026-11-17"
---

改动：src/providers/openai.c:580-630 增加 max_completion_tokens = min(配置, 窗口 - 估算输入 - 256)；Codex 同路径（openai_codex.c:732 附近）。
验证：mock 断言字段值；小窗口下不超窗口。
