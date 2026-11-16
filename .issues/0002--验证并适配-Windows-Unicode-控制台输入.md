---
id: 2
title: 验证并适配 Windows Unicode 控制台输入
status: open
priority: medium
labels:
    - source:lif
    - type:task
    - parent:3
    - draft
relations:
    blocks:
        - 5
    related-to:
        - 3
created: "2026-11-17"
updated: "2026-11-17"
---

验证 src/render/term.c 的 _getch 输入、中文输入法和特殊键；必要时使用宽字符控制台输入，正确处理 UTF-16 代理对并转 UTF-8，同时检查等待输入和待处理逻辑。验收：Windows 实机验证中文输入法、补充平面字符、方向键、快捷键和粘贴，英文输入不退化。

Windows _getch 输入仍未修复；本环境无可用 Windows 实机验证，交叉编译尝试失败。

按用户决定：中文支持暂视为完成，当前工作挂起；既有未验证事项及已知限制保留，恢复时继续处理。

---

## 迁移来源（lif）

- 原 ID: `clay-3r23`
- 原类型: `task`
- 原状态: `draft`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-4lb4` → #3
- 原阻塞于: 无
- 原阻塞: 无
- 原创建时间: `2026-11-13T15:13:33Z`
- 原更新时间: `2026-11-14T13:34:42Z`
- 原文件路径: `.lif/clay-3r23--验证并适配-windows-unicode-控制台输入.md`
