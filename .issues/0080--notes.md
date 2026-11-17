---
id: 80
title: 环境块与 notes 块只保留最新一份，不累积副本
status: open
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
        - 103
        - 104
created: "2026-11-17"
updated: "2026-11-17"
---

现状：src/commands/context.c:1695-1717 append_context_block 每次内容变化都 push 一条完整新 system 消息，旧块不删除；environment_block/notes_block 槽位只记住最新文本。日期每天变、顶层文件列表每次新建/删除文件都变、每次 remember 都变，历史里于是长期累积多份过期 scratchpad 与环境快照。killi 对照：memory 只在会话开始拼进 prompt，变更走一条“去读当前列表”的提醒（decisions 80，lifecycle.rs:120-139）。

任务（在不破坏“只追加”的前提下）：
1. 评估两种方案并择一实现：(a) 节流——同一块在一个 turn 内最多追加一次，且只在语义变化（新增/删除文件、分支变化、日期变化）时追加；(b) 差异式——新块只携带与上一块的差异（如 “Top-level entries changed: +a.txt, -b.txt”），单调且恒定大小。
2. notes 块改为只在本轮确实写入后追加一次，避免同一内容重复。
3. 若采用 (a)，必须在压缩（#83/#84）重写历史后重新追加当前块，否则环境和 notes 会消失（compact.c 已有此漏洞）。
验收：连续 10 次 remember 后历史中 notes 系统消息 <= 1；顶层文件变化只产生一条差异块；测试断言消息数与内容。
