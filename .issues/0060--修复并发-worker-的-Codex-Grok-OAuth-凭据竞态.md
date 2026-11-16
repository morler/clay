---
id: 60
title: 修复并发 worker 的 Codex/Grok OAuth 凭据竞态
status: in-progress
priority: high
labels:
    - source:lif
    - type:bug
relations:
    blocks:
        - 51
    depends-on:
        - 62
        - 63
        - 64
        - 65
        - 66
        - 67
        - 68
        - 69
        - 70
        - 71
        - 72
        - 73
        - 74
        - 75
created: "2026-11-17"
updated: "2026-11-17"
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

## 2026-11-15 刷新失败安全前置

Codex token 响应先验证并复制全部字符串，成功后才替换旧值；Codex 与 Grok 仅将 HTTP 400 且 OAuth error 为 invalid_grant 视为明确认证失效。传输错误、服务错误、无效响应和复制失败不再触发凭据删除。新增离线 refresh 测试，覆盖传输、服务、解析、认证错误、逐字段分配失败和成功轮换；make -j4 test-openai-codex test-grok test-subagent 与 make -j4 test 均通过，git diff --check 通过。

尚未实现跨客户端刷新单飞；并发 invalid_grant 仍可能先于成功刷新删除旧凭据，成功轮换后持久化失败也仍可能丢失 token。测试未覆盖真实网络或并发交错，任务保持 in-progress。

## 2026-11-15 Codex 即时发布组件（未完成单飞）Codex 刷新成功后同步通知命令层，在 completion 或模型查询继续之前尝试按版本发布和持久化，发布成功后更新该请求持有的版本；旧版本仍不能覆盖新配置。新增本地 mock 验证失败刷新不通知、成功刷新通知发生在模型请求之前，模型请求返回 503 后观察者仍持有新 token。该测试只验证 provider 回调顺序和复制结果，未端到端验证命令层磁盘持久化或 completion 失败。尚未实现按配置代次的跨模型单飞、等待者唤醒和取消、旧 401 复用新快照；并发刷新仍可能轮换冲突或丢失有效 token。回调复制分配或磁盘保存失败仍可能丢失轮换 token；现有网络刷新测试未覆盖并发交错。Grok 单飞同样未完成，项目保持 in-progress。验证：make -j4 test-openai-codex test-grok test-subagent、make -j4 test、git diff --check 通过；本轮未运行 TSAN。

## 2026-11-15 Codex 协调基础（未接入）新增按 provider 和凭据版本共享的刷新所有权、等待及结束唤醒基础接口；等待期间取消回调在锁外执行，版本变化返回更新标记，失败返回失败，之后可重试。屏障控制测试覆盖第二线程与模拟发布、失败、取消交错，并检查更新后的跨模型快照。基础接口尚未连接 provider 的 refresh 调用，现有测试不发起 mock 网络请求、不证明实际刷新单飞，也未验证线程真正进入条件变量等待后的唤醒。Codex 和 Grok 并发轮换、旧 401 复用、保存失败丢失令牌等风险仍存在；继续保持 in-progress。验证：make -j4 test-subagent test-openai-codex test-grok 和 git diff --check 通过；本轮未运行 TSAN。

## 2026-11-15 Codex 接入调查补充实际 refresh 尚未接入协调接口：refresh 的过期及 401/403 路径、模型查询和 completion 必须统一接入，不能仅依赖现有 observer。现有 observer 不返回发布失败，若旋转后的凭据复制失败，客户端销毁会丢失新 token；协调实现须先解决所有权及发布失败语义，避免唤醒等待者后继续使用旧 token。等待者取消和首位执行者取消也需要区分；token_request 当前不接收取消回调。实验性在 begin 入口检查取消曾使现有失败交错测试失效，因为测试以取消回调作为屏障，入口检查改变了 epoch 采样时序；该实验已回退，尚需设计确定性的实际网络并发测试。本轮未接入单飞。验证：make -j4 test-subagent test-openai-codex test-grok、make -j4 test 和 git diff --check 通过。

## 2026-11-15 Codex token 请求取消安全调查

未实现发送中取消：token_request 使用同步 POST；libcurl 中途终止时无法知道服务端是否已消费并轮换 refresh token，若已轮换则新 token 的响应可能丢失，即使不删除本地凭据也可能永久失去认证。当前 API 没有可恢复已提交旋转结果的机制或服务端幂等保证，直接贯穿 should_abort 会引入主动丢失凭据风险，不满足安全实施条件。需先设计刷新所有权、取消后持续接收并发布结果及客户端生命周期，或确认服务端提供可恢复语义；不可仅断开请求。未实现单飞，保持 in-progress。

验证：make -j4 test-openai-codex test-grok test-subagent 与 git diff --check 通过；未增加发送中取消 mock，因为未实现该行为，且 mock 不能证明服务端轮换安全。

