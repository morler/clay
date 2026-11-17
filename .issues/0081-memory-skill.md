---
id: 81
title: 系统提示中的指令、memory 与 skill 索引加上总量上限
status: open
priority: medium
labels:
    - type:task
    - area:cache
    - parent:76
relations:
    blocks:
        - 76
    depends-on:
        - 78
        - 105
        - 106
created: "2026-11-17"
updated: "2026-11-17"
---

现状：src/commands/context.c:1530-1556 把 CLAY_SYSTEM_PROMPT_BASE（9.7KB）、memory 索引、skill 索引、项目指令（AGENTS.md/CLAY.md）无条件拼进系统提示；项目指令每个文件最多 4MB、向上 32 层（context.c:1341-1416），memory/skill 索引条数不设上限（src/memory.c:142-158、src/skill.c:373-391）。这些全部计入每个 chat 的固定前缀成本。killi 对照：memory 20 条/4KB/240 字符上限（memory.rs:15-22）；skill 只列名字（204 个 ≈1k vs 全描述 ≈15.7k，skills.rs:118-126）；system prompt + 工具 ≤3k tokens 由测试强制。

任务：
1. 为项目指令设总量上限（如 32KB）并在截断时写明 “N bytes cut, read the file for the rest”，按 root-first 保留靠前的层级。
2. memory 索引按条数与总字节设上限，超出时按最近更新保留并提示用 memory_read 查看其余。
3. skill 索引已经是 name: description（src/skill.c:373-391），只加条数上限，不要重写格式。
验收：构造超长 AGENTS.md/超多 skill 的测试，断言系统提示不超过预算且截断说明存在；测试加入 UNIT_TEST_TARGETS。
注：系统提示在 chat 创建时写进 journal 后冻结（include/clay/chat.h:26），本项只对新建 chat 生效；老 chat 需要时另加显式重建命令。
