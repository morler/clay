---
id: 147
title: 同一 HTTP 请求的两次 usage 块被记账两次
status: closed
priority: high
labels:
    - bug
created: "2026-11-18"
updated: "2026-11-18"
closed: "2026-11-18"
---

实测（2026-11-18，#76 真实命中测量）发现：本机 OpenAI 兼容代理对一次 /chat/completions 在 SSE 里发送两个带 usage 的 data 块（第一个带 choices[0].delta.role，最后一个带 finish_reason=stop），内容相同。

src/providers/openai.c 的 process_sse_data 每见到一个 usage 块就调用一次 on_usage/on_usage_details，而 src/commands/message.c:1394 的注释与设计是「一次 on_usage_details = 一次 provider 请求」，因此 chats/<id>/requests.jsonl 与 /usage 把一次请求记成两次：

- 单轮无工具会话（-p 只回答 ok）：requests=2，input=16532，两条记录 round=0/1 数值完全相同。
- 13 条记录对应 4 轮用户输入。

影响：#77 的逐请求记账、/usage 的 Requests 与 Input、命中率分母全部翻倍；#76 的真实命中测量不可信。

验收：
1. 一次 HTTP 响应无论出现几个 usage 块，只投递一次 usage 回调（取最后一个块，它是终块）。
2. 重试/取消/错误路径仍按「每次尝试一次记账」保持不变。
3. 新增 mock SSE 用例（同一响应两个 usage 块）断言 usage 回调恰好一次，且用量取终块数值；测试加入 Makefile 的 UNIT_TEST_TARGETS。

## 2026-11-18 修复

`src/providers/openai.c`：流状态多存一个待投递用量（`usage_pending`/`usage_seen`），`process_sse_data` 只记录不再回调；新增 `stream_state_finish`，在请求结束（成功、工具轮、取消、重试换状态、错误）时把用量投递一次，数值取响应里最后一个 usage 块。为此 run 循环里 10 处 `stream_state_free` 收敛为 `stream_state_finish`；重试路径先 finish 再 init，每个尝试仍然各记一次账。

`tests/test_provider_errors.c`：新增用例——一个 200 响应里两个 usage 块（100/7 + cached 40，120/9 + cached 80），断言 `on_usage_details` 恰好调用一次且数值取终块（120/9/cached 80）。该断言在旧实现上失败（`usage_reports==2`），修复后通过。

验证：`./bin/test_provider_errors` 通过；`make test`（34 组）通过；真实会话账本 `requests.jsonl` 的重复记录从「每次请求 2 条」变为 1 条，`/usage` 的 Requests 从 8 降到 5，与 5 次实际 HTTP 请求一致。
