---
id: 8
title: 修复保存的 provider 启动选择
status: closed
priority: medium
labels:
    - source:lif
    - type:bug
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

保存的 custom provider 和模型不应被隐式环境凭据选择覆盖。

## Summary of Changes

修复隐式环境凭据覆盖已保存 provider 的优先级，并添加重启选择与显式覆盖回归测试；test-modes、test-command、test-storage 通过。

---

## 迁移来源（lif）

- 原 ID: `clay-ijou`
- 原类型: `bug`
- 原状态: `completed`
- 原优先级: `normal`
- 原标签: 无
- 原父项: 无
- 原阻塞于: 无
- 原阻塞: 无
- 原创建时间: `2026-11-14T02:00:41Z`
- 原更新时间: `2026-11-14T02:02:50Z`
- 原文件路径: `.lif/clay-ijou--修复保存的-provider-启动选择.md`
