---
# clay-3r23
title: 验证并适配 Windows Unicode 控制台输入
status: todo
type: task
priority: normal
created_at: 2026-11-13T15:13:33Z
updated_at: 2026-11-13T15:39:30Z
parent: clay-4lb4
---

验证 src/render/term.c 的 _getch 输入、中文输入法和特殊键；必要时使用宽字符控制台输入，正确处理 UTF-16 代理对并转 UTF-8，同时检查等待输入和待处理逻辑。验收：Windows 实机验证中文输入法、补充平面字符、方向键、快捷键和粘贴，英文输入不退化。

Windows _getch 输入仍未修复；本环境无可用 Windows 实机验证，交叉编译尝试失败。
