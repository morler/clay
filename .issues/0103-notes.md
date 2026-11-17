---
id: 103
title: notes 块去重
status: wontfix
priority: high
labels:
    - type:task
    - area:cache
    - parent:80
    - step
relations:
    depends-on:
        - 102
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

改动：src/commands/context.c:1705-1716、message.c:542-550：仅当 notes 内容相对上一轮真的变化且本 turn 写入过才追加；same 内容不重复。
验证：连续 10 次 remember 后历史中 notes 系统消息 <= 1，且最后一份内容正确。

## Closed
同轮内 notes 只会同步一次、内容相同即跳过，验收条件今天已成立；真问题是跨轮整份累积，已并入 #80/#102
