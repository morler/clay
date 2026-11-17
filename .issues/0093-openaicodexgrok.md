---
id: 93
title: OpenAI/Codex/Grok 三条完成路径接上逐请求记录
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
        - 94
    depends-on:
        - 92
created: "2026-11-17"
updated: "2026-11-17"
---

改动：clay_commands_run_completion 每个 round 的 on_usage_details 调用 records 接口；compaction 与子代理请求带 kind（main/compact/subagent）；记录不得写入发给 provider 的 messages。
验证：mock HTTP 双侧，一次双工具轮次产生至少 3 条记录，字段与 provider 返回一致。
