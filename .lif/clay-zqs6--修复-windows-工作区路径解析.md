---
# clay-zqs6
title: 修复 Windows 工作区路径解析
status: in-progress
type: bug
priority: normal
created_at: 2026-11-14T02:44:09Z
updated_at: 2026-11-16T02:14:14Z
---

Windows 盘符、反斜杠、UNC 和重解析点需要原生实现及 Windows 验证；当前环境无 Windows 编译器或运行环境。

已实现 Windows 路径字符串规范化及 Linux 测试；make test 通过。待 Windows 原生编译和实机验证，且重解析点检查与文件操作之间仍有竞态。
