---
id: 53
title: 细化：共享 scope 身份与对象生命周期
status: closed
priority: medium
labels:
    - source:lif
    - type:task
    - parent:33
relations:
    blocks:
        - 38
    depends-on:
        - 37
    related-to:
        - 33
created: "2026-11-17"
updated: "2026-11-17"
closed: "2026-11-17"
---

范围：src/commands/context.c、src/commands/context.h、src/commands/subagent.c、src/commands/message.c 和 provider 入口。核实 clay_commands_run_completion 调用链，建立覆盖 worker 生命周期的进程内共享对象，按 provider、凭据身份/端点及模型隔离，不明文记录密钥；可信更宽范围保守合并。验收：同 scope 共用、异 scope 隔离，模型切换及释放不串扰，公开接口与 Codex/Grok 认证兼容。

## 调查与未完成缺口

已核实 completion 调用来自 message.c、subagent.c 和 compact.c；subagent_tool 在返回前 join 所有已启动 worker。各 worker 共用 ClayCommands 与 ClayConnectedProvider，而 clay_commands_save_codex_credentials / clay_commands_save_grok_credentials 会释放并替换共享凭据，当前并发读取和保存存在数据竞争及潜在悬空指针。不能仅增加 scope map 就宣称线程安全。

Codex account_id 可用于稳定身份，但 Grok 凭据结构没有稳定账号字段；access/refresh token 会轮换，直接按令牌散列会使同一账号变为不同 scope，按 provider 固定合并则会混淆不同账号。需要先明确可信身份和令牌轮换的关联策略，并同步凭据快照/更新（网络 I/O 及工具调用期间不能持有全局锁）。还需设计跨 ClayCommands 实例的进程级 registry 引用计数及移除规则，保证 worker 持有引用期间对象不释放，处理模型切换、认证失效及内存分配失败。

尚未实现 scope、接入 provider 或离线多线程/隔离/泄漏测试；任务保持 in-progress，验收未满足。现有工作区代码改动均未触碰。

## 本轮补充调查与实施方案（未完成）已核对 src/commands/context.c、src/commands/context.h、src/commands/subagent.c、src/providers/openai_codex.c、src/providers/grok.c 及凭据保存调用点。Grok 除配置字符串外还存在共享 grok_client 指针竞态：worker 保存成功或认证失败会销毁/替换该指针，而其他 worker 用它判断认证模式；模型列表还使用同一客户端。Codex/Grok 都在请求内部自行刷新，先复制快照再无条件保存不足以正确处理并发 refresh-token 轮换：旧 worker 可能覆盖新凭据或把已成功更新的认证删除；即使 CAS 拒绝旧结果，也不能避免多个 worker 同时使用一次性 refresh token。建议下一步：将凭据及版本放入生命周期覆盖所有 worker 的稳定堆对象，短互斥保护快照、版本、失效标记和 refresh-in-flight；通过条件变量单飞刷新，释放互斥后执行 OAuth 网络请求，重新加锁按版本提交并唤醒等待者；401 刷新及初始过期刷新都走同一协调路径，禁止旧版本失败清除新版本。持久化须按版本有序提交，不能持锁执行网络请求或工具回调；模型客户端也必须独立快照，避免与 worker 互相销毁。测试用屏障控制两个 worker 同时过期、旋转刷新、旧请求 401/失败及模型查询，并检查配置保存顺序、无锁内网络及销毁时无悬空引用。scope 设计建议：Codex 以可信 account_id、provider、endpoint 和 model 为边界；Grok 无可信稳定账号字段时，为独立登录/凭据所有者创建不可猜随机匿名 lineage，刷新只更新 lineage 下凭据；没有可信关联证据的不同配置实例不可合并。API key 不能明文留在 scope key，需使用进程密钥化密码学摘要并明确碰撞与密钥生命周期；不能用轮换 token 散列充当稳定身份。进程 registry 以互斥和引用计数管理，worker 及模型调用持有引用至结束，最后释放安全移除；模型切换获取新 scope 而不破坏旧 worker。本轮仅完成调查和方案，未修改代码、未实施并发测试，也未验证真实凭据。系统 cc 能编译并运行空 ThreadSanitizer 探针，但不代表项目 TSAN 或目标场景通过。任务保持 in-progress。

