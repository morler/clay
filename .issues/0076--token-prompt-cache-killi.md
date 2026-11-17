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
B 前缀稳定：#79 Codex system 提升；#80 环境块/notes 块副本治理；#81 指令与索引总量上限；#82 cache key 与 cache_control 覆盖。
C 压缩与预算：#83 自动压缩触发语义；#84 /compact 重构；#85 按模型窗口的预算与输出预留。
D 工具与输出：#86 输出上限与续读提示；#87 edit/write 失败定位；#88 重复调用断路器；#89 提示与工具瘦身；#90 MCP 工具收敛。
E 子代理：#91 子代理记账与去重。

总验收：同一构建的基线数字可复现；环境/notes 变化后前缀仍命中（mock 断言请求前缀相等）；提示+工具预算由测试锁定；压缩后请求数下降且 resume 复用压缩结果；全部子项完成前本项保持 in-progress。
