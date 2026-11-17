---
id: 141
title: summary 去重
status: open
priority: medium
labels:
    - type:task
    - area:subagent
    - parent:91
    - step
relations:
    blocks:
        - 91
created: "2026-11-17"
updated: "2026-11-17"
---

改动：src/commands/subagent.c:305-348 顶层 output 只保留一份 summary；results[] 里保留结构化字段与 transcript 路径。
验证：断言响应体中 summary 只出现一次。
