---
# clay-d3og
title: 增加 max 思考等级
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:26:42Z
updated_at: 2026-11-15T03:39:05Z
---

实现 max 等级并保持 provider API 兼容，增加测试。

## Summary of Changes

增加 Max 选项及持久化；OpenRouter 使用 reasoning.effort，其余 provider 使用默认值；新增序列化、兼容性测试。make build test 通过。

后续调整：根据 clay-7cyn，非 OpenRouter provider 不再降级，max 按各 provider 请求格式发送。
