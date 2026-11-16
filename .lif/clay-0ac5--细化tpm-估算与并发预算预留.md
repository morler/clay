---
# clay-0ac5
title: 细化：TPM 估算与并发预算预留
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-16T14:45:03Z
parent: clay-c8lo
blocking:
    - clay-tg6w
blocked_by:
    - clay-2jq8
---

范围：共享调度、src/providers/openai.c、src/providers/openai_codex.c、include/clay/providers/openai.h 及测试。按消息长度和可用 max output 保守估算并预留，仅可信头或明确配置提供 TPM 时启用硬预算，未知保持未知；验收：mock 覆盖并发预留、窗口滚动、缺少依据及已知本地预算不超发。

## Summary of Changes

tpm 配置项已加入 provider 文件（键名 tpm，缺省或 0 表示未知，不启用本地预算）。每次 HTTP 尝试在获取 scope permit 后预留估算：请求字节数的一半加 4096 个回复 token。tpm 为 0 时不预留；请求估算超过 tpm 时立即拒绝。测试：test_scope_tpm_budget 覆盖无 tpm 不记账、超限拒绝、取消不占用、窗口滚动后放行；test_provider_tpm_config 覆盖读写往返与缺省值。make test 通过。
