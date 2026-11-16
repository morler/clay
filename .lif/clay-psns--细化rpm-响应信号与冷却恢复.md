---
# clay-psns
title: 细化：RPM 响应信号与冷却恢复
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-16T14:38:56Z
parent: clay-c8lo
blocking:
    - clay-2jq8
blocked_by:
    - clay-s4rb
---

范围：共享调度、provider 入口及 tests/test_subagent.c。仅采用语义可信头，冲突从严，结合 429 保守控制 RPM、冷却和渐进恢复；未知时不虚构 RPM。验收：确定性 mock 检查发送速率、头缺失/冲突、429 冷却恢复及 scope 隔离。

## Summary of Changes

429 冷却与节奏已接入 scope：Retry-After 缺失用 1 秒基础冷却；有效值按 30 秒上限采用；头存在但无效、重复或被丢弃视为冲突，取 30 秒严格上限。每次 429 使准入间隔翻倍（上限 30 秒），每次 2xx 减半直至归零。准入前检查冷却与间隔，等待可取消且不占用 permit。RPM 不从响应头推导（没有语义已验证的头），未知时不虚构。测试：scope 级 test_scope_cooldown_and_pacing 覆盖发送间隔不少于 900 毫秒、头缺失/有效/冲突/丢弃、永久配额不冷却、恢复减半、跨 scope 隔离、冷却中取消；provider 级 test_scope_rate_limit_http 用 mock 首次 429 验证重试前 pace 为 1000、成功后归零。make test 通过。
