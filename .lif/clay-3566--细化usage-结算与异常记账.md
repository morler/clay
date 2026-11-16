---
# clay-3566
title: 细化：usage 结算与异常记账
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

范围：共享调度、provider 入口、include/clay/providers/openai.h 及测试。以 ClayTokenUsage input/output 校准并结算；无 usage、失败、取消、重试与部分流保守处理，不假设缓存计费。验收：测试高估/低估、无 usage、并发、重试和取消，无双算、负数或永久占用。

## Summary of Changes

usage 结算已接入 OpenAI 与 Codex 的流式解析：每次尝试结束时用 input + output 替换估算，即使高于估算；无 usage、失败、取消、重试和部分流都保留估算，不会少记。每次尝试各自预留并结算一次，不会双算；负数 usage 视为未知。结算后再释放 permit，预留条目在结算前不会过期。缓存 token 不做特殊计费。测试：test_scope_tpm_budget 覆盖低于估算释放、无 usage 保留估算、取消与超限不记账；test_provider_tpm_window 用 mock 上报 17000 token，验证第二次请求等待窗口滚动。make test 通过。
