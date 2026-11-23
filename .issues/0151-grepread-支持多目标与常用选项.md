---
id: 151
title: grep/read 支持多目标与常用选项
status: closed
priority: high
labels:
    - type:task
    - area:tools
    - area:test
    - parent:148
relations:
    blocks:
        - 148
    related-to:
        - 150
        - 152
created: "2026-11-20"
updated: "2026-11-20"
closed: "2026-11-20"
---

## 现状

- `grep` schema 只暴露 `pattern/path/glob`（`src/commands/fs_tools.c:1564-1585`），没有 context、大小写、files-with-matches、多路径、结果上限。
- `read` 一次只接受一个 `path` 和一组 `offset/limit`（`fs_tools.c:613-634`）；长文件要翻页，多文件要多次调用。
- 真实日志：shell grep 162 次 vs grep 工具 14 次；shell 侧 cat 82、sed 75、ls 65。模型在用 shell 补工具缺失的能力。
- 最坏 64 轮 turn 有 14 次 edit 分布在 14 个路径；read 侧也有 76 次调用覆盖 48 个唯一路径、33 次带 offset。

## 任务

1. `grep` 扩展：
   - 新增 `paths[]`（与 `path` 兼容）、`context`（前后行数）、`ignore_case`、`files_with_matches`、`max_results`。
   - 保持总输出上限（当前 `CLAY_FS_GREP_LIMIT` 64 KiB）和 UTF-8 边界；截断时明确标记。
   - 保持 Linux fd-safe 遍历/读取路径；非 Linux 回退行为不变。
2. `read` 扩展：
   - 新增 `paths[]`，每项可带 `offset/limit`；输出按路径分段，保留行号。
   - 设总字节上限；UTF-8 安全；单路径旧参数保持兼容。
3. 更新工具描述和 schema，让模型知道一次可处理多个目标。
4. 测试覆盖：多路径、context、ignore-case、files-with-matches、max_results、总上限截断、非法路径、空匹配、UTF-8 边界；Linux 正常路径 + fallback 路径。

## 验收

- 新增/扩展 `tests/test_fs_tools.c` 用例，覆盖上述行为。
- `make test-fs-tools test-fs-matrix` 通过；涉及 fallback 时 `make test-fs-fallback` 通过。
- `make test` 通过。
- 手工用同一 pattern 对比单路径多次调用与一次多路径调用，结果集合一致、顺序稳定、截断标记一致。

## 2026-11-20 执行结果

- `read` 新增 `paths[]`：每项可为字符串或 `{path, offset, limit}`；顶层 offset/limit 作为缺省值；总输出上限 256 KiB，按 UTF-8 边界截断；结果带 `files` 元数据，单个文件失败时保留其他文件并标记 `ok=false`。
- `grep` 新增 `paths[]`、`ignore_case`、`context`、`files_with_matches`、`max_results`。多路径逐个复用单路径实现，共享 64 KiB 内联窗口和 scratch 落盘；`max_results` 按结果行截断（多路径 header 不计数），截断时设置 `output_truncated` 与 hint。
- 非 Linux 分支保持 shell grep 路径；`-U__linux__ -D__APPLE__` 语法检查通过，Windows 交叉构建因本机无 mingw-w64 未运行。
- 工具 schema 变大，`CLAY_PROMPT_AND_TOOL_BUDGET` 17400→17800，`CLAY_SUBAGENT_FIXED_COST_BUDGET` 16800→17300；实测 prompt+tools 17759 B、subagent 17212 B。
- 新增 `test_read_many_and_grep_options`：多文件 read、每项 offset/limit、缺失文件、多路径 grep、ignore_case、context、files_with_matches、单/多路径 max_results。
- 验证：`make test`、`make test-fs-tools`、`make test-fs-matrix`、`make test-fs-fallback`、`git diff --check` 通过。

## 不做

- 不做 multi-edit/apply_patch（后续独立 issue）。
- 不改 shell_exec 的 cwd/批量命令（#152）。
- 不增加新的文件遍历/正则依赖；沿用现有实现。

## 规模

预计 20-30 轮；涉及 `src/commands/fs_tools.c`、`src/commands/context.h`、`tests/test_fs_tools.c`、Makefile（如新增测试目标）。
