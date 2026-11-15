---
# clay-6cks
title: 一阶段细化：捕获最终响应头与错误体
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:53:47Z
updated_at: 2026-11-15T04:00:30Z
parent: clay-c8lo
blocking:
    - clay-5jth
---

范围：src/http.c、include/clay/http.h 及本地 HTTP mock。为缓冲和流式请求保留最终响应的有界限流头、状态及原始错误体，明确所有权与释放；不暴露敏感头。验收：离线测试覆盖大小写、重复头、代理/重定向多响应头块仅取最终块、无头及释放，既有接口兼容。

## 验收记录

- 缓冲与流式响应均保留最终状态、限流头白名单及有界原始错误体；重复头按顺序保留，截断/丢弃显式标记，释放可重复调用。
- 离线 HTTP mock 覆盖大小写、重复头、100 中间响应、无头、302、二进制错误体、上限、敏感头过滤与传输失败；解析器测试模拟代理、重定向和尾部块。
- 验证：make -s build、make -s test、HTTP 测试 ASan/UBSan 和 git diff --check 均通过。

## Summary of Changes

保留最终响应的白名单限流头和有界错误体，补充离线回归测试及生命周期说明。
