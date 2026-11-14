---
# clay-ogb9
title: 统一 UTF-8 边界和终端列宽处理
status: in-progress
type: task
priority: normal
created_at: 2026-11-13T15:13:33Z
updated_at: 2026-11-14T08:10:17Z
parent: clay-4lb4
---

保留字节缓冲和字节偏移，提供安全的码点前后移动、解码及显示列宽；定义无效或不完整 UTF-8 的处理，避免越界与死循环，保留 ANSI 转义序列零宽行为。评估 Linux、Windows 与 locale 差异，采用一致、可测试的实现。验收：中英、组合字符、无效和不完整 UTF-8、ANSI 序列的边界与列宽测试通过。

已实现跨 locale 的 UTF-8 宽度与裁剪，并加入专项测试；待进一步审查、跨平台验证。

在独立 /tmp 构建目录重新编译并运行 test_prompt_utf8、test_term_width、test_wrap_utf8 均通过；新增 POSIX PTY 中文编辑回归，跨平台和真实终端验证仍待完成。
