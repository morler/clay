---
id: 92
title: usage 逐请求记录结构与写入点
status: open
priority: high
labels:
    - type:task
    - area:tokens
    - parent:77
    - step
relations:
    blocks:
        - 77
        - 93
        - 95
created: "2026-11-17"
updated: "2026-11-17"
---

现状：usage 只在每轮结束汇总（src/commands/message.c:1342-1350、1604-1612），没有逐请求记录。

改动：src/commands/context.h 新增 ClayRequestUsage（provider id、model、round、prompt、cached、completion、预算、毫秒）；src/chat.c 旁新增 clay_chat_append_request_usage()，追加到 ~/.clay/chats/<id>/requests.jsonl；写盘失败只告警、不影响本轮。每行必须一次 write() 写完（O_APPEND），否则并发子代理会写出交错的行，/usage 解析失败。

验证：新增 tests/test_request_log.c，写 3 条后逐行解析，字段齐全；并发写入 2 个线程各 50 行后每行都能解析；测试加入 UNIT_TEST_TARGETS。
