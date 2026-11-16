---
id: 13
title: 修复 Windows 工作区路径解析
status: in-progress
priority: medium
labels:
    - source:lif
    - type:bug
created: "2026-11-17"
updated: "2026-11-17"
---

Windows 盘符、反斜杠、UNC 和重解析点需要原生实现及 Windows 验证；当前环境无 Windows 编译器或运行环境。

已实现 Windows 路径字符串规范化及 Linux 测试；make test 通过。待 Windows 原生编译和实机验证，且重解析点检查与文件操作之间仍有竞态。

---

## 迁移来源（lif）

- 原 ID: `clay-zqs6`
- 原类型: `bug`
- 原状态: `in-progress`
- 原优先级: `normal`
- 原标签: 无
- 原父项: 无
- 原阻塞于: 无
- 原阻塞: 无
- 原创建时间: `2026-11-14T02:44:09Z`
- 原更新时间: `2026-11-16T02:14:14Z`
- 原文件路径: `.lif/clay-zqs6--修复-windows-工作区路径解析.md`
