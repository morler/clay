---
# clay-k2gt
title: 修复一阶段 429 审查缺陷
status: completed
type: bug
priority: normal
created_at: 2026-11-15T04:51:04Z
updated_at: 2026-11-15T04:54:48Z
parent: clay-c8lo
---

修复 Grok 401 回调与 SSE 429 显示并增加回归。

## Summary of Changes

Grok 401 安全刷新重试仅报告最终错误；CLI 区分 SSE 429 与实际 HTTP 状态且不输出敏感正文，增加离线回归。make test、test-pty、test-fs-fallback 通过。
