---
# clay-s4rb
title: 二阶段：进程内共享配额作用域和生命周期
status: in-progress
type: task
priority: normal
created_at: 2026-11-15T03:51:47Z
updated_at: 2026-11-16T03:03:30Z
parent: clay-c8lo
blocked_by:
    - clay-zn7b
---

范围：src/commands/context.c、src/commands/context.h、src/commands/subagent.c、src/commands/message.c、provider 入口。核实 clay_commands_run_completion 调用链；

## 开工前现状核查（2026-11-16）
依赖 `blocked_by: clay-zn7b`（原写 clay-7609，但 zn7b 仍 in-progress 且 `blocking: clay-s4rb`，已改正为实际阻塞者）。已核实：
- `src/commands/scope.c` 注册表/scope 已存在：最多每 scope 1 permit（`ClayScope.occupied`）、条件变量释放唤醒（`pthread_cond_t released`，`CLAY_SCOPE_CANCEL_POLL_MS=20` 轮询取消）、密钥加盐 SHA-256 摘要键（不含明文）。
- `src/commands/context.c:1391-1396` 的 `clay_commands_run_completion` 已按 `provider->generation` + `provider->config->base_url` + `commands->selected_model` 调用 `clay_scope_get`，失败返回 -1；`context.c:1743` 创建 registry，`context.c:1861` 在停止并 join 后台任务后销毁（调用者须先 join worker）。
- 缺口：registry 只在 `ClayCommands` 内，subagent worker 通过共享 `ClayCommands` 复用主会话 scope，尚无独立的生命周期/隔离验证；`clay_scope_acquire` 尚未被任何 provider 入口调用（`grep clay_scope_acquire src/providers` 为空），completion 仅在测试 hook 中拿到 scope，故本任务验收中的“同 scope 共享 / 异 scope 隔离 / 无死锁”目前只由 tests/test_subagent.c 的 test_scopes、test_scope_wait、test_scope_wakeup、test_concurrent_scope_failure、test_completion_scopes 覆盖到 scope 层，不覆盖真实请求准入。
- 范围边界：permit 的排队/取消语义归 clay-dxtk，RPM/冷却归 clay-2jq8；本任务只管共享对象身份、隔离、寿命与不持锁 I/O。设计进程内线程安全共享状态，主会话和并发子代理共用；按 provider、凭据身份/端点及模型隔离，敏感密钥不明文记录。可信信号表明更宽范围时保守合并，未知不视作无限额度。共享对象寿命覆盖 worker，不持锁执行网络 I/O 或工具；模型切换、取消与释放不串扰。验收：多线程 mock 验证同 scope 共享、不同 scope 隔离、无死锁悬挂；Codex、Grok 认证和公开接口兼容。
