---
# clay-w8rj
title: 基线与未提交改动审查
status: completed
type: task
priority: normal
created_at: 2026-11-15T01:38:37Z
updated_at: 2026-11-15T01:41:48Z
parent: clay-1m1e
---

检查 git status/diff、Makefile、src/commands/workspace_fd.c、tests/test_workspace_fd.c 及 clay-guq0、clay-nb73、clay-7sah、clay-t3kr；验收：记录命令、测试结果、失败和已有改动，不覆盖用户变更。

## 基线验收记录（2026-11-15）已运行 lif prime 并阅读 ponytail skill；检查 git status、git diff、Makefile、src/commands/workspace_fd.c、tests/test_workspace_fd.c 和四个关联条目；git diff --check 退出 0。工作前已有修改：.lif/clay-guq0--文件工具防止符号链接竞态.md、src/commands/workspace_fd.c、tests/test_workspace_fd.c；未跟踪 .rpi/。源码改动为 openat2 优先读取、openat 回退、路径校验和 seccomp 与目录交换竞态测试，Makefile 无改动。clay-guq0、clay-nb73 进行中；clay-7sah、clay-t3kr 完成。环境：Linux Fedora x86_64 7.3.3-200.fc44，Homebrew clang 23.1.5，GNU Make 4.4.1；lif run 无任务。首次 make -j4 BUILD_DIR=/tmp/clay-baseline.5kw3Dw/build BIN_DIR=/tmp/clay-baseline.5kw3Dw/bin test 编译成功但退出 2，因 Makefile 在绝对测试路径前加 ./ 导致 .//tmp/clay-baseline.5kw3Dw/bin/test_workspace_fd: 没有那个文件或目录，测试未运行。使用 make -s -j4 BUILD_DIR=../../../../../../tmp/clay-baseline.5kw3Dw/build BIN_DIR=../../../../../../tmp/clay-baseline.5kw3Dw/bin test 退出 0；默认单元测试通过，日志 /tmp/clay-baseline.5kw3Dw/test-relative.log。LEAK function 1 /outside/secret.c 为 tests/test_repo_map.c:103 模拟脚本输出。独立执行 /tmp/clay-baseline.5kw3Dw/bin/test_workspace_fd 退出 0，无 openat2/seccomp 或 renameat2 交换跳过提示。cc -std=gnu11 -Wall -Wextra -O2 -Iinclude -DCLAY_NO_OPENAT2 src/commands/workspace_fd.c tests/test_workspace_fd.c -o /tmp/clay-baseline.5kw3Dw/fallback 后执行 fallback 退出 0。写入、edit、glob、grep、undo 竞态尚未整体解决；打开后移出工作区及逐组件回退目录迁移仍有风险，竞态采样通过并不证明无竞态。未测 Windows、独立 PTY 和网络服务；未改源码或提交。

## Summary of Changes

完成 Linux 基线、未提交改动及关联任务审查，记录测试结果、失败和风险；未改源码或提交。
