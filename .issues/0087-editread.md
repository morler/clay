---
id: 87
title: edit/read 失败时给出可定位的差异，减少盲目重读
status: closed
priority: medium
labels:
    - type:task
    - area:tools
    - parent:76
relations:
    blocks:
        - 76
        - 88
    depends-on:
        - 127
        - 128
        - 129
created: "2026-11-17"
updated: "2026-11-18"
closed: "2026-11-18"
---

现状：src/commands/fs_tools.c:715 找不到 oldString 时只返回 “oldString was not found in the file”；多匹配只返回 “not unique - include more context”（:720-728）；read 的 ENOENT 才有 did-you-mean 建议（:449-492）。模型只能重新整读文件。killi 实测：edit 失败率 5-10%，把“最接近行/首个不同行 + 文件实际文本 ≤160 字符”加进错误后，盲目重读变成定向读（tools/edit.rs:290-343；decisions 60/61/67，实测 76/961 次失败里提示成本最高的一类被消除）。

任务：
1. 未找到时：给出最接近的起始行号与首个不同行的文件实际内容（限长），或“文件只有 N 行/路径不存在”。
2. 多匹配时：列出每处行号（最多 5 处）。
3. 多 edit 重叠时指出重叠的索引与行号。
4. read 时对疑似二进制/超大行给出明确错误而不是静默截断。
验收：为三类失败各加测试，断言错误文本包含行号；不改匹配与替换语义。
