---
# clay-ni3u
title: 修复新文件原子提交及清理错误
status: completed
type: bug
priority: normal
created_at: 2026-11-15T02:19:17Z
updated_at: 2026-11-15T02:24:19Z
---

优先使用 renameat2 RENAME_NOREPLACE，测试回退与失败；记录不支持平台的限制。

已实现 renameat2(RENAME_NOREPLACE) 优先、强制回退及故障注入测试；仍未完全解决：旧内核或文件系统不支持 RENAME_NOREPLACE 时，linkat 回退遇到持续 unlinkat 错误会留下目标的隐藏硬链接；若同时不支持硬链接则无法原子新建。须研究安全兼容方案或决定对不支持的环境失败关闭，不能直接原位写入。测试：make test-workspace-fd test-fs-tools test-undo、make test-fs-fallback、git diff --check 通过。


## 2026-11-15 调查与未解决风险

- 已为临时名 unlinkat 的 EINTR 增加重试，并在 workspace_fd 测试中注入连续三次 EINTR；持续 EIO 仍按错误返回。验收测试还验证：linkat 已提交后清理持续失败，即使重写并删除目标，隐藏硬链接仍能读出原内容。
- linkat 成功即已提交，后续 unlinkat 失败不能回滚：删除目标可能竞态删除其他人的替换，删除隐藏链接也可能一直失败。现有 int/errno 接口把此状态当普通错误返回；write 工具会丢弃待提交的 undo，盲目重试可能覆盖已提交结果，删除目标也不能消除残留。不能把它报告为普通成功，也不能使用原位写入或假装清理成功。
- 可执行的失败关闭方案（尚未实现）：新建文件改用 O_TMPFILE 在目标父目录创建无名 inode，写入并 fsync 后用 linkat(AT_EMPTY_PATH) 直接排他发布；仅在文件系统、内核和权限都支持时允许发布，任何不支持或失败直接关闭 fd 并报错，绝不退回含内容的命名临时文件或原位写。须验证无名 inode 发布支持及安全语义、EEXIST 竞态、写入/fsync/link 失败、权限和目录移动等边界；适配非 Linux 与不支持 O_TMPFILE 的文件系统时应拒绝新建。已有覆盖写仍用命名临时文件，其清理失败内容残留是独立风险，不能声称全局无泄漏。
- 在方案实现前的实际安全措施：禁止在 renameat2(RENAME_NOREPLACE) 不受支持的环境对敏感数据调用该写入路径；已发生错误时暂停重试，隔离目录，核对目标和 .clay-*.tmp 的 inode 与内容，在受信任环境人工清理并确认，必要时按内容泄漏处理。不要批量删除未知临时文件。
- 验证：make test-workspace-fd test-fs-tools test-undo test-fs-fallback 和 git diff --check 通过。条目保持 in-progress，尚未实现上述失败关闭策略。

## Summary of Changes

采用最小失败关闭方案：新建仅用 renameat2(RENAME_NOREPLACE)，不支持或其他错误时不发布并尽力清理命名临时文件，删除 linkat 回退；原有文件替换路径不变。前述 O_TMPFILE 方案与 linkat 风险描述为历史记录，已被此方案取代。兼容影响：不支持该操作的内核、文件系统或系统调用过滤环境不能新建。EINTR 清理重试不保证最终结束，持续清理错误仍可能留下含内容的未提交临时文件，原错误优先返回。测试覆盖并发 EEXIST、ENOSYS/EINVAL/EOPNOTSUPP、清理 EINTR 与 EIO、正常及 CLAY_NO_OPENAT2 强制回退；make test-workspace-fd test-fs-tools test-undo test-fs-fallback 通过。
