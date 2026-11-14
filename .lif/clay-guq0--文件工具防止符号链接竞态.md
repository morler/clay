---
# clay-guq0
title: 文件工具防止符号链接竞态
status: in-progress
type: bug
priority: normal
created_at: 2026-11-14T02:44:09Z
updated_at: 2026-11-14T14:41:36Z
---

逐组件 lstat 与路径操作存在 TOCTOU；需设计并实现基于目录句柄的读写、遍历、grep 和 undo 安全链路，并覆盖并发测试。

Linux read 已改为 openat2/逐组件 openat 并拒绝特殊文件；测试通过。write/edit/glob/grep/undo 仍需目录句柄改造；兼容回退的已打开目录可被移出工作区。

新建文件提交改用 Linux renameat2 RENAME_NOREPLACE，防止准备期间出现的同名文件被静默覆盖；不支持时安全失败。新增 fsync 注入的确定性竞态测试，验证 EEXIST、并发文件保留及临时文件清理；make test 与 make test-workspace-fd 通过。已有文件替换、edit/undo 读写间竞态及父目录移出工作区等问题仍未解决。

审查 grep：现有 grep -Ern 使用小写 r，递归时不跟随链接；新增 unleashed 模式回归验证外部文件和目录链接不泄露。起始路径校验后交给 grep 期间仍存在替换竞态，未修复；make test 通过。

Linux glob 扫描保留检查用目录 fd，第二次打开后比较设备号和 inode；不一致则跳过。新增确定性目录替换测试，make test、test-fs-tools、test-repo-map 通过。该措施并非目录树快照，不能防止打开后目录移出工作区，grep 起始路径竞态仍未解决。

Linux grep 对普通文件目标已改为安全打开并验证普通文件后，通过子进程 stdin fd 搜索，不再重新打开路径；新增确定性目标替换测试及 sandbox/unleashed 输入 fd 测试。make test、test-sandbox、test-fs-tools 通过。目录目标仍沿用路径递归，检查到执行间和递归期间的竞态未修复；打开后移动、硬链接及并发修改不在此保证范围。Windows fd 输入未实现。
