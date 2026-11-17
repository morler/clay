---
id: 95
title: -p --json 汇总输出
status: closed
priority: high
labels:
    - type:task
    - area:tokens
    - parent:77
    - step
relations:
    blocks:
        - 77
    depends-on:
        - 92
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

改动：src/cli/startup.c 增加 --json；src/main.c 结束时输出单行 JSON：requests/tool_calls/input_tokens/cached_tokens/output_tokens/duration_ms。
验证：tests/test_cli.c 断言 JSON 可解析、键齐全、非交互下无额外输出。
