---
id: 106
title: memory 与 skill 索引上限
status: closed
priority: medium
labels:
    - type:task
    - area:prompt
    - parent:81
    - step
relations:
    blocks:
        - 81
    depends-on:
        - 105
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

改动：src/memory.c:142-158、src/skill.c:373-391 设条数与总字节上限（对齐 killi 20 条 / 4KB / 单条 240 字符）；超限保留最近更新并提示 memory_read/memory_list。
验证：构造 50 条 memory 与 300 条 skill，断言系统提示段不超预算且提示可查看其余。
