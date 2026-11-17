---
id: 101
title: 全 provider 回归与实测记录
status: closed
priority: high
labels:
    - type:task
    - area:cache
    - parent:79
    - step
relations:
    blocks:
        - 79
    depends-on:
        - 100
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

改动：make test-openai-codex test-subagent test-grok；用 mock 记录改动前后首个请求 prompt_tokens 与第二次命中；写入 #79。
验证：测试全绿；命中率提升有数字。
