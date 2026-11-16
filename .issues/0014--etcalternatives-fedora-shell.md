---
id: 14
title: 沙箱内挂载 /etc/alternatives，修复 Fedora 上 shell 命令缺失
status: closed
priority: medium
labels:
    - source:lif
    - type:bug
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

Fedora 上 /usr/bin/cat 等经 /usr/bin/x -> /etc/alternatives/x -> /usr/bin/gnu_x 两级符号链接；沙箱只挂载 /usr /bin /sbin /lib /lib64，/etc/alternatives 缺失导致 coreutils 全部 command not found（exit 127），make test 在 tests/test_sandbox.c:44 失败。修复：setup_system_dirs 中按需创建 /etc/alternatives 并只读 bind mount。验证：bin/test_sandbox 输出 sandbox tests passed，make test 全部通过。

## Summary of Changes

已在 src/sandbox/linux.c setup_system_dirs 中条件挂载 /etc/alternatives；bin/test_sandbox 与完整 make test 通过。

---

## 迁移来源（lif）

- 原 ID: `clay-zul5`
- 原类型: `bug`
- 原状态: `completed`
- 原优先级: `normal`
- 原标签: 无
- 原父项: 无
- 原阻塞于: 无
- 原阻塞: 无
- 原创建时间: `2026-11-14T13:12:43Z`
- 原更新时间: `2026-11-14T13:12:50Z`
- 原文件路径: `.lif/clay-zul5--沙箱内挂载-etcalternatives修复-fedora-上-s.md`
