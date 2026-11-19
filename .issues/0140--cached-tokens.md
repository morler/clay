---
id: 140
title: 子代理 cached tokens 记账
status: closed
priority: medium
labels:
    - type:task
    - area:subagent
    - parent:91
    - step
relations:
    blocks:
        - 91
        - 142
created: "2026-11-17"
updated: "2026-11-19"
closed: "2026-11-19"
---

改动：src/commands/subagent.c:75-79、345 计入 cached；字段与 #92 的记录一致；不要只加 input/output 让父会话缓存比例失真。

验证：双分支测试断言父会话 cached 总数正确；测试加入 UNIT_TEST_TARGETS。