## 2026-11-15 Codex 发布错误传播（部分修复）刷新 observer 改为返回状态；发布或保存失败时刷新返回失败、不标记认证失效，模型查询和 completion 的最终保存失败也向调用者返回错误。provider 级测试注入 observer 拒绝，验证新 token 在客户端销毁前仍在内存且未标为认证失效。专项 make -j4 test-openai-codex test-grok test-subagent 与 git diff --check 通过。尚不满足完整目标：复制失败或版本冲突后短生命周期客户端仍会销毁唯一新凭据；保存失败仅在配置中保留内存值；未注入命令层复制或磁盘保存失败，未验证端到端错误与旧版本交错。服务端轮换后响应丢失、进程崩溃无法恢复；单飞和 Grok 尚未完成。保持 in-progress，不声称修复已完成。

## 2026-11-15 审查修正

Codex 最终保存区分旧版本丢弃（正值）与真实错误（负值），不会因旧快照改写成功或取消结果；锁内先校验版本再复制凭据。Grok 模型查询无论业务成功与否均尝试保存凭据，真实保存错误返回失败，旧快照仍丢弃。新增 HTTP mock 和屏障交错测试：Codex 完成成功及取消遇到并发版本变化，Grok 刷新后 catalog 503 时验证磁盘保存、catalog 成功和保存错误。

验证：make -j4 test-subagent test-openai-codex test-grok、make -j4 test、git diff --check 通过；未运行 TSAN 或真实网络。

未解决：Codex 刷新 observer 的复制失败或版本冲突可能导致短生命周期客户端销毁唯一轮换 token，磁盘保存失败仅保留内存；跨客户端刷新单飞、Grok 即时发布及并发唯一 token 保存仍未完成。任务继续 in-progress。

## 2026-11-15 Codex 所有权转移调查（未实施）未能在本轮安全实现 observer 发布修复，保留现有代码及 in-progress 状态。版本匹配时可考虑将客户端完整凭据与 config 字段在锁内交换，无需分配即可让共享配置持有旋转 token；但交换后客户端只拥有旧凭据，必须确保发布成功后不会以旧凭据继续 completion 或覆盖新配置，且重新复制失败时只能中止并保留共享值。磁盘保存失败必须明确报错且不能被最终保存逻辑掩盖。版本冲突时不能用 swap 覆盖已发布凭据，且多个并发刷新可能各自产生不同的新 token；单个备用槽不足以无损保存任意冲突结果。安全方案需要预先拥有的可转移存储（例如客户端内嵌链表节点，挂入 provider 生命周期）及 logout、provider 替换和释放时的明确处理，同时必须以不含秘密的消息提示重新认证；当前未设计和验证这些生命周期，不能直接销毁客户端，也不能简单泄漏或覆盖。另一个独立前置风险是 src/providers/openai_codex.c 的 credentials_from_token 在服务端已返回旋转结果后仍 strdup；分配失败会释放响应并丢失新 token，此问题早于 observer，单修 observer 不能覆盖该失败路径。本轮仅调查，无代码修改、无新增故障注入测试。验证：make -j4 test-subagent test-openai-codex test-grok 和 git diff --check 通过；测试仍未覆盖上述命令层所有权转移及冲突。未运行真实网络或 TSAN。

## 2026-11-15 Codex observer 同版本所有权前置

刷新 observer 在版本匹配时先在锁内无分配交换客户端与共享配置的凭据，增加版本，再持久化和复制供原客户端继续使用；持久化或复制失败返回错误，共享配置保留新 token，旧客户端版本不能在最终保存时覆盖新值。新增模型列表 HTTP mock，验证成功时请求使用新 access token，以及保存失败、复制创建失败时错误返回且客户端销毁后共享配置仍持有新 token。

此步骤不解决版本冲突时唯一 token 丢失、token 响应复制失败、进程退出前持久化失败、Codex/Grok 单飞及其并发网络测试。任务保持 in-progress。验证见本轮测试结果。

## 2026-11-15 协调发布完成前等待（部分前置）修正协调原语：刷新仍在进行时，即使凭据版本已改变，等待者也须等待结束或取消，避免在发布及持久化尚未结束时提前采用新版本；新增双线程测试验证发布后、结束前取消，以及结束后看到新版本。该更改尚未接入 Codex 真实刷新。直接接入仍需定义持久化失败时等待者的结果：现有 begin 只比较版本和 epoch，不能区分成功发布与发布后保存失败；同时需保证 invalid_grant 清理与结束顺序、旧快照保存不能与刷新所有权冲突，以及 token 响应解析分配失败后的轮换值保全。不能把当前版本变化误判为持久化成功。过期、401、列表真实单飞及其双 worker mock、业务失败和取消测试均未完成；Grok 原有风险不变。保持 in-progress。验证：make -j4 test-subagent test-openai-codex test-grok、make -j4 test 和 git diff --check 通过；未运行 TSAN 或真实网络。

