---
id: 94
title: /usage 命令
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
    depends-on:
        - 93
created: "2026-11-17"
updated: "2026-11-17"
---

改动：src/commands/register.c 注册 /usage；汇总本会话 requests.jsonl：请求数、工具调用数、输入、缓存比例、输出、连续未命中次数（达到 3 次时提示原因排查）。
验证：tests/test_usage_command.c 用固定 jsonl 断言输出文本；无记录时提示尚未请求。
