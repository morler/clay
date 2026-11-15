---
# clay-dxtk
title: 细化：permit 排队及取消释放
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

范围：共享调度、src/commands/context.c、src/providers/openai.c、src/providers/openai_codex.c、tests/test_subagent.c。按 scope 获取和释放 permit，未知可靠额度默认每 scope 最多 1 个在途，排队有上限且可取消，明确公平和饥饿限制；验收：多 worker mock 验证在途上限、隔离、失败和取消释放。
