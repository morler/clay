---
id: 111
title: Anthropic 风格缓存用量字段解析
status: wontfix
priority: medium
labels:
    - type:task
    - area:cache
    - parent:82
    - step
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

改动：src/providers/openai.c:317-336 解析 cache_creation_input_tokens / cache_read_input_tokens / input_cache_read 等，写入 ClayTokenUsage 并进入 #77 的记录。
验证：tests/test_grok.c:116-133 同款解析测试扩展到新字段，未知字段不影响。

## Closed
clay 只有 grok/openai/openai_codex 三个 provider，没有 Anthropic 客户端；等真正接入 Anthropic 协议时再单独立项
