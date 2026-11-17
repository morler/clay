---
id: 84
title: 重构 /compact：保留用户原话、压缩摘要请求、落 journal 并恢复 context 块
status: open
priority: high
labels:
    - type:task
    - area:context
    - parent:76
relations:
    blocks:
        - 76
    depends-on:
        - 83
        - 116
        - 117
        - 118
        - 119
created: "2026-11-17"
updated: "2026-11-17"
---

现状（src/commands/compact.c）：
- 摘要 transcript 里工具调用 arguments 原样展开（:59-69），write 的整份文件内容也会进摘要请求；只有工具结果被截到 300 字节（:14、:28-39）。
- 摘要请求是 system(508B) + user(transcript) 一次 completion，没有大小检查。
- 成功后 conversation 只剩 system prompt + 一条 summary（:145-157），但只改内存数组、不写 journal（:159-160），/resume 仍加载完整旧历史。
- environment_block/notes_block 槽位未重置，compact 后 sync_context_blocks 认为“未变”不再追加，环境与 notes 从此消失（context.c:1695-1717）。
- 摘要指令不要求保留用户原话。
killi 对照：保留用户请求原文（单条 4,000 字符、总 80,000，超出注明截断，compact.rs:9,14,366-376）；summary 要求 <1500 tokens、不含命令记录（compact.rs:508-519）；压缩后重发 plan/swarm/peers 提醒（compact.rs:165-169）；压缩作为可重放的 journal 记录（session/load.rs:158-166）；超限走 reduced() 重试。

任务：
1. transcript 中的工具参数截断（保留 name + 关键字段摘要 + 字节数），write 内容只保留路径与行数。
2. 摘要请求前做大小检查，超限走缩减版重试。
3. 新历史 = 冻结 system + “用户原话集合（逐条截断）” + summary，并清空/重置 environment_block、notes_block 槽位以便重新追加。
4. 把压缩结果写入 journal（作为可重放的 compaction 记录），使 /resume 复用压缩结果而不是完整旧历史。
验收：压缩后 /resume 的历史与压缩后一致；环境块与 notes 在下一轮重新出现；摘要请求字节数下降有记录；make test 通过。
