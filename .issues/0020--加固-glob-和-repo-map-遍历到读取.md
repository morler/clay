---
id: 20
title: 加固 glob 和 repo-map 遍历到读取
status: closed
priority: medium
labels:
    - source:lif
    - type:task
    - parent:15
relations:
    blocks:
        - 29
    depends-on:
        - 22
        - 25
    related-to:
        - 15
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

检查 src/commands/fs_tools.c、src/commands/repo_map.c、src/commands/workspace_fd.c 及相关测试：在受限 FD 下保持遍历到读取句柄链，或证明从同一固定根重解析满足既定安全契约但不是一致性快照。验收：正常与强制回退确定性交错覆盖逃逸链接、消失、权限和普通目录替换；外部诱饵、排序及过滤正确；低 FD 深层遍历可用；保留 fallback 已打开目录移出限制。

## 复核进度

现有 Linux glob walker 按相对路径反复从根重新打开；repo_map 枚举与读取使用不同根描述符并以路径重开，尚未形成遍历到读取的句柄链。已有压力测试及 workspace_fd 确定性交错测试不能替代 glob/repo-map 的确定性交错、消失、权限、降级及外部诱饵回归。正常及强制回退专项测试、lif check 和 git diff --check 已通过；grep race baseline 报告可暴露外部内容，属于 README 所述排除范围。未改代码，验收未满足。

## 本轮测试进度

保留现有改动并复核 tests/test_repo_map.c：包装 clay_workspace_fd_read，在读取前确定性替换或移走父目录；断言不出现内外诱饵及原有排序。tests/test_fs_tools.c 与 tests/test_repo_map.c 增加枚举后撤销父目录权限的确定性测试；root 运行时明确跳过。普通 make test-fs-tools test-repo-map 与 make test-fs-fallback 均通过（本次 uid 1000，权限测试未跳过）。这些测试验证现有路径重开时失败关闭，不证明遍历到读取保持句柄链；句柄链改造和相应测试仍未完成，故不关闭任务。强制回退构建跳过 openat2 专项。grep race baseline 报告 outside content exposed，属于 README 已记录的排除范围。

## 本轮实现进度Linux repo_map 的两次文件枚举及读取现共享同一个工作区根 fd；新增确定性根 fd 打开后切换 cwd 的外部诱饵及排名测试，原有父目录交换、消失和权限测试保留。普通 make test-repo-map test-fs-tools、make test-fs-fallback 和 git diff --check 均通过，回退构建按预期跳过 openat2 专项。grep race baseline 仍报告 outside content exposed，属于 README 的排除范围。此改动只固定根 fd，walker 仍存路径并从根重开，尚未形成枚举到读取的句柄链；glob 同样仍按路径重开。后续需设计限制 fd 资源的目录句柄传递或边遍历边读取，并补充对应确定性交错测试；已打开目录被移出工作区的限制继续存在。

## 验收说明Linux glob 从固定遍历根重解析队列项，repo_map 两轮遍历和读取使用同一固定根。openat2 每次使用 RESOLVE_BENEATH、RESOLVE_NO_SYMLINKS 和 RESOLVE_NO_MAGICLINKS，拒绝逃逸符号链接；其他非允许错误失败关闭。迭代遍历不按深度持有 FD；但普通目录替换可能被重新解析，不保证对象身份或快照。逐组件 fallback 不等价于 openat2，已打开目录移出根后仍可访问；根移动、挂载和硬链接限制保持不变。确定性交错覆盖外部链接、消失、权限及普通替换；低 FD 下验证 40 层深度；原有排序及过滤保留。普通专项与强制回退测试通过，回退构建跳过 openat2 专项。grep 竞态仍属 README 排除范围。

## Summary of Changes

证明固定根重解析与已公开契约一致但不提供快照，明确 fallback 限制；新增普通替换及低 FD 深层遍历测试并修正可解析诱饵，普通专项、强制回退、lif check 和 git diff --check 通过。

---

## 迁移来源（lif）

- 原 ID: `clay-jk7c`
- 原类型: `task`
- 原状态: `completed`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-1m1e` → #15
- 原阻塞于: `clay-mubf` → #22, `clay-uxeh` → #25
- 原阻塞: 无
- 原创建时间: `2026-11-15T01:38:37Z`
- 原更新时间: `2026-11-15T02:36:05Z`
- 原文件路径: `.lif/clay-jk7c--加固-glob-和-repo-map-遍历到读取.md`
