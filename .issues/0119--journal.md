---
id: 119
title: 压缩结果写入 journal 并可重放
status: open
priority: high
labels:
    - type:task
    - area:context
    - parent:84
    - step
relations:
    blocks:
        - 84
    depends-on:
        - 117
        - 118
created: "2026-11-17"
updated: "2026-11-17"
---

改动：src/commands/compact.c:159-160 增加持久化（复用 chat.c 的 turn 记录或新增 compaction 记录类型）；/resume 加载时复用压缩结果。
验证：压缩后退出再 /resume，历史与压缩后一致，环境/notes 块在下一轮重新出现。
