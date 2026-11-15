---
# clay-mcxf
title: 修复并发 worker 的 Codex/Grok OAuth 凭据竞态
status: in-progress
type: bug
priority: high
created_at: 2026-11-15T07:42:54Z
updated_at: 2026-11-15T07:51:39Z
blocking:
    - clay-dxtk
---

二阶段前置：src/commands/context.c:563-629 保存凭据时释放并替换共享配置字符串，:1054-1101 并发 worker 读取这些字符串及 grok_client；Grok 保存还会销毁/替换模型客户端，可能产生数据竞争、悬空引用和使用后释放。src/providers/openai_codex.c 与 src/providers/grok.c 的过期刷新和 401/403 刷新都由各短生命周期客户端独立进行；同时使用可轮换 refresh token 可能导致失败，旧请求随后无条件保存或移除配置，覆盖或删除新凭据。clay_commands_fetch_models 亦使用持久客户端及相同保存路径。

需要为每个配置代次引入跨模型、覆盖 worker 与模型查询的稳定凭据所有权和短锁保护快照；在过期刷新与 401 刷新处按凭据版本单飞协调，等待者获取最新快照；刷新网络请求、completion 和工具回调不得持锁。提交及持久化必须版本有序，旧版本失败不得删除新认证，分配/保存失败不能丢失已轮换 token；同步处理 Grok 模型客户端寿命及原有外部所有者串行契约。不能只加读写锁、只做 CAS、按 model scope 协调或锁住整个 completion。

验收：离线屏障控制双 worker 同时过期及单次 refresh、旧 401 与新版本交错、刷新失败和成功交错、持久化顺序、模型查询、分配和保存失败；验证无锁中网络/工具、无死锁和新值不回退，并运行专项测试和可用 TSAN。当前未实施修复和确定性交错测试。现有 make test-subagent test-openai-codex test-grok 通过；独立 GCC TSAN test_subagent 通过，但现有测试不执行 OAuth 并发刷新，不能证明此 bug 安全。

## 2026-11-15 阶段进展Codex worker 和模型查询现在在共享锁下创建独立凭据快照，版本校验阻止旧快照提交或删除新版本；持久化顺序受同一锁保护，网络请求不持锁。新增屏障控制的真实双线程快照和冲突提交测试。尚未实现刷新单飞：并发刷新可能同时消耗轮换 refresh token，失败先于成功时仍可能删除旧磁盘配置；双成功时较晚获得的有效轮换 token 可能被版本校验丢弃。保存失败仅保留内存值，客户端销毁后进程退出仍可能丢失轮换 token。Grok 及其客户端寿命竞态未修复；完整任务继续 in-progress。

验证：make test-subagent test-openai-codex test-grok 通过；独立 GCC ThreadSanitizer 构建的 test_subagent 直接运行通过（make 的测试配方对绝对 BIN_DIR 错误拼接 ./，故直接运行二进制）；git diff --check 通过。新增测试不覆盖网络刷新。

## 2026-11-15 Grok 阶段进展

Grok worker 与模型查询使用锁内创建的独立客户端凭据快照，版本校验阻止过期提交或删除新凭据；保存不再销毁或替换共享模型客户端，网络和工具调用不持凭据锁。凭据复制失败不覆盖配置。新增屏障控制的双线程快照和提交冲突测试，验证原快照及长期客户端仍可使用。

验证：make -j4 test-subagent test-openai-codex test-grok 通过；全新目录 GCC ThreadSanitizer 构建并运行 test_subagent 通过；git diff --check 通过。测试未模拟网络刷新或失败交错。刷新单飞、保存失败导致进程退出丢失轮换 token 等问题仍未完成；保持 in-progress。

## 2026-11-15 单飞调用链调查

Codex 的 ensure_token、401/403 重试及模型查询均在 src/providers/openai_codex.c 内调用静态 refresh；Grok 在 src/providers/grok.c 的 ensure_access_token 和通用 OpenAI 请求的 refresh_for_run 中调用 refresh。命令层直到整个 completion 或成功的模型查询结束才保存快照，因此仅在现有保存处加互斥无法实现单飞：刷新成功必须在请求继续前发布凭据并唤醒等待者。当前 token_request 把传输错误、非 2xx、无效 JSON 合并为失败，refresh 将其一律标记 authentication_invalid，可能删除现有磁盘凭据；Codex credentials_from_token 先释放旧字段再 strdup，分配失败未检查，亦须修复。取消回调目前传给 Codex run 而没有传给 refresh 或模型查询；协调机制需要覆盖可取消等待以及在旧版本收到 401 时采用已发布新快照。

本轮仅完成调用链调查，未实现单飞或 mock 并发测试；原竞态及数据丢失风险仍存在，任务保持 in-progress。
