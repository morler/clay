---
id: 86
title: 工具输出上限与续读提示对齐（read/grep/bash/glob）
status: open
priority: high
labels:
    - type:task
    - area:tools
    - parent:76
relations:
    blocks:
        - 76
    depends-on:
        - 78
        - 123
        - 124
        - 125
        - 126
created: "2026-11-17"
updated: "2026-11-17"
---

现状：
- read 上限 256KB（src/commands/fs_tools.c:34），超过时只置 output_truncated（:539），不给续读 offset；没有每行字符上限（killi 是单行 2000 字符、2000 行/50KB、末尾给 'Use offset=… to continue'，files.rs:14,69-100）。
- grep 内联 64KB（fs_tools.c:35），到限直接截断并落盘（:1117-1127、1332-1342），不告诉模型如何缩小范围或提高 limit；匹配上限是 4096 文件（:37）。
- shell 内联 64KB（message.c:9），超限落盘并给路径（:381-403），但没有“尾部优先/失败命令头尾都留”的策略（killi bash.rs:300-419 保留最后 2000 行/50KB，失败命令保留头尾各一半）。
- glob 上限 500（fs_tools.c:38）。
killi 对照与效果见 decisions 36（工具结果约占输入一半，98% 是缓存命中）。

任务：
1. read：单行截断、分页提示（“Showing lines a-b of N. Use offset=…”），默认行数上限而非整个文件。
2. grep：到限时输出 “N matches shown of more, narrow the pattern or pass limit=”，支持 output=files/count 形式（file:line 或仅文件列表），减少 follow-up read。
3. bash：超限保留尾部（构建/测试日志的关键信息在尾部）；失败时保留头部错误 + 尾部。
4. glob：到限提示如何缩小。
验收：对一个大文件/大日志/多匹配 grep 各加测试，断言提示文本与可续读语义；make test 通过。
