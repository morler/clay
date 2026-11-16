---
id: 51
title: 细化：permit 排队及取消释放
status: closed
priority: medium
labels:
    - source:lif
    - type:task
    - parent:33
relations:
    blocks:
        - 39
    depends-on:
        - 38
        - 60
    related-to:
        - 33
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

范围：共享调度、src/commands/context.c、src/providers/openai.c、src/providers/openai_codex.c、tests/test_subagent.c。按 scope 获取和释放 permit，未知可靠额度默认每 scope 最多 1 个在途，排队有上限且可取消，明确公平和饥饿限制；验收：多 worker mock 验证在途上限、隔离、失败和取消释放。

## 本轮生产接入安全核查（未完成）已核实 clay_commands_run_completion 向 OpenAI、Codex、Grok 客户端传入 session scope；Grok completion 复用 OpenAI 请求循环。OpenAI 和 Codex 每轮实际 HTTP 请求及 429 重试分别在 src/providers/openai.c:741 和 src/providers/openai_codex.c:786；认证刷新和工具执行发生在请求返回后，应在进入重试等待、刷新和工具前归还 permit，每次重试重新获取，等待取消应返回现有取消结果；无 scope 需保持现状。模型列表及 OAuth 另有独立 HTTP 调用，其是否纳入 completion scope 须区分调用对象及身份，不能贸然套用同一 permit。当前阻碍：前置项 clay-s4rb 尚未完成；src/commands/context.c:563-630 的 OAuth 凭据保存并发释放、替换共享字符串及 Grok 客户端，而 :1061-1099 的 worker 并发读取和复制，同一 scope 的 HTTP permit 并不保护这些区域，也无法避免各客户端在获取 permit 前并发刷新一次性 refresh token；简单锁住完整 completion 则可能在后续 subagent 工具中自锁。src/commands/scope.c 的现有轮询许可无队列长度上限或公平保证，尚不满足本任务验收。流式请求内还会同步调用用户 token、reasoning、usage 回调，若回调重入同 scope completion 亦可能自锁，须先明确调用契约或设计处理。当前仅在请求周围加 acquire/release 不能安全声称主/subagent 和所有 provider 的完整生产接入，因此未修改 provider 或现有测试；保持 in-progress，等待先解决共享认证及取消前置项，再设计有界公平等待和可重复 provider HTTP mock 测试。验证：timeout 120s make test-subagent、make bin/test_retry_e2e 与 timeout 30s ./bin/test_retry_e2e 通过；这些现有测试不覆盖 scope 的生产请求准入、OAuth 并发或所有 provider。git diff --check 通过。未进行真实认证、真实服务、TSAN 或 Windows 验证。

## Summary of Changes

permit 排队有界：每 scope 最多 8 个等待者、单次等待最长 600 秒；等待期间可取消并返回取消结果；请求失败、取消和超限路径均释放或不占用 permit；每次 HTTP 尝试重新获取。已知限制：唤醒非 FIFO，不保证公平与防饥饿；permit 只包住单次 HTTP 尝试，OAuth 刷新与模型列表不受其约束（OAuth 并发由 clay-mcxf 跟踪）；同一 scope 内请求串行，因此主会话与子代理使用同一模型时不再并行，这是默认策略的有意改变；流式回调若重入同一 scope 的 completion 会等到上限后失败（当前无此调用方）。未运行 TSAN 或真实服务验证。make test 通过。

---

## 迁移来源（lif）

- 原 ID: `clay-dxtk`
- 原类型: `task`
- 原状态: `completed`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-c8lo` → #33
- 原阻塞于: `clay-s4rb` → #38
- 原阻塞: `clay-2jq8` → #39
- 原创建时间: `2026-11-15T03:55:04Z`
- 原更新时间: `2026-11-16T14:29:54Z`
- 原文件路径: `.lif/clay-dxtk--细化permit-排队及取消释放.md`