## 本轮所有权核查（未完成）用户限定本轮为 session-owned registry，不操作或依赖共享可变凭据，也不合并独立配置的同账户；此前进程级 registry 和令牌散列建议不适用。实际文件为 src/commands/subagent.c，src/commands/context/subagent.c 不存在。src/main.c 同步运行消息后销毁 ClayCommands；src/commands/subagent.c 的 subagent_tool 在返回前 join 所有已启动 worker，因此当前 CLI 正常调用链上的 session 寿命覆盖 worker，但公开的 clay_commands_run_completion、clay_commands_destroy 没有持有引用或同步协议，尚不能保证任意调用方并发销毁时安全。ClayConnectedProvider 按值保存在可增长、可删除的 commands->providers 数组中；clay_commands_load_provider 对现有槽位原位更换配置，clay_commands_logout_provider 删除并移动槽位。因此数组元素地址和 provider type ID 都不是配置实例的可靠身份；同一地址重用可错误合并独立配置。选择模型的字符串亦会释放替换。要实现可靠 registry，须引入独立稳定配置实例身份/代次、明确调用方的 session 持有及关闭协议，并在实例重配、登出、模型/端点变更与 worker 在途情况下确定安全快照；只以地址或类型作 key 不安全。不得由此推断无法实现；只是现有接口和对象所有权尚不足以证明所需保证。本轮没有添加缺少这些保证的接口或伪通过测试；原有 Codex/Grok 凭据竞态仍存在，也未尝试修复。验证：make test 退出 0；输出包含现有测试提示 grep race baseline: outside content exposed 和 LEAK function 1 /outside/secret.c，不能据此宣称相关安全问题已修复。没有运行真实认证、TSAN 或新增共享/隔离/生命周期测试。保留任务 in-progress。

## 本轮实例身份安全核查（未实施）已核查全部 ClayConnectedProvider 直接构造/复制：生产代码只在 clay_commands_load_provider 中以局部值经 clay_array_push_val 的 memcpy 写入数组；clay_array_grow 使用 realloc，clay_array_remove 使用 memmove；tests/test_message.c 另有直接按值构造和赋值。重连在现有槽位原位替换，登出释放后移动数组。commands_create 在 tool_lock 初始化之前加载 provider，因此不能直接复用该锁做初始化同步。全局原子计数可避免并发生成重复值，但不能使数组访问、重连与删除时的身份读取安全，也不能延长旧槽位或 session 的寿命；在原位字段上使用原子类型仍会遭遇数组 memcpy/memmove/realloc 与并发访问问题。完成要求需先确定并实施覆盖 provider 数组查找、移动、重连、删除及 session 销毁的同步和引用/快照协议，并处理初始化、分配失败与身份耗尽；不能把线程安全 ID 生成误认为线程安全 provider 实例。为避免提交无法满足并发及生命周期要求的伪身份，本轮未修改代码或添加误导性测试，也未实现 registry 或改变认证。make test 退出 0；输出仍有 LEAK function 1 /outside/secret.c，不代表该风险已解决。未运行新增并发测试、TSAN 或真实认证；任务继续 in-progress。

## 本轮 worker 寿命核查（未实现）实际 CLI 在 src/main.c 同步执行消息后销毁会话；src/commands/subagent.c:259-284 启动并 join 每个成功启动的 worker，但 subagent_tool 在 join 后仍使用 commands 汇总 usage，不能以单个 worker 退出作为整个调用结束。公开 include/clay/commands.h:8 的 destroy 接受原始指针并释放对象；对于未持有外部所有权且与最终释放并发开始的调用，无法安全访问内嵌锁或增加引用，也无法只靠内嵌标志保证两个销毁调用安全（等待者可能访问已释放锁，地址还可能被重用）。需要明确外部所有者保证：销毁只能一次，关闭前停止新入口，已进入的批次持有引用直至全部结果处理完成；不可声称支持任意悬空指针和任意重复 destroy。重配还需覆盖 src/commands/context.c:490-496 原位替换配置、:615-629 删除移动 provider、:1229-1256 重置 chat 和模型、src/commands/resume.c:46-48 替换 chat；worker 在 src/commands/subagent.c:130-179 及 src/commands/context.c:1021-1043 借用这些字段。仅在 destroy 等待 worker 无法保护这些路径，等待时也不得持有 worker 需要的 tool_lock 或锁住网络请求。并行 worker 之间共享 OAuth 凭据的竞态另需解决；单纯寿命引用不解决数据竞争。可在明确单所有者关闭协议下继续实现受保护批次引用、关闭屏障与重配规则，但本轮没有完成覆盖所有入口的安全实现，不应假称架构不可能实现。专项检查：make bin/test_subagent && ./bin/test_subagent 成功；现有测试只覆盖输入校验及工具集合，没有启动 worker 或覆盖并发销毁、重配、认证，不能作为这些场景通过的证据。未改源码，任务仍 in-progress。

## 本轮所有者契约及未完成项

在 include/clay/commands.h 明确外部所有者须串行化公开调用、等待调用及其内部 subagent worker 返回后只销毁一次；不承诺并发重配或裸指针并发销毁。该注释不新增运行时同步。未实现共享限流状态：在 src/commands/context.c 中 provider 数组重配可替换/移动元素，而 Codex/Grok worker 并行刷新仍会改写共享凭据；未确定可靠 scope 分配及在途引用时，按地址、provider 类型或单个全会话 deadline 建模会错误合并配置或给出错误隔离保证。下一步是在此所有者契约下为每个配置世代引入稳定的独立 scope，明确模型及端点归属，并在 worker 入口取得可活至 join 的引用；认证并发单独协调，不持锁网络。未新增并发测试、未接入 provider，任务仍 in-progress。验证 make bin/test_subagent && ./bin/test_subagent、git diff --check 通过；现有测试不启动 worker，不能证明生命周期安全。

