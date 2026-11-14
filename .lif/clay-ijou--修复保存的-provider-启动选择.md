---
# clay-ijou
title: 修复保存的 provider 启动选择
status: completed
type: bug
priority: normal
created_at: 2026-11-14T02:00:41Z
updated_at: 2026-11-14T02:02:50Z
---

保存的 custom provider 和模型不应被隐式环境凭据选择覆盖。

## Summary of Changes

修复隐式环境凭据覆盖已保存 provider 的优先级，并添加重启选择与显式覆盖回归测试；test-modes、test-command、test-storage 通过。
