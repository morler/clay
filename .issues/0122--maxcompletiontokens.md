---
id: 122
title: 按协议分别添加输出上限字段
status: closed
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
updated: "2026-11-19"
closed: "2026-11-19"
---

改动分三步，不能一把梭：
1. OpenAI 官方（src/providers/openai.c:580-630）：max_completion_tokens = min(配置, 预算 - 估算输入 - 256)，mock 断言字段值。
2. Codex / Responses（src/providers/openai_codex.c:725-745）：字段名是 max_output_tokens，且 ChatGPT 计划会拒绝该字段（crates/killi/src/llm/responses.rs:50-51 有注释）；必须先查官方文档，并在 OAuth/API key 两种认证下分别验证，默认只在 API key 路径开启。
3. 其余兼容 provider：先照 #108 的方法用 mock 确认接受该字段；未确认的一律不发。

验证：每个上线的 provider 都有 mock 断言；未确认的 provider 请求体不得出现该字段；测试加入 UNIT_TEST_TARGETS。

Linux 验收（2026-11-19）：

- `~/.clay/config.json` 的 `max_output_tokens` 启用每次请求的上限。官方 Chat Completions 发 `max_completion_tokens`，官方 Responses 的 API key 路径发 `max_output_tokens`。可选的 `context_window` 扣除估算输入和 256 token 余量，再应用协议最小值（分别为 1 和 16）。窗口与自动压缩预算独立。
- 对照 [Chat Completions](https://raw.githubusercontent.com/openai/openai-python/main/src/openai/types/chat/completion_create_params.py) 和 [Responses](https://raw.githubusercontent.com/openai/openai-python/main/src/openai/types/responses/response_create_params.py) 的官方 SDK 协议定义。模拟 HTTP 分别验证 Responses API key 和 Codex OAuth 的端点、认证头、字段值或字段缺省。真实公开 API 调用返回 `401 invalid_api_key`，当前没有可用于真实 Codex 请求的 Clay OAuth 凭据。
- 模拟 HTTP 验证 OpenAI 代理、OpenRouter、Groq、DeepSeek、Mistral、Together、Grok 和 custom 的请求均省略输出上限字段。第三方端点接受该字段的行为尚未验证。
- `tests/test_output_limit.c` 已接入 `UNIT_TEST_TARGETS`，还覆盖未配置、0、窗口收紧、协议最小值，以及输出达到上限时保留文本并跳过未完成工具调用。Linux `make -j8 test` 通过。
