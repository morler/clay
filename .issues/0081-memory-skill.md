---
id: 81
title: 系统提示中的指令、memory 与 skill 索引加上总量上限
status: closed
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
updated: "2026-11-18"
closed: "2026-11-18"
---

现状：src/commands/context.c:1530-1556 把 CLAY_SYSTEM_PROMPT_BASE（9.7KB）、memory 索引、skill 索引、项目指令（AGENTS.md/CLAY.md）无条件拼进系统提示；项目指令每个文件最多 4MB、向上 32 层（context.c:1341-1416），memory/skill 索引条数不设上限（src/memory.c:142-158、src/skill.c:373-391）。这些全部计入每个 chat 的固定前缀成本。killi 对照：memory 20 条/4KB/240 字符上限（memory.rs:15-22）；skill 只列名字（204 个 ≈1k vs 全描述 ≈15.7k，skills.rs:118-126）；system prompt + 工具 ≤3k tokens 由测试强制。

任务：
1. 为项目指令设总量上限（如 32KB）并在截断时写明 “N bytes cut, read the file for the rest”，按 root-first 保留靠前的层级。
2. memory 索引按条数与总字节设上限，超出时按最近更新保留并提示用 memory_read 查看其余。
3. skill 索引已经是 name: description（src/skill.c:373-391），只加条数上限，不要重写格式。
验收：构造超长 AGENTS.md/超多 skill 的测试，断言系统提示不超过预算且截断说明存在；测试加入 UNIT_TEST_TARGETS。
注：系统提示在 chat 创建时写进 journal 后冻结（include/clay/chat.h:26），本项只对新建 chat 生效；老 chat 需要时另加显式重建命令。

## 2026-11-18 收尾

三项上限已分别落地（#105 项目指令 32KB 且 root-first、#106 memory 20 条/4KB 与 skill 60 条/8KB 并带“其余在何处”提示）。缺口是验收要求的**组合**断言：本轮前只有三个子项各自测单段上限，没有任何测试断言“三者都顶到上限时系统提示总量仍有界”。
tests/test_token_budget.c 现在构造一个敌意 store：私有 HOME 里 40 条 memory（summary 300 字符）、70 个 skill、cwd 处于带 40KB AGENTS.md 的 repo 中，然后断言：

- 三段各自不超过自身上限；三段之和不超过总量预算；总量预算 52 KB 为独立硬上限（实测 50,856 B）。
- 实际下发的 system_prompt 字节数等于分段统计的 total，且三处截断说明（memory_list and memory_read / /skill lists the rest / bytes of project instructions cut）都在正文里。

同时把三个上限常量从 .c 内部提升到 include/clay/memory.h、clay/skill.h、src/commands/context.h，作为唯一来源供测试推导预算：改上限必须同时改测试可见的预算，否则断言失败。反证已验证：把 memory 上限改成 16KB，总量断言立即失败（exit 134）。

验证：make test 全绿；make test-token-budget 打印 base 9657 / memory 3928 / skills 4334 / instructions 32937，总计 50,856 B，预算 53,248 B。
