---
id: 76
title: 优化 token 消耗与 prompt cache 命中（对标 killi）
status: in-progress
priority: high
labels:
    - type:feature
    - area:tokens
relations:
    depends-on:
        - 77
        - 78
        - 79
        - 80
        - 81
        - 82
        - 83
        - 84
        - 85
        - 86
        - 87
        - 88
        - 89
        - 90
        - 91
    related-to:
        - 145
        - 146
        - 148
        - 149
created: "2026-11-17"
updated: "2026-11-20"
---

目标：以 killi（/var/home/msc802/code/harness/killi）的 prompt-cache 设计为参照，降低 clay 的输入 token 并让缓存命中可测、可回归。killi 侧证据：system prompt + 全部工具 11,714 字符 ≈2,900 tokens（docs/decisions.md:481，tests.rs:5-16 强制预算）；历史只追加、工具列表会话内冻结（decisions 9/26，tools/mod.rs:1-2）；压缩阈值按窗口 60%、85% 上限、输出预留 ≤limit/4、20% 增长门槛、保留用户请求（compact.rs:4-67）；usage 逐请求记录并带 model/context_window（agent/request.rs:182-214），scripts/sessions.py 统计缓存率与 cache-miss streak。

clay 现状（HEAD 080b070）：系统提示基座 9.7KB + 18 个工具 ~8.5KB ≈4.5k tokens（context.c:13-231、message.c:1361-1443）；prompt_cache_key 只对 provider openai 生效（context.c:1678-1683）；Codex 把全部 system 提升到 instructions，环境/notes 一变即清空前缀（openai_codex.c:651-659）；自动压缩阈值用的是本轮各 round 累加值而非已计费上下文（message.c:1342-1350、1604-1612 对比 context.h:237-241）；/compact 不落 journal、摘要请求携带原始工具参数（compact.c:59-69、145-160）；无逐请求 usage 记录、无 /usage、无基准脚本。

阶段与子项：
A 度量：#77 逐请求 usage 与缓存归因 + /usage + -p --json；#78 离线基线基准与预算测试。
B 前缀稳定：#79 Codex system 提升；#80 环境块/notes 块副本治理；#81 指令与索引总量上限；#82 cache key 覆盖（cache_control 推迟）。
C 压缩与预算：#83 自动压缩触发语义；#84 /compact 重构；#85 预算单位与配置语义（折叠 + 提醒，不移植窗口百分比）。
D 工具与输出：#86 输出上限与续读提示；#87 edit/write 失败定位；#88 重复调用断路器（等 #77 数据）；#89 提示与工具瘦身；#90 MCP 工具收敛（先量字节）。
E 子代理：#91 子代理记账与去重。

执行顺序（ponytail 评审后重排）：#78/#96/#97 → #92-95(#77) → #99-101(#79) → #112-115(#83) → #116-119(#84) → #102/#104(#80) → #105-107(#81) → #120-122(#85) → #123-129(#86/#87) → #89 → #90 → #91；#88 与 #130-132 仅在 #77 的重复率数据支持时才做，否则关闭。

总验收（拆成两条，避免把离线断言当成真实命中）：
1. 离线：同一构建下请求前缀逐字节相等（环境/notes 变化后仍相等），提示 + 工具的字节预算由 make test 中的测试锁定。
2. 真实运行：一次手动会话记录命中数字（provider、模型、日期、前后对比），写进本项进展；不做 CI 断言。

约束与结论（评审确认）：
- 每个新增测试必须加入 Makefile 的 UNIT_TEST_TARGETS（Makefile:186），否则验收写着「make test 生效」也会假通过。
- 系统提示在 chat 创建时写进 journal 后冻结（include/clay/chat.h:26、src/chat.c:315）：#81/#89/#105 只对新建 chat 生效，老 chat 用 /resume 看不到变化；需要时应另加显式重建命令，不要在本项偷偷重建。
- 字节与 token 的换算只写一次：killi 的预算断言是 12,000 字符（crates/killi/src/tests.rs:5-16），约 3k token；不要写成「3k 字符」。
- 引用 killi 时写全路径（crates/killi/src/agent/compact.rs 等）；docs/decisions.md 的编号有重复，引用同时给日期与标题。
- 估算函数复用 src/commands/scope.c 的「请求字节 ÷ 2 + 输出预留」（scope.c:343-346 注释），不要再造第三种估算。

