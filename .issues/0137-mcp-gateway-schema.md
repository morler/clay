---
id: 137
title: MCP 固定 gateway 工具 schema
status: closed
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
        - 144
created: "2026-11-17"
updated: "2026-11-18"
closed: "2026-11-18"
---

改动：src/commands/mcp.c:29-49 + message.c:1444-1453 改成单个工具（action=list|call，server/tool/args 字段）；服务器列表进结果文本。
验证：mock MCP server 断言固定 schema 与调用结果。

## 2026-11-18 完成记录

改动：`src/commands/mcp.c`（新增 `mcp_tool_schema()`、`clay_mcp_gateway_tool()`、`mcp_list_result()`）、`src/commands/message.c`（注册循环换成单个 `mcp` 工具）、`include/clay/mcp.h`（公开 gateway 入口）、`src/commands/context.{c,h}`（删除 `mcp_bindings`）、`tests/test_mcp.c`、README、CHANGELOG。

**为什么这不是"少几个字节"的优化**：旧实现为每个被发现的 MCP 工具注册一个顶层工具，因此（1）字节成本等于服务器自己 `inputSchema` 的大小，clay 无法设上限；（2）服务器一连接，工具列表就变，prompt 前缀随之失效——每个请求都为此付费。新 schema 固定 303 B，与服务器数量和其 schema 大小无关。

行为变化（有意）：
- 模型不再看到 `mcp__<server>__<tool>`，改为 `mcp({action:"list"})` → `mcp({action:"call",server,tool,arguments})`。
- 工具参数 schema 从工具列表移到 `list` 结果里（`arguments_schema`），只有真正调用它的那次请求才为它付费。
- 基座提示对应改了一句（`mcp__<server>__<tool>` → `mcp` 工具的两种 action），净增 34 B。
- `ClayCommands.mcp_bindings` 及初始化/销毁/测试里的 6 处引用一并删除：gateway 不需要绑定记录。

预算：工具数 18 → 19，工具字节 7885 → 8188（+303 gateway），基座 9062 → 9056。合计 17244 B（预算 17400）；子代理固定成本 16703 B（预算 16800）。

验证：
- `make test` 全绿；44 个 UNIT_TEST_TARGETS 逐个运行 failures=0。
- `test_mcp` 覆盖：list 返回服务器与工具（含 `arguments_schema`）；call 成功返回 output；工具级错误返回 `ok:false` + 服务器原文；未知 server/tool 返回可恢复提示；schema 两次构建逐字节相同且不含 `mcp__`。
- **#139 的核心断言**：在同一 HOME 里配置并真实连接一个 fake 服务器后，新建会话的工具数必须等于未配置时的工具数（`without_servers == with_servers`）。
- 反证：把注册循环换回"每个工具一个"后，上面这条断言立即失败（exit 134）。这是关键——预算测试本身发现不了，因为它的 HOME 里没有配置任何服务器（这一点也已确认：反证时预算测试通过，说明 #90 需要真实配置才有意义）。
- 测试自清理：单次运行 0 残留（过程中修掉 `mkdtemp` 模板作用域问题与 `.clay` 下两个文件的清理）。

已知限制：MCP 工具的权限分类未变（仍走默认），`/mcp` 增删服务器仍是"下次发消息时连接"。
