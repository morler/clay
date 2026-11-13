---
# clay-505n
title: 修复中文显示宽度和流式输出
status: todo
type: task
priority: normal
created_at: 2026-11-13T15:13:33Z
updated_at: 2026-11-13T15:39:30Z
parent: clay-4lb4
blocked_by:
    - clay-ogb9
---

让 src/render/term.c 测宽与裁剪、src/render/list.c 换行与尾部裁剪使用一致的列宽规则；修正 src/render/below.c 输入视口和光标定位，处理空间不足以容纳双列字符及超窄终端。验证 src/commands/message.c 流式分片是否会跨 UTF-8 边界，若会则缓冲不完整字节直至完整或流结束。验收：中英混排、组合字符、ANSI、窄屏、分片和无效输入测试通过，不越界或死循环。

已实现列宽换行、视口和流式分片处理并加入专项测试；仍需终端集成验证。
