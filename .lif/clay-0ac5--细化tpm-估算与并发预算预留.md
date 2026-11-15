---
# clay-0ac5
title: 细化：TPM 估算与并发预算预留
status: todo
type: task
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-15T03:55:04Z
parent: clay-c8lo
blocking:
    - clay-tg6w
blocked_by:
    - clay-2jq8
---

范围：共享调度、src/providers/openai.c、src/providers/openai_codex.c、include/clay/providers/openai.h 及测试。按消息长度和可用 max output 保守估算并预留，仅可信头或明确配置提供 TPM 时启用硬预算，未知保持未知；验收：mock 覆盖并发预留、窗口滚动、缺少依据及已知本地预算不超发。
