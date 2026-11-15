---
# clay-m6zq
title: 细化：解析 Retry-After 秒数与日期
status: completed
type: task
priority: normal
created_at: 2026-11-15T03:55:03Z
updated_at: 2026-11-15T04:04:32Z
parent: clay-c8lo
blocking:
    - clay-5jth
---

范围：src/http.c、include/clay/http.h 和 HTTP mock。解析秒数与 HTTP 日期，墙钟只用于日期换算；验收：离线覆盖有效值、无头、非法、溢出及过期日期，禁止提前重试。

## Summary of Changes

实现保守的 Retry-After 秒数及 IMF-fixdate 解析，验证溢出、重复、缺失、非法和过期日期；HTTP 离线测试及构建通过。
