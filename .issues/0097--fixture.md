---
id: 97
title: 离线请求体基准脚本与固定 fixture
status: closed
priority: high
labels:
    - type:task
    - area:tokens
    - parent:78
    - step
relations:
    blocks:
        - 78
        - 98
        - 114
    depends-on:
        - 96
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

改动：scripts/tokens.sh + tests/fixtures/token_conversation.json（含 read/bash/grep 结果各一条）；输出序列化后请求体字节数、按 4 字节/token 与 2 字符/token 两种估算；不联网。
验证：连续三次运行输出同值；可直接对比 #86/#89 前后。