## 本轮配置世代身份小步（未完成整个任务）ClayCommands 为每次有效 provider 加载分配非零、会话内单调递增的 uint64_t 世代；重配和登出后重建均换代，数组扩容、移动和 Codex token 更新保持世代。计数耗尽时拒绝加载且保留原配置；新增数组项分配失败时释放资源。subagent 在创建线程前复制所选 provider 世代，尚未用于共享限流或 scope registry，世代仅在同一 ClayCommands 内有意义，不包含模型；0 表示未找到。离线测试覆盖扩容、删除搬移、重建、原位重配、Codex 凭据轮换和耗尽。make bin/test_subagent && ./bin/test_subagent、make test、git diff --check 均通过；make test 仍输出既有 LEAK function 1 /outside/secret.c。未解决并发 OAuth 凭据竞态，未实现 scope registry、模型隔离、并发刷新协调或真实认证测试；item 保持 in-progress。

## 本轮 session registry 进展（未完成验收）
新增 src/commands/scope.c，会话持有 registry；completion 按 session-local generation、endpoint、model 查找共享对象，互斥仅用于链表查找和插入，网络调用不持锁。endpoint 和 model 以 session 随机密钥前缀的 SHA-256 存储，临时输入擦除，不存凭据明文；会话销毁按 include/clay/commands.h 的外部串行及 worker join 契约释放 registry。src/main.c 处理初始化失败。并发测试覆盖 8 线程相同 scope、代次/endpoint/model/会话隔离、真实 provider 重配后旧 scope 可用，检查 registry/scope 内无测试敏感字符串；测试 join 后销毁。make test、git diff --check、ASan+LSan 的 test_subagent 通过；make test 仍打印既有 grep race baseline 和 LEAK function 1 /outside/secret.c。
尚未验证真实 subagent completion 到 registry 的端到端 worker 生命周期，也未以故障注入验证全部分配失败；因此本任务保持 in-progress。现有 Codex/Grok 并发凭据刷新竞态仍未解决。registry 只建立身份和生命周期，尚无 RPM/TPM 限流，不代表第二阶段完成；未做真实认证或 TSAN。

## 本轮接入核查（未完成）已核实 src/commands/context.c 的 clay_commands_run_completion 在创建 provider 客户端之前调用 clay_scope_get；message.c、subagent.c、compact.c 均通过该入口，但返回指针只用于判空，未传给任何 provider，不能称作 provider 共享状态或限流已接入。当前 registry 整体在 session destroy 时释放，依赖外部串行与 subagent join 契约；已有测试只直接调用 registry，不验证真实主会话和 worker completion。make test-subagent、make test、git diff --check 通过，完整测试仍输出既有 LEAK function 1 /outside/secret.c。未完成 worker 端到端、分配失败注入和清理验证，未运行真实认证；任务保持 in-progress。Codex/Grok 并发凭据刷新竞态仍未解决。

## 本轮离线 completion 路径测试
为 OpenAI、Codex、Grok 请求客户端保存借用的 session scope；scope 不实现调度。测试专用 completion hook 在客户端绑定 scope 后拦截网络，并用真实 subagent worker、条件变量和主线程 completion 验证同代次同模型共享，worker 阻塞期间 scope 可查；join 后验证模型切换和 provider 重配隔离。scope 分配的三个位置分别注入失败，断言 completion 失败且未进入 provider hook；ASan/LSan 覆盖清理。既有 registry 测试覆盖跨 session、endpoint 和代次隔离。验证 make test-subagent、make test、ASan/LSan test_subagent、git diff --check 通过。未验证真实网络认证或 TSAN；Codex/Grok 共享凭据刷新竞态和限流调度仍未解决，item 保持 in-progress。

## Summary of Changes

完成 session-owned scope 基础验收：按配置 generation、endpoint 和 model 区分并在完成请求客户端关联共享 scope；真实 subagent worker 离线同步测试覆盖生命周期及共享，测试覆盖 scope 三处请求分配失败、重配和隔离。修复 Grok subscription 未向实际 OpenAI 请求客户端传递 scope，增加 Codex、Grok subscription 与 API-key 离线入口测试。make test、make test-subagent、lif check、git diff --check 通过。scope 不包含 RPM/TPM 限流或调度；Codex/Grok 既有共享 OAuth 凭据刷新竞态仍未解决，未验证真实认证或 TSAN。make test 仍报告 grep race baseline: outside content exposed 和 LEAK function 1 /outside/secret.c。

---

## 迁移来源（lif）

- 原 ID: `clay-j7un`
- 原类型: `task`
- 原状态: `completed`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-c8lo` → #33
- 原阻塞于: `clay-7609` → #37
- 原阻塞: `clay-s4rb` → #38
- 原创建时间: `2026-11-15T03:55:04Z`
- 原更新时间: `2026-11-15T07:21:35Z`
- 原文件路径: `.lif/clay-j7un--细化共享-scope-身份与对象生命周期.md`
