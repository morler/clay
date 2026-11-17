---
id: 91
title: 子代理记账、summary 去重与独立预算
status: open
priority: medium
labels:
    - type:task
    - area:subagent
    - parent:76
relations:
    blocks:
        - 76
    depends-on:
        - 83
        - 140
        - 141
        - 142
created: "2026-11-17"
updated: "2026-11-17"
---

现状（src/commands/subagent.c）：
- on_usage_details 只累加 input/output，忽略 cached（:77-81），父会话缓存统计因此失真；clay_commands_add_usage 同样只加 input/output（:345）。
- 子代理结果里 summary 同时出现在 results[].summary 和顶层 output 字符串（:305-348），父代理收到两份。
- 子代理没有上下文预算或压缩，只有 32 轮上限（:172），工具结果原样累积。
- summary 上限 16KB（:8）。
killi 对照：subagent 继承系统提示与固定工具列表，fresh 历史（只保留 system），fork 才继承历史；有独立 Stats 和 compact_at；返回给父的只有最终报告，截断时明确说明并 spill（subagent.rs:346-448,311-317；swarm.rs:74-152 等额裁剪 + 全文路径）。

任务：
1. cached tokens 计入子代理与父会话统计（#77 的记录字段对齐）。
2. 顶层 output 只保留一次 summary（或明确只留压缩版，完整版进 transcript 路径）。
3. 子代理接入 #83 的预算/压缩逻辑。
4. 超过上限时按等额份额裁剪并给完整 transcript 路径，而不是只丢弃。
验收：双分支 subagent 测试断言响应体只含一份 summary；cached 计数正确；make test 通过。
