---
id: 137
title: MCP 固定 gateway 工具 schema
status: open
priority: low
labels:
    - type:task
    - area:tools
    - parent:90
    - step
relations:
    blocks:
        - 90
        - 138
        - 139
created: "2026-11-17"
updated: "2026-11-17"
---

改动：src/commands/mcp.c:29-49 + message.c:1444-1453 改成单个工具（action=list|call，server/tool/args 字段）；服务器列表进结果文本。
验证：mock MCP server 断言固定 schema 与调用结果。
