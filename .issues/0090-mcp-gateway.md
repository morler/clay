---
id: 90
title: MCP 工具收敛为固定 gateway 工具，避免前缀抖动
status: open
priority: medium
labels:
    - type:task
    - area:tools
    - parent:76
relations:
    blocks:
        - 76
    depends-on:
        - 89
        - 137
        - 138
        - 139
created: "2026-11-17"
updated: "2026-11-17"
---

现状：MCP 工具在每次构建工具集时无条件追加，且带完整 server schema（src/commands/message.c:1444-1453、src/commands/mcp.c:29-49）；clay_commands_tools_build 每次 user 轮都会重建（message.c:1554），而 MCP 服务器数量/连接状态可能变化，工具列表变更会直接让前缀缓存归零（killi 在 MiMo 上实测，decisions 9）。killi 对照：MCP 通过一个固定 gateway 工具访问，工具列表会话内不变，/mcp 开关不删工具（mcp/gateway.rs:14-30；mcp.rs:1-4,34-62）。

任务：
1. 把 MCP 暴露为单个固定工具（list/call schema 固定），服务器与工具名在结果里出现。
2. 工具集在 chat 生命周期内只构建一次并缓存，除非模型/provider 切换（换 chat 才会换 cache key）。
3. 断言：同一会话两次请求的 tools JSON 逐字节相同。
验收：mock MCP server 测试；两次请求 tools 相同；make test 通过。
