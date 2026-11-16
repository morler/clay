---
id: 32
title: 修复 max 思考等级的非 OpenRouter 提示
status: closed
priority: medium
labels:
    - source:lif
    - type:bug
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

每次请求前明确提示非 OpenRouter 使用默认值；覆盖切换与恢复并完善文档。

需求调整：max 不再对非 OpenRouter 降级；按各请求格式显式发送，实际支持由服务端及模型决定。

## Summary of Changes

保留 OpenRouter reasoning.effort，Codex 使用 reasoning.effort，其他兼容接口（含 custom 与 Grok）发送 reasoning_effort；补充默认值、序列化测试和文档，make -j4 test 通过。

---

## 迁移来源（lif）

- 原 ID: `clay-7cyn`
- 原类型: `bug`
- 原状态: `completed`
- 原优先级: `normal`
- 原标签: 无
- 原父项: 无
- 原阻塞于: 无
- 原阻塞: 无
- 原创建时间: `2026-11-15T03:30:08Z`
- 原更新时间: `2026-11-15T03:37:35Z`
- 原文件路径: `.lif/clay-7cyn--修复-max-思考等级的非-openrouter-提示.md`
