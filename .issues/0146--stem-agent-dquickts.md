---
id: 146
title: 实现 stem agent 的阶段编排（不引入 dquickts）
status: open
priority: medium
labels:
    - type:feature
    - area:tokens
relations:
    related-to:
        - 145
        - 76
created: "2026-11-18"
updated: "2026-11-18"
---

结论：可行，且不需要 dquickts。stem agent 的本体是「通用 system prompt + 每阶段用 user message 下发阶段指令」，靠跨阶段复用同一份 prompt cache 前缀省钱。clay 已有三块地基（详见 #145）：

1. system prompt 在 chat 生命周期内冻结（src/commands/context.c:1555-1557 注释）。
2. cache key 取 chat id，跨轮跨子代理稳定（src/commands/message.c:1614、src/commands/subagent.c:200）。
3. 每轮环境信息作为独立 system 消息追加在数组末尾，前缀只增不减（src/commands/context.c:1974-1991 append_context_block）。

改动：

- 加 phase 概念：chat 里记一个当前阶段名（默认为空）。切阶段时，往 conversation 末尾追加一条 user 消息，内容是本阶段指令；system prompt 一字不动。
- 最小形态：一条命令 /phase <name>，指令文本内置三段（explore / plan / execute）。不引入配置文件、模板引擎、阶段图。
- /plan 不要合并进阶段：/plan 现在是工具层开关（mode 标志，message.c:176/207/285/362、tasks.c:251、undo.c:143、subagent.c:276 拒绝写与变更类命令），阶段是提示层概念，不改变工具可用性。/plan 现有语义保持不变。

验证：

- 单测：切阶段前后，conversation 里 system 消息前缀逐字节不变，只有末尾多出一条 user 消息。
- 单测：cache key 仍是 chat id。
- 回归：make test-message test-context-compact test-tokens test-token-budget。

不做：

- 不引入 dquickts 或任何 JS 运行时：strip 后体积从 385KB 涨到约 1.2MB，与 README 的体积卖点冲突，且仓库无第三方 vendoring 传统（CODESTYLE.md:6）。若将来确需跨进程续跑，再单独评估。
- 不做阶段配置化、不做自动阶段切换、不做阶段间的工具裁剪。

关联：#145（本次评估）、#76（token 与 prompt cache）。
