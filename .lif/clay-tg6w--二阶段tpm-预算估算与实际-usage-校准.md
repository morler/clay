---
# clay-tg6w
title: 二阶段：TPM 预算估算与实际 usage 校准
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:52:01Z
updated_at: 2026-11-16T14:45:03Z
parent: clay-c8lo
blocked_by:
    - clay-2jq8
---

范围：src/providers/openai.c、src/providers/openai_codex.c、include/clay/providers/openai.h、共享调度与测试。发送前以消息长度及可用 max output 保守估算 token 并预留预算；仅在可信限流头或明确配置提供可用 TPM 依据时启用本地硬预算，未知维持未知，不声称精确配额。完成后使用 ClayTokenUsage 实际 input/output 校准并释放或结算；无 usage、失败、取消、重试和部分流按保守策略记账，防止双算、负数及永久占用，不假设缓存 token 计费规则。验收：mock 覆盖高估/低估、无 usage、并发预留、重试、取消和窗口滚动，已知本地预算不超发，仅表述为估计。

## Summary of Changes

本地 tpm 硬预算以估算预留、以 usage 结算，窗口 60 秒。仅在配置了 tpm 时启用，未知时不声称额度。估算为保守上界，只表述为估计，不声称精确配额；未检查缓存计费。等待预算期间持有 permit 并可取消。测试见 clay-0ac5 与 clay-3566。make test 通过。