全部子项完成前本项保持 in-progress。

## 2026-11-18 实测：重复调用分布（#88 的前置测量）

#88 要求先证明模型真的在重复调用，再决定是否实现断路器。用 `scripts/repeat_report.py` 统计 16 个真实 chat 日志、20 个有工具调用的 turn、399 次调用：

```
397 distinct fingerprints (same turn only)
>=2: 0 fingerprints, 0 calls skipped (0.0%)
>=3: 0 fingerprints, 0 calls skipped (0.0%)
>=8: 0 fingerprints, 0 calls skipped (0.0%)
```

334 次 `shell_exec` 对应 334 个不同命令。**结论：clay 当前的模型在这些会话里从不重试同一调用**，killi 的“第 3 次阻断 / 第 5、3 次警告”在这里没有可拦对象。#88、#130、#131、#132 因此关闭；脚本保留，换模型或换场景时可重跑。

局限：单一用户、16 个会话、模型/provider 组合有限。

## 2026-11-18 真实运行测量（总验收第 2 条）

环境：provider `custom`（本机 OpenAI 兼容代理 `http://127.0.0.1:8787`，DeepSeek 上游），model `deepseek/deepseek-v4.1-flash`，reasoning_effort `max`，sandbox `unleashed-auto`。clay 构建：`25508a5`（放账本的前一个提交，四个 prompt/tool 瘦身提交之前）对比 `7e47e5a`（今日 HEAD，含 #79–#84/#86/#87/#89 全部瘦身）。两次测量都用同一个隔离 HOME（只拷 config.json 与 providers/，不带全局 AGENTS.md 与 memory），同一个 3 条消息脚本，工作区 `a.txt`/`b.txt`/`README.md`，各跑 3 次：

```
只回答 ok
读 a.txt 和 b.txt，用一句话说明内容
列出当前目录下的文件名，只要名字
/usage
```

每个会话 3 轮用户输入、3 次工具调用（read、read、shell_exec），两版轮次结构相同。

| 版本 | 请求数 | 输入 | 其中缓存 | 非缓存输入 | 输出 | 命中率 | 首个请求输入 / 缓存 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 25508a5（瘦身前） | 5 | 40,475 / 40,411 / 40,379 | 40,064 / 39,936 / 40,064 | ~500 | 423 / 415 / 274 | 99.0 / 98.8 / 99.2 % | 7,865 / 7,808 |
| 7e47e5a（今日 HEAD） | 5 | 39,447 / 39,457 / 39,449 | 38,784 ×3 | ~660 | 276 / 256 / 295 | 98.3 ×3 | 7,677 / 7,552 |

逐请求输入（一次运行的代表值）：

```
25508a5  7865  7950  8119  8221  8320
7e47e5a  7677  7762  7907  8005  8098
```

结论：

1. 前缀稳定（验收 1）在真实运行里成立：5 次请求里每次的缓存命中都覆盖了上一次的前缀，命中率 98–99 %，没有出现「某一轮清空前缀」的缓存 miss streak。
2. 瘦身把首请求少了 188 tokens（−2.4 %），整个会话少约 1,000 输入 tokens（−2.5 %）；非缓存部分从约 400 涨到约 665（每次请求约 120–180 tokens 的尾巴），因为工具 schema 变短后前缀边界落在不同 token 块上。净收益是小幅正向，不是之前离线预算（18,192 → 17,244 B）暗示的 5 % 量级。
3. 命中率的上限受提供方缓存粒度限制：缓存按 128/256 token 块计，非缓存尾巴每次约 120–180 tokens，这部分无法靠 clay 消掉。

测量过程中发现并修复了一个记账缺陷（#147）：该代理在一次 SSE 响应里发两个 usage 块，clay 每块都记账一次，旧账本上每次请求记两遍——上面 25508a5 的原始账本是 8 条记录、64,6xx 输入；表格里的数字是把相邻重复记录去掉后的同一请求。修复在 `src/providers/openai.c`：usage 现在在响应结束时投递一次，取最后一个块。
