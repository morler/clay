---
id: 82
title: 为所有 OpenAI 兼容 provider 发送稳定 cache key，并评估 Anthropic 风格 cache_control
status: open
priority: medium
labels:
    - type:task
    - area:cache
    - parent:76
relations:
    blocks:
        - 76
    depends-on:
        - 77
        - 108
        - 109
        - 110
        - 111
created: "2026-11-17"
updated: "2026-11-17"
---

现状：只有 provider id 为 openai 时设置 prompt_cache_key（src/commands/context.c:1678-1680，src/providers/openai.c:588-590），grok 用 x-grok-conv-id 头（context.c:1680-1682），OpenRouter/Groq/DeepSeek/Mistral/Together/custom 完全依赖供应商自动缓存。源码中没有任何 cache_control 标记。

任务：
1. 对 OpenAI 兼容 provider 默认发送 prompt_cache_key=chat id（确认每个供应商不会因未知字段拒绝；必要时按 provider type 白名单）。
2. 评测 cache_control：在 system 消息与最后一条消息上打点（只作用于发送副本，历史保持逐字节不变），并以配置开关默认关闭，参考 killi openai.rs:94-121、decisions 48（实测第二次请求命中 4,387/4,607 tokens）。
3. 供应商返回 cache_creation/input_cache_read 类字段时解析进 ClayTokenUsage（anthropic 风格）。
验收：mock HTTP 测试断言请求体包含 cache key 且历史消息未被打点；ci 不依赖外网。