## 2026-11-15 协调结果状态（未接入网络刷新）

Codex 协调原语按 epoch 保存失败结果，end 接收发布及持久化结果；等待者不会把失败后的版本变化视为成功，即使后续轮次已开始或结束。发布后失败时共享配置保留轮换 token，并将状态置为失败：后续新旧版本调用都返回错误，不自动重试或恢复，以免使用尚未确认持久化的凭据。失败但未发布时允许后续重试。双线程受控测试覆盖真实配置保存成功及重新读取、配置保存失败并保留内存 token、等待取消、失败后另一轮成功及后续调用；测试中同步点位于取消回调，不证明条件变量唤醒或真实网络单飞。

验证：make -j4 test-subagent test-openai-codex test-grok、make -j4 test 和 git diff --check 通过。协调接口尚未接入真实 Codex/Grok 刷新；失败后重新持久化和解锁恢复尚未实现，进程退出仍可能丢失内存 token；版本冲突、响应复制失败与网络轮换风险仍未解决。未运行 TSAN 或真实网络，任务保持 in-progress。

## 2026-11-15 Codex token 响应所有权前置（未完成单飞）

Codex 解析成功的 OAuth token 响应后直接转移 JSON 字符串所有权，避免逐字段 strdup 失败丢弃已轮换 token；认证及刷新复用该路径，新增所有权和 strdup 故障测试。专项 make -j4 test-openai-codex test-grok test-subagent 与 git diff --check 通过。JSON 解析本身分配失败、发布冲突及持久化失败仍有丢失风险；实际 completion / 模型列表协调接入、HTTP 双线程和取消测试均未实现，避免在未解决 invalid_grant 清理顺序、等待者复制和取消结果语义时引入不安全行为。未运行 TSAN 或完整测试，继续 in-progress。

## 2026-11-15 Codex 网络刷新协调接入（部分完成）

Codex completion 与模型列表的 provider refresh 接入现有按版本单飞：先竞争所有权，等待者取得已发布凭据快照；网络请求不持锁，发送后不中断并在唤醒等待者前发布及持久化。completion 等待期间可取消并返回取消状态；协调刷新失败不删除已有凭据。401/403 也经过同一协调入口，但其并发交错尚无专项测试。双线程本地 HTTP mock 覆盖模型列表刷新与 completion 同时预过期、只发送一次 token 请求、等待者取消、invalid_grant 失败保留旧凭据；检查 token 请求不可中途取消。专项 make -j4 test-subagent test-openai-codex test-grok、完整 make -j4 test 和 git diff --check 通过。

未验证 401/403 旧版本交错、真实网络、TSAN、持久化失败后的恢复及 JSON 解析分配失败；Grok 单飞未实现。环境覆盖 mock 未证明磁盘失败交错；服务端响应丢失和持久化失败后进程退出仍可能丢失轮换 token。任务保持 in-progress。

## 2026-11-15 Codex 旧 HTTP 认证失败交错验证

新增离线 HTTP mock 屏障测试：两个独立 worker 均以旧 access token 发出请求并收到 401 或 403；先释放一个响应，确认其刷新并以新 token 重试后，再释放另一个旧响应。覆盖双 completion 及 completion 与模型列表交叠，均断言两个新 token 请求且只有一次 token 刷新。扩展预过期的列表与 completion 交错，在真实配置保存失败时验证等待者和刷新者均返回失败且共享内存仍保留新 refresh token；既有交错覆盖 invalid_grant 后等待者和等待取消。没有发现需修改的实现。

验证：make -j4 test-subagent、make -j4 test、git diff --check 通过；独立 GCC ThreadSanitizer 构建的 test_subagent 运行通过，无报告。HTTP 层使用函数 mock 而非实际网络；未覆盖真正的网络传输或服务端轮换。持久化失败仅保留进程内 token，退出后仍可能丢失；Grok 单飞、JSON 解析分配失败和其他既有风险仍未解决，保持 in-progress。

## 2026-11-15 Grok 失败保护前置（未完成单飞）Grok 命令层保存时不再因 invalid_grant 删除磁盘凭据，返回错误；独立 Grok 客户端行为未改。新增真实 provider 模型查询路径的离线 HTTP mock，确认 invalid_grant 返回失败且已保存的旧 refresh token 未删除。专项 make -j4 test-subagent test-openai-codex test-grok、完整 make -j4 test、git diff --check 通过。仍未接入 Grok 按配置代次单飞、即时发布、取消等待；未验证 Grok 双 worker、401 交错或持久化失败。Grok token 响应复制失败、并发刷新和最终保存失败仍可能丢失已轮换 token；completion 仍未传播最终保存错误。无法安全声称修复，保持 in-progress；未运行真实网络或 TSAN。

