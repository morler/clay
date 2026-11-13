---
# clay-69su
title: 中文支持端到端回归验证
status: todo
type: task
priority: normal
created_at: 2026-11-13T15:13:33Z
updated_at: 2026-11-13T15:39:30Z
parent: clay-4lb4
blocked_by:
    - clay-p83r
    - clay-505n
    - clay-3r23
---

运行现有测试并补充终端集成测试：中文输入法提交、粘贴、历史记录、长文本滚动、流式回复、窗口缩放；覆盖中文 JSON 往返、ANSI 颜色及英文输入。Windows 实机验证中文输入法和特殊键。验收：中文输入、编辑、提交和回复无损，光标、换行、裁剪正确，原有功能不退化；记录终端与字体列宽差异。

make test 在 tests/test_sandbox.c:44 失败；专项 UTF-8 测试通过，端到端及 Windows 验证尚未完成。
