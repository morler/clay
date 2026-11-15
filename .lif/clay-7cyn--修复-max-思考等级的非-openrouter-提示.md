---
# clay-7cyn
title: 修复 max 思考等级的非 OpenRouter 提示
status: completed
type: bug
priority: normal
created_at: 2026-11-15T03:30:08Z
updated_at: 2026-11-15T03:37:35Z
---

每次请求前明确提示非 OpenRouter 使用默认值；覆盖切换与恢复并完善文档。

需求调整：max 不再对非 OpenRouter 降级；按各请求格式显式发送，实际支持由服务端及模型决定。

## Summary of Changes

保留 OpenRouter reasoning.effort，Codex 使用 reasoning.effort，其他兼容接口（含 custom 与 Grok）发送 reasoning_effort；补充默认值、序列化测试和文档，make -j4 test 通过。
