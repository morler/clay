---
id: 24
title: README 与 lif 收尾
status: closed
priority: medium
labels:
    - source:lif
    - type:task
    - parent:15
relations:
    blocks:
        - 12
    depends-on:
        - 29
    related-to:
        - 15
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

更新 README.md 及 lif 条目 clay-guq0，汇总 Linux 安全契约、兼容降级、硬链接、目录移出、乐观 undo 与残余窗口。验收：对照所有任务和测试矩阵，文档与实现一致，记录结果与风险；完整范围完成后才关闭 clay-guq0。

先完成本任务并解除依赖，再在完整范围确已完成时单独关闭 clay-guq0；不得把关闭 clay-guq0 作为本任务完成的前置条件。

## 文档收尾（2026-11-17）

README.md 的 “File access and Linux race boundaries” 已覆盖 root 描述符与逐组件回退、硬链接、目录移出、write/edit 与 renameat2、乐观 undo、grep 逐文件固定描述符、沙箱/模式边界和残余窗口；新增 Verification 段记录 `make test-fs-matrix` 与跳过语义。已对照 #16–#23、#25–#29 的实现与本地矩阵结果（详见 #29），文档与实现一致，无失败命令。

`.lif/` 中没有 `clay-guq0` 对应文件；按仓库现行约定不再创建或修改 lif 任务条目，迁移后的状态由 git-issues #15 及其子项承载。因此本项只更新 README 与 git-issues，不触碰 `.lif/`。#15 的完整范围仍需在 #12 完成后另行评估关闭。

---

## 迁移来源（lif）

- 原 ID: `clay-uska`
- 原类型: `task`
- 原状态: `todo`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-1m1e` → #15
- 原阻塞于: `clay-x1q4` → #29
- 原阻塞: 无
- 原创建时间: `2026-11-15T01:38:37Z`
- 原更新时间: `2026-11-15T01:38:47Z`
- 原文件路径: `.lif/clay-uska--readme-与-lif-收尾.md`
