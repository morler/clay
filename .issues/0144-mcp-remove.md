---
id: 144
title: /mcp remove 不切断已连接的服务器
status: closed
priority: low
labels:
    - type:bug
    - area:tools
    - parent:90
relations:
    blocks:
        - 90
    depends-on:
        - 137
created: "2026-11-18"
updated: "2026-11-18"
closed: "2026-11-18"
---

现状：src/commands/mcp.c:109-115 的 `/mcp remove <name>` 只调用 `clay_mcp_config_remove`（删配置文件），不影响本会话已连接的 `commands->mcp_servers`。因此被删的服务器在 `mcp({action:"list"})` 的结果里仍然出现，且仍可 `action=call` 成功调用，直到进程退出。

影响：用户以为已经移除，模型却还能继续调用它；提示语 'Removed %s.' 与实际状态不符。

改动：remove 时按名字在 `commands->mcp_servers` 里找到对应 server，`clay_mcp_disconnect` 并从数组移除；保留配置删除。

验证：test_mcp 增加用例——连接 fake 服务器、调用 remove、断言 (1) 服务器从 mcp_servers 消失，(2) `action=list` 不再列出它，(3) `action=call` 返回 unknown server 错误。反证：注释掉断连逻辑，三条断言都应失败。

边界：remove 一个本会话未连接（但已配置）的服务器应仍只删配置，不报错。

## 2026-11-18 完成

改动：`src/commands/mcp.c` 的 `/mcp remove` 分支在删配置前，先按名字在 `commands->mcp_servers` 里找已连接的 server，`clay_mcp_disconnect` 后 `clay_array_remove`。顺带把空名字的用法提示拆开，避免 `clay_mcp_config_remove("")`。

`clay_array_remove` 不释放元素，所以必须先 disconnect 再 remove，顺序不能反（`clay_mcp_disconnect` 会 free server 本身）。

验证（`tests/test_mcp.c`）：
1. 连接 fake 服务器 → `clay_cmd_mcp("remove fake")` → `mcp_servers.count == 0`
2. `action=list` 的结果不再含该服务器
3. `action=call` 返回 unknown server 错误
4. 边界：remove 一个已配置但本会话未连接的服务器，只删配置、数组不变、不报错

反证（三次独立运行，各自对应一条断言失败）：
- 删掉整个断连块 → `removing->mcp_servers.count == 0` 失败
- 断连但不从数组移除 → 同上（说明数组断言兜底）
- 放宽数组断言 + 不处理数组 → `!strstr(listed_text.data, "\"server\":\"fake\"")` 失败

`make test-mcp` 通过；单次运行 0 临时目录残留。
