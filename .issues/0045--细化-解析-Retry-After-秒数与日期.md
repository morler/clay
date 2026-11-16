---
id: 45
title: 细化：解析 Retry-After 秒数与日期
status: closed
priority: medium
labels:
    - source:lif
    - type:task
    - parent:33
relations:
    blocks:
        - 34
    related-to:
        - 33
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

范围：src/http.c、include/clay/http.h 和 HTTP mock。解析秒数与 HTTP 日期，墙钟只用于日期换算；验收：离线覆盖有效值、无头、非法、溢出及过期日期，禁止提前重试。

## Summary of Changes

实现保守的 Retry-After 秒数及 IMF-fixdate 解析，验证溢出、重复、缺失、非法和过期日期；HTTP 离线测试及构建通过。

---

## 迁移来源（lif）

- 原 ID: `clay-m6zq`
- 原类型: `task`
- 原状态: `completed`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-c8lo` → #33
- 原阻塞于: 无
- 原阻塞: `clay-5jth` → #34
- 原创建时间: `2026-11-15T03:55:03Z`
- 原更新时间: `2026-11-15T04:04:32Z`
- 原文件路径: `.lif/clay-m6zq--细化解析-retry-after-秒数与日期.md`
