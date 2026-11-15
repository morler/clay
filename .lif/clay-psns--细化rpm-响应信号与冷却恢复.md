---
# clay-psns
title: 细化：RPM 响应信号与冷却恢复
status: todo
type: task
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-15T03:55:04Z
parent: clay-c8lo
blocking:
    - clay-2jq8
blocked_by:
    - clay-s4rb
---

范围：共享调度、provider 入口及 tests/test_subagent.c。仅采用语义可信头，冲突从严，结合 429 保守控制 RPM、冷却和渐进恢复；未知时不虚构 RPM。验收：确定性 mock 检查发送速率、头缺失/冲突、429 冷却恢复及 scope 隔离。
