---
id: 104
title: 压缩后重新追加当前环境与 notes
status: open
priority: high
labels:
    - type:task
    - area:cache
    - parent:80
    - step
relations:
    blocks:
        - 80
    depends-on:
        - 102
created: "2026-11-17"
updated: "2026-11-17"
---

改动：context.c 在压缩/重写（#83/#84）后把 environment_block/notes_block 槽位置空，使下一轮 sync 重新追加；与 #84 的第三步对齐。注意顺序：#83/#84 先落地，本项随后；否则压缩后环境和 notes 会永久消失。

验证：压缩后一轮请求里同时出现最新环境块与 notes 块，且只各一条；测试加入 UNIT_TEST_TARGETS。
