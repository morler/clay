---
id: 112
title: 确定性红测试：多 round 累加导致提前折叠
status: closed
priority: high
labels:
    - type:bug
    - area:context
    - parent:83
    - step
relations:
    blocks:
        - 83
        - 114
        - 113
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

改动：tests/test_context_compact.c 新增用例：伪造 3 个 round 的 usage 累加超过预算但单次请求未超，断言当前实现错误折叠（红）。
验证：在 HEAD 失败。
