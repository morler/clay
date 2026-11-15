---
# clay-2jq8
title: 二阶段：自适应 RPM、并发与冷却调度
status: todo
type: task
created_at: 2026-11-15T03:51:54Z
updated_at: 2026-11-15T03:51:54Z
parent: clay-c8lo
blocked_by:
    - clay-s4rb
---

范围：共享调度、src/providers/openai.c、src/providers/openai_codex.c、src/commands/context.c、tests/test_subagent.c。按 scope 统一获取与释放 permit，主会话及子代理共用；依据可信响应头和 429 保守退避、冷却及渐进恢复，限制 RPM 与并发。默认无可靠额度时每 scope 最多 1 个在途，不凭空设 provider RPM；头有效且语义可信才采用，冲突从严。排队和等待有上限且可取消，失败和取消释放 permit，明确公平及饥饿问题；不推断精确额度或跨进程协调。验收：确定性 mock 多 worker 验证总在途、发送速率、429 冷却恢复、scope 隔离、排队取消和无泄漏。
