---
id: 76
title: 优化 token 消耗与 prompt cache 命中（对标 killi）
status: open
priority: high
labels:
    - type:feature
    - area:tokens
relations:
    depends-on:
        - 77
        - 78
        - 79
        - 80
        - 81
        - 82
        - 83
        - 84
        - 85
        - 86
        - 87
        - 88
        - 89
        - 90
        - 91
created: "2026-11-17"
updated: "2026-11-17"
---

目标：以 killi（/var/home/msc802/code/harness/killi）的 prompt-cache 设计为参照，降低 clay 的输入 token 并让缓存命中可测、可回归。killi 侧证据：system prompt + 全部工具 11,714 字符 ≈2,900 tokens（docs/decisions.md:481，tests.rs:5-16 强制预算）；历史只追加、工具列表会话内冻结（decisions 9/26，tools/mod.rs:1-2）；压缩阈值按窗口 60%、85% 上限、输出预留 ≤limit/4、20% 增长门槛、保留用户请求（compact.rs:4-67）；usage 逐请求记录并带 model/context_window（agent/request.rs:182-214），scripts/sessions.py 统计缓存率与 cache-miss streak。

clay 现状（HEAD 080b070）：系统提示基座 9.7KB + 18 个工具 ~8.5KB ≈4.5k tokens（context.c:13-231、message.c:1361-1443）；prompt_cache_key 只对 provider openai 生效（context.c:1678-1683）；Codex 把全部 system 提升到 instructions，环境/notes 一变即清空前缀（openai_codex.c:651-659）；自动压缩阈值用的是本轮各 round 累加值而非已计费上下文（message.c:1342-1350、1604-1612 对比 context.h:237-241）；/compact 不落 journal、摘要请求携带原始工具参数（compact.c:59-69、145-160）；无逐请求 usage 记录、无 /usage、无基准脚本。

阶段与子项：
A 度量：#77 逐请求 usage 与缓存归因 + /usage + -p --json；#78 离线基线基准与预算测试。
B 前缀稳定：#79 Codex system 提升；#80 环境块/notes 块副本治理；#81 指令与索引总量上限；#82 cache key 覆盖（cache_control 推迟）。
C 压缩与预算：#83 自动压缩触发语义；#84 /compact 重构；#85 预算单位与配置语义（折叠 + 提醒，不移植窗口百分比）。
D 工具与输出：#86 输出上限与续读提示；#87 edit/write 失败定位；#88 重复调用断路器（等 #77 数据）；#89 提示与工具瘦身；#90 MCP 工具收敛（先量字节）。
E 子代理：#91 子代理记账与去重。

执行顺序（ponytail 评审后重排）：#78/#96/#97 → #92-95(#77) → #99-101(#79) → #112-115(#83) → #116-119(#84) → #102/#104(#80) → #105-107(#81) → #120-122(#85) → #123-129(#86/#87) → #89 → #90 → #91；#88 与 #130-132 仅在 #77 的重复率数据支持时才做，否则关闭。

总验收（拆成两条，避免把离线断言当成真实命中）：
1. 离线：同一构建下请求前缀逐字节相等（环境/notes 变化后仍相等），提示 + 工具的字节预算由 make test 中的测试锁定。
2. 真实运行：一次手动会话记录命中数字（provider、模型、日期、前后对比），写进本项进展；不做 CI 断言。

约束与结论（评审确认）：
- 每个新增测试必须加入 Makefile 的 UNIT_TEST_TARGETS（Makefile:186），否则验收写着「make test 生效」也会假通过。
- 系统提示在 chat 创建时写进 journal 后冻结（include/clay/chat.h:26、src/chat.c:315）：#81/#89/#105 只对新建 chat 生效，老 chat 用 /resume 看不到变化；需要时应另加显式重建命令，不要在本项偷偷重建。
- 字节与 token 的换算只写一次：killi 的预算断言是 12,000 字符（crates/killi/src/tests.rs:5-16），约 3k token；不要写成「3k 字符」。
- 引用 killi 时写全路径（crates/killi/src/agent/compact.rs 等）；docs/decisions.md 的编号有重复，引用同时给日期与标题。
- 估算函数复用 src/commands/scope.c 的「请求字节 ÷ 2 + 输出预留」（scope.c:343-346 注释），不要再造第三种估算。

全部子项完成前本项保持 in-progress。
