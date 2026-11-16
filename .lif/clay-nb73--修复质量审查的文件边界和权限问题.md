---
# clay-nb73
title: 修复质量审查的文件边界和权限问题
status: in-progress
type: task
priority: normal
created_at: 2026-11-14T02:40:10Z
updated_at: 2026-11-16T02:14:14Z
---

修复文件工具链接越界和项目文件权限变化，并验证安全限制。

已修复静态链接越界、POSIX 普通模式保留和 grep -E；完整测试通过。尚存 TOCTOU，Windows 路径及 ACL/Windows 原子替换待验证；不标记完成。
