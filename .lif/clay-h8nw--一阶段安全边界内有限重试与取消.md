---
# clay-h8nw
title: 一阶段：安全边界内有限重试与取消
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:51:33Z
updated_at: 2026-11-15T04:37:22Z
parent: clay-c8lo
blocked_by:
    - clay-onc0
---

范围：src/providers/openai.c、src/providers/openai_codex.c、src/commands/message.c 和相关 provider 头。仅在本次请求尚未产生可见内容或 reasoning、尚未执行工具时重试明确 429；工具参数碎片本身不算已执行，但重试须丢弃临时状态，不重复追加消息和调用工具。只重试当前请求而非整轮，保留此前已完成工具结果；已有输出或副作用时保留部分结果并报错。采用容器默认有限次数、指数退避抖动和预算；Retry-After 为下界，超预算立即报原错；请求与等待通过 should_abort 取消，单调时钟测预算，墙钟仅用于 HTTP 日期换算。耗尽保留最后 429 状态和 body。验收：mock 验证次数、预算、抖动、取消、工具恰好一次、有输出不重试、消息回调不重复、原错保留以及非 429/OAuth 无回归。

## Summary of Changes

独立验收安全重试、限额与取消；修复普通 SSE 错误被忽略或后续 429 覆盖、Codex 空 reasoning 回调重放、多轮部分输出重复保存；补充错误分类、回调顺序、空 Retry-After、请求取消和持久化回归。make test、make build、lif check 和 git diff --check 通过。
