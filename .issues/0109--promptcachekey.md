---
id: 109
title: 默认发送 prompt_cache_key 并按需白名单
status: wontfix
priority: medium
labels:
    - type:task
    - area:cache
    - parent:82
    - step
relations:
    blocks:
        - 110
    depends-on:
        - 108
created: "2026-11-17"
updated: "2026-11-20"
closed: "2026-11-20"
---

改动：src/commands/context.c:1678-1683 去掉只对 openai 的限制；src/providers/openai.c:588-590 已有字段；按 108 的结论配置例外。
验证：mock 断言请求体带 cache key=chat id；历史消息逐字节不变。

## Closed
merged into #82 as checklist (2026-11-20)
