---
# clay-pwho
title: 细化：端到端并发离线压力回归
status: todo
type: task
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-15T03:55:04Z
parent: clay-c8lo
blocking:
    - clay-8mkl
blocked_by:
    - clay-tg6w
---

范围：tests/test_subagent.c、tests/test_message.c、provider mock fixtures、Makefile。模拟主会话及最多四子代理、同异 provider 和模型、429 恢复、取消、工具副作用、缺头及错误；验收：相关 make test-* 离线可重复，检查线程安全和释放，记录 Windows 构建或不可用原因，不使用真实密钥或外网。
