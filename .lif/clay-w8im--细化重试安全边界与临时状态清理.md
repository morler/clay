---
# clay-w8im
title: 细化：重试安全边界与临时状态清理
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:55:03Z
updated_at: 2026-11-15T04:23:38Z
parent: clay-c8lo
blocking:
    - clay-h8nw
blocked_by:
    - clay-onc0
---

范围：src/providers/openai.c、src/providers/openai_codex.c、src/commands/message.c、相关头与 mock。仅当前请求无可见内容或 reasoning 且未执行工具时重试，清理工具参数碎片，保留此前工具结果，不重放整轮；验收：有输出或副作用时保留部分结果且不重试，回调不重复、工具恰好一次。

## Summary of Changes

建立逐请求可见输出、工具执行和回调副作用的重放安全边界；丢弃失败请求的工具碎片、保留部分输出及既有工具结果；阻止不安全的认证重放。mock 与相关回归测试通过；429 退避重试仍由后续任务实现。
