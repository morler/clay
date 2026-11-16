---
# clay-dxtk
title: 细化：permit 排队及取消释放
status: in-progress
type: task
priority: normal
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-16T02:14:14Z
parent: clay-c8lo
blocking:
    - clay-2jq8
blocked_by:
    - clay-s4rb
---

范围：共享调度、src/commands/context.c、src/providers/openai.c、src/providers/openai_codex.c、tests/test_subagent.c。按 scope 获取和释放 permit，未知可靠额度默认每 scope 最多 1 个在途，排队有上限且可取消，明确公平和饥饿限制；验收：多 worker mock 验证在途上限、隔离、失败和取消释放。

## 本轮生产接入安全核查（未完成）已核实 clay_commands_run_completion 向 OpenAI、Codex、Grok 客户端传入 session scope；Grok completion 复用 OpenAI 请求循环。OpenAI 和 Codex 每轮实际 HTTP 请求及 429 重试分别在 src/providers/openai.c:741 和 src/providers/openai_codex.c:786；认证刷新和工具执行发生在请求返回后，应在进入重试等待、刷新和工具前归还 permit，每次重试重新获取，等待取消应返回现有取消结果；无 scope 需保持现状。模型列表及 OAuth 另有独立 HTTP 调用，其是否纳入 completion scope 须区分调用对象及身份，不能贸然套用同一 permit。当前阻碍：前置项 clay-s4rb 尚未完成；src/commands/context.c:563-630 的 OAuth 凭据保存并发释放、替换共享字符串及 Grok 客户端，而 :1061-1099 的 worker 并发读取和复制，同一 scope 的 HTTP permit 并不保护这些区域，也无法避免各客户端在获取 permit 前并发刷新一次性 refresh token；简单锁住完整 completion 则可能在后续 subagent 工具中自锁。src/commands/scope.c 的现有轮询许可无队列长度上限或公平保证，尚不满足本任务验收。流式请求内还会同步调用用户 token、reasoning、usage 回调，若回调重入同 scope completion 亦可能自锁，须先明确调用契约或设计处理。当前仅在请求周围加 acquire/release 不能安全声称主/subagent 和所有 provider 的完整生产接入，因此未修改 provider 或现有测试；保持 in-progress，等待先解决共享认证及取消前置项，再设计有界公平等待和可重复 provider HTTP mock 测试。验证：timeout 120s make test-subagent、make bin/test_retry_e2e 与 timeout 30s ./bin/test_retry_e2e 通过；这些现有测试不覆盖 scope 的生产请求准入、OAuth 并发或所有 provider。git diff --check 通过。未进行真实认证、真实服务、TSAN 或 Windows 验证。
