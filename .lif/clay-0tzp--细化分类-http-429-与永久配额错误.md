---
# clay-0tzp
title: 细化：分类 HTTP 429 与永久配额错误
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:55:03Z
updated_at: 2026-11-15T04:10:11Z
parent: clay-c8lo
blocking:
    - clay-onc0
blocked_by:
    - clay-5jth
---

范围：src/providers/openai.c、src/providers/openai_codex.c、include/clay/providers/openai.h 及 mock。分类 HTTP 429、永久配额/余额与其他错误，传递真实状态及原始错误；验收：离线覆盖各类错误、Codex 认证、Grok 和 OpenAI-compatible。

## Summary of Changes

新增结构化 HTTP 错误回调，保守识别 429 与显式配额错误，传播状态、原始体、白名单限流头及 Retry-After；覆盖 OpenAI-compatible、Codex 刷新认证和 Grok，保留原回调及既有认证刷新，不增加限流重试。离线 mock、make build、make test 和 git diff --check 通过。
