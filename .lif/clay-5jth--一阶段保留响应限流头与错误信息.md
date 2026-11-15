---
# clay-5jth
title: 一阶段：保留响应限流头与错误信息
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:51:20Z
updated_at: 2026-11-15T04:06:43Z
parent: clay-c8lo
---

范围：src/http.c、include/clay/http.h 和本地 mock 测试。暴露有界、生命周期明确的 Retry-After 与常见限流头；处理大小写、重复头、多个响应头块及重定向/代理，只采用最终响应；不暴露敏感头，保持缓冲和流式调用兼容。解析 Retry-After 秒数与 HTTP 日期、非法值和溢出，保存状态与有界原始错误体。验收：离线 mock 覆盖两种响应、大小写/重复/多响应、无头/非法/超大/过期日期与释放，不泄漏或破坏现有接口。

## Summary of Changes

阶段验收：检查最终响应头、白名单、缓冲/流式错误体、重复及丢弃、调用方释放；补充 RFC 850/asctime 旧日期保守拒绝和丢弃时不得提前重试测试。make build、make test、HTTP ASan/UBSan 及 git diff --check 通过。仅支持 IMF-fixdate，旧日期返回解析失败，调用方不得据此提前重试。
