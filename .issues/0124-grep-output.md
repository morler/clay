---
id: 124
title: grep 到限提示与 output 模式
status: closed
priority: high
labels:
    - type:task
    - area:tools
    - parent:86
    - step
relations:
    blocks:
        - 86
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

改动：src/commands/fs_tools.c:1100-1135、1325-1345：到限输出 “N matches shown of more, narrow the pattern or pass limit=”；新增 output=files|count；grep 结果去掉重复路径前缀。
验证：多匹配测试断言提示；output=files 只回文件列表。
