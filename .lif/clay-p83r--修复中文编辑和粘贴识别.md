---
# clay-p83r
title: 修复中文编辑和粘贴识别
status: todo
type: task
priority: normal
created_at: 2026-11-13T15:13:33Z
updated_at: 2026-11-13T15:39:30Z
parent: clay-4lb4
blocked_by:
    - clay-ogb9
---

修复 src/render/prompt.c 左右移动、退格和粘贴块偏移，确保不拆分 UTF-8 码点；检查密钥输入、src/render/modals/model_select.c 和其他文本输入路径。优先用 bracketed paste 标记识别粘贴，设计不支持该协议时的兼容行为，避免仅按字节数误判中文输入。验收：中英混排移动、插入、删除、输入法提交和粘贴测试通过，快捷键与英文输入不退化。

已实现码点编辑、密码与筛选退格；非 ASCII 粘贴暂不折叠，bracketed paste 和交互验证待完成。
