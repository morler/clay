---
# clay-2jq8
title: 二阶段：自适应 RPM、并发与冷却调度
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:51:54Z
updated_at: 2026-11-16T14:38:56Z
parent: clay-c8lo
blocked_by:
    - clay-s4rb
---

范围：共享调度、src/providers/openai.c、src/providers/openai_codex.c、src/commands/context.c、tests/test_subagent.c。按 scope 统一获取与释放 permit，主会话及子代理共用；依据可信响应头和 429 保守退避、冷却及渐进恢复，限制 RPM 与并发。默认无可靠额度时每 scope 最多 1 个在途，不凭空设 provider RPM；头有效且语义可信才采用，冲突从严。排队和等待有上限且可取消，失败和取消释放 permit，明确公平及饥饿问题；不推断精确额度或跨进程协调。验收：确定性 mock 多 worker 验证总在途、发送速率、429 冷却恢复、scope 隔离、排队取消和无泄漏。

## Summary of Changes

OpenAI、Codex、Grok 的每次 HTTP 尝试统一经过 scope permit 与节奏（见 clay-psns）：默认每 scope 1 个在途，429 冷却与渐进恢复生效，排队有上限且可取消，取消与失败路径释放 permit，既有 scope 与 race 测试覆盖无泄漏。限制：串行与节奏只作用于同一 scope（endpoint 加模型），不设跨 scope 的总在途上限，因为没有可信额度来源；RPM 不从响应头推导；未设 provider 级 RPM 默认值。make test 通过。
