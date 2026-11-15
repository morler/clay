---
# clay-8mkl
title: 二阶段：端到端压力回归与限制说明
status: todo
type: task
created_at: 2026-11-15T03:52:08Z
updated_at: 2026-11-15T03:52:08Z
parent: clay-c8lo
blocked_by:
    - clay-tg6w
---

范围：tests/test_subagent.c、tests/test_message.c、provider mock fixtures、README.md、ARCHITECTURE.md、Makefile。覆盖主会话与最多四个子代理并行、同/异 provider 和模型、429 恢复、取消、工具副作用、各 provider 错误及缺失头；检查线程安全、释放、时间控制和跨平台构建。文档说明默认策略、估算及配置来源、原错保留、平台限流与上游容量差异；无通用可查询的服务端额度，平台 API 可能无权限、限速或无此接口，不承诺精确值或跨进程协调。验收：本地离线 mock 与相关 make test-* 可复现，文档与默认值一致，Windows 构建或不可用原因记录，不引入真实密钥或外网依赖。
