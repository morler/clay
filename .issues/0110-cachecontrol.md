---
id: 110
title: cache_control 发送副本打点（配置开关默认关）
status: open
priority: medium
labels:
    - type:task
    - area:cache
    - parent:82
    - step
relations:
    blocks:
        - 82
    depends-on:
        - 109
created: "2026-11-17"
updated: "2026-11-17"
---

改动：src/providers/openai.c:580-630 在发送副本的 system 与最后一条 message 上标记；不改动 commands->conversation；新增配置项与 /config 说明。
验证：mock 断言打点只出现在发送副本；关闭开关时请求体与当前一致。
