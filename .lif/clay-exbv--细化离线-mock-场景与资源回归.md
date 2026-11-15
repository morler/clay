---
# clay-exbv
title: 细化：离线 mock 场景与资源回归
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-15T04:40:22Z
parent: clay-c8lo
blocking:
    - clay-7609
blocked_by:
    - clay-h8nw
---

范围：tests/test_message.c、tests/test_openai.c、tests/test_openai_codex.c、tests/test_grok.c 和必要 mock fixture。注入时钟、等待与随机源或等价 seam，补齐 HTTP/SSE 分片、取消、错误、可选回调及资源释放回归；验收：测试无需网络凭据且重复运行，覆盖失败和取消释放。

## Summary of Changes

新增本地 HTTP 端到端重试测试：多轮 429、工具恰好一次、成功/取消/失败和每场景 fd 数；补充超长 Retry-After 丢弃边界。既有 provider mock 覆盖 OpenAI、Codex、Grok 的分片、错误与取消。make test、重复运行、Linux ASan/LSan 和 git diff --check 通过。
