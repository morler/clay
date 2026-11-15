---
# clay-usy4
title: 细化：reasoning 与现有 provider 兼容回归
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-15T04:42:59Z
parent: clay-c8lo
blocking:
    - clay-7609
blocked_by:
    - clay-h8nw
---

范围：tests/test_reasoning.c、tests/test_reasoning_codex.c、tests/test_message.c、Makefile。补齐 reasoning、工具副作用、回调不重复及 OAuth 非 429 回归；验收：相关 make test-* 和原 provider 测试离线可重复，记录平台差异及未覆盖项。

## Summary of Changes

在 tests/test_provider_errors.c 的离线 HTTP mock 中补充 Codex 非空 reasoning 无回调时禁止重试、Grok reasoning 禁止重试、三种 provider 正常分片响应，以及 Codex/Grok 非 429 与 OAuth 刷新失败回归；结合已有测试覆盖工具和回调不重复。专项测试及 make test 通过；test-openai 使用仅监听 127.0.0.1 的临时 mock。Linux x86_64 验证；未验证 Windows 或真实服务。全量测试包含既有 grep/repo_map race baseline 报告，未在本项修复。