## 2026-11-15 Grok 同版本即时发布前置

Grok 成功刷新后立即在业务请求前按版本检查并无分配交换客户端和配置凭据，持久化成功后重新复制到客户端继续请求；保存或复制失败返回错误且共享配置保留新 token，completion 也传播最终保存错误。OAuth JSON 字符串直接转移以避免逐字段 strdup 丢失 token。独立客户端未设置 observer 时仍可刷新。本地 HTTP mock 验证模型请求前新凭据已进入配置、成功落盘及请求使用新 token、业务 503 后凭据保留、保存失败后配置保留新 token；provider 故障注入验证令牌字段无需 strdup。专项 make -j4 test-grok test-subagent test-openai-codex 通过，git diff --check 通过。

尚未接入 Grok 单飞：并发刷新发生版本冲突时唯一轮换 token 仍可能随短期客户端销毁，不能宣称全面保全；版本达到 UINT64_MAX 同样无法发布。保存失败后进程退出仍可能丢失 token，JSON 解析分配失败及网络响应丢失未解决；clay_openai_set_api_key 静默忽略复制失败，可能以旧 access token 发请求。未验证真实网络或 TSAN，需继续处理协调、冲突保全和失败恢复。

## 2026-11-15 Grok 单飞再次调查（未实现）已复核 Grok 即时发布、Codex 按版本协调和真实 provider 路径测试；Grok 的 refresh 仍未接入协调，模型列表仅在过期时刷新，401 响应没有刷新重试。完成路径取消还需处理通用 OpenAI 401 刷新回调的取消结果；clay_openai_set_api_key 分配失败静默保留旧 token。未安全完成实现或新增双 worker 网络 mock，未修改源代码；保留原有未提交工作和 in-progress。专项 make -j4 test-grok test-openai-codex test-subagent、完整 make -j4 test 及 git diff --check 通过；未运行 TSAN 或真实网络。并发旋转、唯一 token 丢失和持久化失败风险仍然存在。

## 2026-11-15 Grok 生产单飞（部分完成）

Grok completion 与模型列表按独立 provider 版本和 epoch 协调预过期及 completion 的 401 刷新；网络在锁外，成功先即时发布并持久化，等待者采用新凭据并更新实际 OpenAI 客户端，更新失败不重放旧 token；等待可取消，刷新失败唤醒等待者，持久化失败保留内存新 token 并阻止自动重试。独立客户端仍可使用；版本或 epoch 耗尽时不发送刷新。受控 HTTP mock 覆盖模型列表与 completion 双 worker 预过期、取消、invalid_grant、保存失败，以及两个 completion 的旧 401 交错和新 token 重放，各断言仅一次刷新。模型列表本身的 401 刷新未实现。

验证：make -j4 test-grok test-openai-codex test-subagent、make -j4 test、git diff --check 通过。未测试真实网络、TSAN、401 等待取消和 OpenAI key 复制失败注入；服务端响应丢失、JSON 解析分配失败、磁盘保存失败后退出及既有 Codex 风险仍存在，保持 in-progress。



## 2026-11-16 Grok 模型列表 401/403 刷新重试

clay_grok_list_models 在 401/403 时经 refresh(client) 协调刷新后重试一次（grok.c:482-510）；测试 tests/test_grok_refresh.c 新增序列 mock 用例（401 → 刷新 200 → models 200）；Makefile test_grok_refresh 链接去掉 openai.o。验证：make -j4 test-grok、make -j4 test、git diff --check 通过。未做：TSAN、真实网络、首次 200 不刷新、403 分支、刷新失败分支等额外用例；clay-mcxf 其余未完成项仍 open，保持 in-progress。

## 2026-11-17 拆分计划

本 item 已拆为 #62-#75 共 14 个子项，覆盖 TSAN 基础设施、Codex/Grok 刷新与取消边界、持久化失败恢复、凭据锁与 provider 生命周期、故障注入与确定性交错测试、真实网络验证。所有子项完成前本 item 保持 in-progress；`issues check` 已验证关系一致。

---

## 迁移来源（lif）

- 原 ID: `clay-mcxf`
- 原类型: `bug`
- 原状态: `in-progress`
- 原优先级: `high`
- 原标签: 无
- 原父项: 无
- 原阻塞于: 无
- 原阻塞: `clay-dxtk` → #51
- 原创建时间: `2026-11-15T07:42:54Z`
- 原更新时间: `2026-11-16T02:20:25Z`
- 原文件路径: `.lif/clay-mcxf--修复并发-worker-的-codexgrok-oauth-凭据竞.md`
