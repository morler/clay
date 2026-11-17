---
id: 82
title: 为所有 OpenAI 兼容 provider 发送稳定 cache key，并解析缓存用量字段
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
        - 108
        - 109
created: "2026-11-17"
updated: "2026-11-17"
---

现状：只有 provider id 为 openai 时设置 prompt_cache_key（src/commands/context.c:1678-1680，src/providers/openai.c:588-590），grok 用 x-grok-conv-id 头（context.c:1680-1682）；OpenRouter/Groq/DeepSeek/Mistral/Together/custom 完全依赖供应商自动缓存。Codex 已经发送 prompt_cache_key（context.c:1632 调 clay_openai_codex_set_prompt_cache_key，openai_codex.c:733-735），不要重复添加。

任务：
1. 对其余 OpenAI 兼容 provider 发送 prompt_cache_key=chat id；先按 #108 用 mock 确认每个供应商不会因未知字段拒绝，不兼容的记例外。
2. 供应商返回 cache_creation/input_cache_read 类字段时解析进 ClayTokenUsage（仅限实际返回这类字段的供应商）。

推迟（不在本项做）：cache_control 打点（原 #110）只对走 Claude 网关的用户有用，且默认关；clay 目前只有 grok/openai/openai_codex 三个 provider，没有 Anthropic 客户端。真正接入 Anthropic 协议时再单独立项。

验收：mock HTTP 测试断言请求体包含 cache key、历史消息未被打点，且 Codex 路径不重复设置；ci 不依赖外网。
