---
id: 116
title: 摘要 transcript 的工具参数截断
status: open
priority: high
labels:
    - type:task
    - area:context
    - parent:84
    - step
relations:
    blocks:
        - 84
        - 117
        - 118
created: "2026-11-17"
updated: "2026-11-17"
---

改动：src/commands/compact.c:59-69 工具调用只保留 name + 关键字段（path/pattern/command 首行）+ 参数字节数；write/edit 只保留路径与行数统计，正文不进摘要请求。
验证：tests/test_compact.c 断言 write 内容不出现、字节数下降。
