---
# clay-07dm
title: 建立 grep 行为与竞态基线
status: completed
type: task
priority: normal
created_at: 2026-11-15T01:38:37Z
updated_at: 2026-11-15T01:56:42Z
parent: clay-1m1e
blocked_by:
    - clay-mubf
---

检查 src/commands/fs_tools.c、tests/test_fs_tools.c、Makefile 和 clay-o3q7；记录 grep 正则、输出、过滤、错误及 sandbox/unleashed 行为，建立确定性交错基线。验收：现有行为和预期安全边界可复现，记录失败及跳过。

## 基线与后续回归

- Unleashed 使用 grep -Ern；单文件输出为行号:内容，目录输出含文件名；普通和扩展正则、无匹配、非法正则、缺失路径、空模式、glob、shell 引号、二进制过滤及 64 KiB 预览截断已有测试。错误消息为 pattern is required 或 grep failed；无匹配是成功空输出。
- Linux 确定性交错：测试 PATH 中的 grep 包装器在路径检查之后、真实 grep 打开之前，将工作区起始目录替换为指向外部目录的符号链接。当前 Unleashed 实测输出 outside content exposed，CLAY_NO_OPENAT2 同样复现；该观察不是安全通过断言，不使常规测试因已知缺陷失败。
- 修复后的回归应使用不依赖外部 grep PATH 的同步点，在检查后和打开前交换起始目录及中间组件，断言结果不含外部秘密，并验证正常搜索仍成功；同时分别覆盖 Unleashed、可用的 Sandbox 和强制回退，避免只因包装器未运行而误判安全。
- 当前环境 Sandbox 普通 grep 集成通过；因 Sandbox 固定 PATH，未实现其确定性竞态交错，不声称验证 Sandbox 竞态边界。Windows grep 不可用，未在 Windows 测试；/usr/bin/grep 缺失时 Linux 竞态探测跳过。未验证 4 MiB 捕获上限。test-fs-fallback 中现有 repo-map 输出 LEAK function 1 /outside/secret.c，测试自身仍通过，非本任务新增断言。
- 验证：make test-fs-tools、make test-fs-fallback、git diff --check、lif check 通过。初次基线断言误以为单文件输出带文件名，实测无文件名后已修正。

## Summary of Changes

建立 grep 功能、错误与确定性 Unleashed 竞态基线；记录外部内容泄漏、后续回归设计及 Sandbox 竞态测试缺口。
