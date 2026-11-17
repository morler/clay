---
id: 122
title: 按协议分别添加输出上限字段
status: open
priority: medium
labels:
    - type:task
    - area:context
    - parent:85
    - step
relations:
    blocks:
        - 85
    depends-on:
        - 121
created: "2026-11-17"
updated: "2026-11-17"
---

改动分三步，不能一把梭：
1. OpenAI 官方（src/providers/openai.c:580-630）：max_completion_tokens = min(配置, 预算 - 估算输入 - 256)，mock 断言字段值。
2. Codex / Responses（src/providers/openai_codex.c:725-745）：字段名是 max_output_tokens，且 ChatGPT 计划会拒绝该字段（crates/killi/src/llm/responses.rs:50-51 有注释）；必须先查官方文档，并在 OAuth/API key 两种认证下分别验证，默认只在 API key 路径开启。
3. 其余兼容 provider：先照 #108 的方法用 mock 确认接受该字段；未确认的一律不发。

验证：每个上线的 provider 都有 mock 断言；未确认的 provider 请求体不得出现该字段；测试加入 UNIT_TEST_TARGETS。
