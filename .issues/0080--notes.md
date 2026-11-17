---
id: 80
title: 环境块与 notes 块只保留最新一份，不累积副本
status: closed
priority: high
labels:
    - type:task
    - area:cache
    - parent:76
relations:
    blocks:
        - 76
    depends-on:
        - 78
        - 102
        - 104
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

现状：src/commands/context.c:1695-1717 append_context_block 每次内容变化都 push 一条完整新 system 消息，旧块不删除；environment_block/notes_block 槽位只记住最新文本。日期每天变、顶层文件列表每次新建/删除文件都变、每次 remember 都变，历史里于是长期累积多份过期 scratchpad 与环境快照。killi 对照：memory 只在会话开始拼进 prompt，变更走一条“去读当前列表”的提醒（decisions 80，lifecycle.rs:120-139）。

任务（在不破坏“只追加”的前提下）：
1. 择一实现：(a) 节流——同一块在一个 turn 内最多追加一次，且只在语义变化（新增/删除文件、分支变化、日期变化）时追加；(b) 差异式——新块只携带与上一块的差异（如 “Top-level entries changed: +a.txt, -b.txt”），单调且恒定大小。
2. 跨轮不累积副本是本项真正的问题：同轮内内容相同已会被跳过（context.c:1697），所以验收不能只写“同一轮 10 次 remember”；要测跨 10 个用户轮每轮 remember 一次后的条数。
3. 压缩后重发当前块（#104）必须与 #83/#84 的落地一起做：压缩会重写历史，之后槽位仍持旧值，环境和 notes 会永久消失。
验收：跨 10 个用户轮、每轮 remember 一次后历史中 notes 系统消息 <= 1（或按阶段增长且有总量上限）；顶层文件变化只产生一条差异块；测试断言消息数与内容并加入 UNIT_TEST_TARGETS。
