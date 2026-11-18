---
id: 145
title: 评估由 dquickts 运行的 stem agent 落地路径
status: open
priority: medium
labels:
    - type:feature
    - area:tools
    - area:tokens
relations:
    related-to:
        - 76
created: "2026-11-18"
updated: "2026-11-18"
---

结论：可行，但 dquickts 不是 stem agent 的必要条件。stem agent（getvix.dev/docs#stem-agents）的定义是「通用 system prompt + 每阶段用 user message 下发指令」，靠跨阶段复用同一份 prompt cache 前缀省钱；它是一种提示编排，与运行时无关。clay 已经具备 stem 所需的三块地基：

1. system prompt 在一个 chat 生命周期内冻结（src/commands/context.c:1556 注释「prompt is frozen for the life of a chat」），阶段切换不会改写前缀。
2. cache key 取 chat id（src/commands/message.c:1614、src/commands/subagent.c:200），主会话与子代理共用，跨阶段稳定。
3. 每轮的环境信息作为独立 system 消息追加在数组末尾，不插在中间（src/commands/context.c:1974-1986 append_context_block），前缀只增不减。

缺的只是「阶段」这个概念：把 explore/plan/execute 的阶段说明作为 user 消息注入同一会话，并让各阶段共用同一 system prompt 与同一 cache key。这部分不需要新依赖。

dquickts（github.com/stencil-hq/dquickts，QuickJS fork，MIT）的价值在另一处：它给的是可跨进程续跑的 JS 运行时。本机实测（/tmp/dqjs/，源码 /var/home/msc802/code/c/rt-eval/dquickts，裁剪掉 Python/Intl/Temporal，只编 quickjs+dtoa+libregexp+libunicode+cutils）：

- TypeScript 直接执行，无需转译（add(2,3)=5）。
- 快照写读跨进程恢复精确（循环求和 done=124999750000 无误）。
- durable host call 闭环：宿主 C 函数返回 JS_SuspendCall 挂起 → 快照 → 另一进程 JS_ReadSnapshot → JS_ResumeWith 注入工具结果 → 脚本续跑打印 body=TOOL_RESULT_42。
- 护栏可用：JS_SetInterruptHandler + JS_GetProgramBranchCount 做精确分支预算（1000 次循环=2001 分支），无限循环被打断（ticks=12），JS_SetMemoryLimit 拦得住分配洪水。
- 裁剪核心无 os/std/console/require/fetch/process，能力面全靠宿主注入，这正是沙箱想要的形状。

代价与障碍（按严重度）：

1. 二进制体积。dquickts 核心静态库 1.2MB，链接后最小宿主 955KB（strip 后 855KB）；clay 现在 strip 后 385KB。embed 后约 1.2MB，与 README「单文件 ~90% KB compressed」的卖点冲突。clay 没有 vendoring 传统（CODESTYLE.md：除 libcurl 外无第三方依赖，libcurl 也是动态链接不打包），引入静态链接的 JS 引擎属于改产品承诺。
2. 许可。dquickts 引擎 MIT，可 vendoring；但 vix 是 AGPL-3.0，只能借鉴其「阶段指令用 user message 下发」的思路，不能抄代码。
3. 快照只认同版本构建（同引擎、同 natives 注册表、同字长），且 natives 顺序换错不报错只换错人；恢复后必须重设 interrupt handler、内存与栈限制。宿主契约要逐条对齐 README「The host's side of the contract」。
4. 线程。clay 的子代理并行跑（最多四个），dquickts 一个 runtime 只允许一个挂起执行，每个并行分支需独立 runtime。
5. 交叉编译。本机没有 mingw-w64，未验证 quickjs.c 在 Windows 构建下通过。

建议路径（先做便宜的一半）：

- 第一步（无新依赖）：给 clay 加「阶段」概念——同一 chat 内用 user 消息下发阶段说明，共享 system prompt 与 cache key，并让 /plan 从「模式开关」变成「阶段」。这是 stem 的本体，收益直接落在 prompt cache 命中率上，且可复用 issue #76 那条 token 优化链。
- 第二步（可选，代价大）：若确实需要跨进程续跑，再把 dquickts 作为进程内 JS 运行时引入，通过 host call 把 read/write/edit/shell 暴露给模型生成的代码。届时须同时决定：接受二进制涨到 1.2MB、把引擎做成外部进程还是静态链接、以及快照文件与引擎版本的绑定策略。

先做第一步。若第一步的缓存命中率收益已达标，第二步的动机就不成立。

## 2026-11-18 可行性评估

实测环境：/var/home/msc802/code/c/rt-eval/dquickts（fa5fe79），/tmp/dqjs/ 下的四个探针程序。裁剪构建命令：gcc -std=gnu11 -Os -fwrapv -D_GNU_SOURCE -DCONFIG_VERSION=\"2026-06-04\" -I. -c {quickjs,dtoa,libregexp,libunicode,cutils}.c。注意 quickjs.c 用 -std=c11 编译会因 asm volatile 报错，需 gnu11（clay 本身就是 gnu11，无冲突）。
