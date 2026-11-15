---
# clay-3566
title: 细化：usage 结算与异常记账
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

范围：共享调度、provider 入口、include/clay/providers/openai.h 及测试。以 ClayTokenUsage input/output 校准并结算；无 usage、失败、取消、重试与部分流保守处理，不假设缓存计费。验收：测试高估/低估、无 usage、并发、重试和取消，无双算、负数或永久占用。
