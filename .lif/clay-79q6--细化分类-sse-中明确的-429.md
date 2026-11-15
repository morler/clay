---
# clay-79q6
title: 细化：分类 SSE 中明确的 429
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:55:03Z
updated_at: 2026-11-15T04:14:36Z
parent: clay-c8lo
blocking:
    - clay-onc0
blocked_by:
    - clay-5jth
---

范围：src/sse.c、src/providers/openai.c、src/providers/openai_codex.c 和 SSE mock。仅按明确结构识别 HTTP 200 SSE error/response.failed 中的 429，保留真实状态与原错；验收：分片、多行、明确 429 和普通错误均覆盖，不误判任意文本。

## Summary of Changes

识别结构化 SSE 429，保留真实 HTTP 状态、错误事件、已有输出及未执行工具调用；不重试。字节分片、多行、HTTP 与 SSE、普通错误和各 provider 离线测试通过；make test、lif check 通过。
