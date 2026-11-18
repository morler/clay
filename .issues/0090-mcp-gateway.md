---
id: 90
title: MCP 工具字节成本先量后改；必要时收敛为固定 gateway 工具
status: open
priority: low
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
        - 144
created: "2026-11-17"
updated: "2026-11-18"
---

现状：MCP 工具在每次构建工具集时追加，且带完整 server schema（src/commands/message.c:1444-1453、src/commands/mcp.c:29-49）。但 MCP 服务器在会话里只连接一次，之后调用是空操作（src/commands/context.h:245-246），工具列表只在 /mcp 增删时才变，所以本项原题的“前缀抖动”不成立。

真正的成本是每个 MCP 工具都带一份完整 schema（工具参数说明）占字节，且随服务器数量线性增长。

任务：
1. 先在 #78 的基准里量出 MCP 工具占的字节数（0 个 / 1 个 / 3 个服务器的 fixture），写进本项。
2. 只有量表明确显示占比高时才做 gateway：一个固定工具，给 server 列出工具，给 server + tool 就调用（形状照 crates/killi/src/mcp/gateway.rs:14-30，比 action=list|call 少一个字段）。
3. 无论是否做 gateway，工具集在 chat 生命周期内只构建一次并缓存，除非模型/provider 切换。

验收：字节量表的数字写进本项；若做 gateway，mock MCP server 测试通过且同一会话两次请求的 tools JSON 逐字节相同；若不做，写明触发条件（多少字节占比）后关闭 #137-139。
