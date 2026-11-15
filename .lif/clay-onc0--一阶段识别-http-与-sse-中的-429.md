---
# clay-onc0
title: 一阶段：识别 HTTP 与 SSE 中的 429
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:51:26Z
updated_at: 2026-11-15T04:20:13Z
parent: clay-c8lo
blocked_by:
    - clay-5jth
---

范围：src/providers/openai.c、src/providers/openai_codex.c、src/sse.c、include/clay/providers/openai.h；Grok 代理共用 OpenAI 路径。区分 HTTP 429 与 HTTP 200 SSE error/response.failed 中明确 429，不将一般 error 或任意文本误判为 429；保留真实状态、原始 provider 错误及非 429/认证刷新行为。验收：mock 覆盖分片、多行 SSE、HTTP 429、普通错误、Codex 认证和 Grok/OpenAI-compatible 路径。

## Summary of Changes

独立复核 HTTP/SSE 429、配额与原始状态，修正错误输出落盘的后缀误判；补充真实工具防执行、Codex/Grok、SSE 后传输失败及日志去重回归测试。专项测试、make test、make build、lif check 与 git diff --check 通过。
