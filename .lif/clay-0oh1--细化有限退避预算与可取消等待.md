---
# clay-0oh1
title: 细化：有限退避预算与可取消等待
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:55:04Z
updated_at: 2026-11-15T04:32:49Z
parent: clay-c8lo
blocking:
    - clay-h8nw
blocked_by:
    - clay-onc0
---

范围：src/providers/openai.c、src/providers/openai_codex.c、provider 头及 mock。最多重试 2 次，初值 1 秒指数退避加抖动，单次上限 30 秒、累计 60 秒；Retry-After 为下界，超预算保留原错，单调时钟计预算，should_abort 中断请求与等待。验收：确定性测试次数、抖动、预算、取消、永久配额不重试及最后 429 状态和 body。

## 未完成及交接缺口已核对 HTTP 结构化错误、OpenAI/Codex/Grok 和逐请求安全边界；尚未实现自动重试，不满足验收，保持 in-progress。必须区分 Retry-After 完全缺失与无效、重复或丢弃的值（clay_http_retry_after 均返回 0，后者不可当作无提示重试）；永久配额判定目前在 clay_openai_report_http_error 内且只在有回调时执行，重试前须独立判定，SSE 429 同样须防止永久配额误重试。重试须仅重建当前请求及临时 stream，保留既有工具结果，不提前触发错误回调；Codex 401/403 刷新路径须保持独立。还需跨平台单调时钟、可取消等待与可控抖动的确定性 mock，覆盖预算、次数、取消、原错及三个 provider。现有 tests/test_provider_errors.c 断言首次 429 即退出，实施后须同步调整并验证全部回归。未修改已有未提交源码。

已实现独立共享退避决策和可取消等待：src/providers/retry.c、include/clay/providers/retry.h，离线模拟时钟测试 tests/test_provider_retry.c。尚未接入 OpenAI/Codex/Grok，尚无生产单调时钟及抖动适配、永久配额独立判定或 SSE 处理；因此自动重试仍未生效，本任务继续保持 in-progress。

OpenAI-compatible 请求已接入每请求有限退避，Grok 共用路径已测；Codex 尚未接入，任务继续 in-progress。确定性 mock 覆盖重试次数、抖动、Retry-After、预算、取消、永久配额、SSE、既有工具结果和最终错误；make test 已通过。Windows 生产时钟分支尚未交叉编译验证。

## Summary of Changes

Codex OAuth 请求循环接入共享逐请求退避，保留认证刷新及工具结果；确定性 mock 覆盖安全边界、次数、预算、取消、配额、SSE 和最终错误；make -j4 test 与 git diff --check 通过。Windows 时钟分支未交叉编译。
