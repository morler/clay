---
id: 23
title: 加固 Linux grep 的 FD 读取
status: in-progress
priority: medium
labels:
    - source:lif
    - type:task
    - parent:15
relations:
    blocks:
        - 29
    depends-on:
        - 16
        - 25
    related-to:
        - 15
created: "2026-11-17"
updated: "2026-11-17"
---

修改 src/commands/fs_tools.c、src/commands/workspace_fd.c、include/clay/workspace_fd.h 及 tests/test_fs_tools.c；避免检查后按路径重开，兼顾 sandbox/unleashed、正则、过滤和性能。验收：保留基线；正常和强制回退交换诱饵不越过契约，记录性能及限制。

## 进行中：调查记录已复核 clay-07dm 基线和 Linux grep、sandbox 执行、描述符遍历实现。当前 grep 仍将检查后的原始路径交给外部 grep；直接把目录 FD 交给递归 grep 只能固定起点，不能证明递归打开每个子项时不会发生符号链接交换。逐文件安全打开后交给 grep 还需解决 sandbox 中 FD 继承、目录搜索的文件名与 glob 语义、总输出上限和超时等契约，不能用未经验证的简化方案替代。复跑 make test-fs-tools（0.83 秒，最大 RSS 6056 KiB）及 make test-fs-fallback（0.95 秒，最大 RSS 6288 KiB），均通过，但两者都报告 grep race baseline: outside content exposed；回退测试另有既有 LEAK function 1 /outside/secret.c 输出。这些只是现状基线，不是修复或性能对比。尚未更改代码或安全回归，保持 in-progress。

## Linux FD 方案评估（继续进行，未修复）

当前缺陷是竞态读取工作区外内容，不是内存越界。根目录 FD 不能约束外部递归 grep 打开后代；现有 glob 遍历会跳过 .git、限制匹配数量、匹配语义不同，且枚举后按路径重开仍有竞态。

逐文件 stdin 方案：每个候选从工作区 root FD 使用 openat2 的 RESOLVE_BENEATH、RESOLVE_NO_SYMLINKS、RESOLVE_NO_MAGICLINKS 打开并 fstat 常规文件，dup2 到 grep stdin；保留 -E -n -I，目录输出补原始相对文件名、单文件不加文件名。需对照 GNU grep --include 的显式文件、斜线、隐藏文件及通配符语义，空目录也必须验证非法正则，保留错误、二进制、顺序与无匹配行为。现有 sandbox 和 shell 执行器没有输入 FD 接口；须显式白名单继承 FD、避免泄漏，sandbox 失败不能降级，整个搜索共享单调时钟 120 秒期限、4 MiB 捕获及 64 KiB 预览和 scratch，截断后仍需安全消费管道。

性能粗测：本机 /usr/bin/grep 搜索 300 个两行小文件，递归单次 0.003479 秒、逐文件 subprocess.run 加 stdin 0.274801 秒，约 79 倍；未包含 sandbox 创建、shell 和输出捕获开销，仅单次测量。不能假定逐文件进程满足性能，需真实大仓库和 sandbox 测量并确定阈值。

可执行批处理方向：父进程安全打开有限批文件，通过明确 FD 白名单传入一次 sandbox，运行 grep -E -n -I -H -Z 搜索 /proc/self/fd/N，由受控中间层解析 NUL 分隔的 FD 标签并映射原文件名；单文件移除标签。现有捕获层将 NUL 转成问号，须在转换前解析；批大小受 RLIMIT_NOFILE 和 argv 限制，逐批共享总时限和输出预算。应对照测试含冒号或换行文件名、二进制、错误、空目录和 glob，验证 sandbox 的 /proc、FD 继承及权限并测端到端性能；这不是已验证实现。

安全障碍：CLAY_NO_OPENAT2 的组件式 openat 和 O_NOFOLLOW 虽防符号链接，但已打开的中间目录可能移出工作区后再从中打开文件，不能证明强制回退目录移动时的工作区边界。需要明确回退契约，例如不支持时失败关闭并评估兼容性，以及目录移动、硬链接、挂载的威胁边界。

本次 make test-fs-tools、make test-fs-fallback、git diff --check 返回成功，但两种 grep 均输出 grep race baseline: outside content exposed，仅是现状复现，不是安全通过；回退 repo-map 仍输出 LEAK function 1 /outside/secret.c。未改源代码或测试，保留既有未提交修改。后续须增加不依赖 PATH 的确定性起始和中间目录交换测试，覆盖 unleashed、可用 sandbox 和强制回退，再验证功能、捕获上限、超时及性能。

## FD 批处理原型（继续进行，未实现）已检查实际 clay_fs_tool_grep → clay_sandbox_exec → bash/grep 路径：sandbox 内新挂载 /proc；fd_read 返回 O_CLOEXEC，现有执行器无 FD 白名单，捕获把 NUL 改成问号后才返回；还需完整处理资源上限、继承和关闭，以及 stderr 与 stdout 混流。不能只传目录 FD：后代仍由 grep 按路径打开。临时非生产原型使用单个 GNU grep -EnIHZ 检索两个 O_CLOEXEC 文件 FD，通过 subprocess pass_fds 传入 /proc/self/fd/3、4；分别含冒号和换行的原始文件名可逐条重映射，每文件两条匹配；打开后重命名第一个文件并用 secret 替换旧路径，输出仍为打开文件的 hit1/hit2。主机 RLIMIT_NOFILE 软 1024、硬 524288；这里只验证两 FD，未确定安全批量上限。grep -Z 每条匹配都有 FD 标签和 NUL，不能简单按 NUL 两两分组。另用临时 C 程序调用实际 clay_sandbox_exec：同一文件 FD 设置 CLOEXEC 时 rc=0、exit=2、grep 报 /proc/self/fd/3: No such file or directory；清除 CLOEXEC 时 rc=0、exit=0，得到 /proc/self/fd/3?1:hit，其中 ? 是捕获层替换的 NUL。证明本机 sandbox /proc 和继承可用，但需子进程定向继承、避免其他 FD 泄露，不能全局移除 CLOEXEC；禁止失败后重开原路径。现有 grep -E 正则、二进制、单文件与目录文件名前缀、GNU --include 对显式文件和递归候选的语义、含特殊字符文件名、空目录非法正则、退出码及 stderr、4 MiB 捕获和 64 KiB 预览/scratch 截断、多个批次共享 120 秒总时限和超时必须保持；NUL 消失与 stderr 混流阻碍可靠重映射，截断也可能切断记录。FD 批量由 RLIMIT_NOFILE、执行器内部 FD、进程并发和 argv 上限共同约束；需逐批关闭、共享预算并在所有错误路径关闭；限定常规文件并安全枚举。当前组件式 openat 回退在打开中间目录后被移出根目录的竞态不满足完整边界，不能声称强制回退已安全。硬链接和挂载仍需按现有威胁模型说明。本轮未改生产行为或将泄露基线伪装为通过：make test-fs-tools、make test-fs-fallback、make test-sandbox 返回成功，正常和强制回退都报告 grep race baseline: outside content exposed。回退 repo-map 测试还打印 LEAK function 1 /outside/secret.c，该行是测试脚本输出，不能单凭这一行认定 repo-map 实际泄漏。现有测试只是打印基线而非不泄露断言，任务保持 in-progress；需先设计二进制安全且 stdout/stderr 可区分的捕获及受控 FD 继承，解决回退安全契约，再添加确定性起点/中间目录交换的非泄露回归与性能/超时覆盖。

## 二进制捕获基础实现（仍未修复 grep）
新增 include/clay/term.h 的 ClayExecOptions.preserve_output_nul；src/render/term.c 和 src/sandbox/linux.c 在选中时保留 NUL，包括 on_output；默认仍替换成问号。tests/test_sandbox.c 对 unleashed 和本机 sandbox 验证 NUL、回调、限额截断及默认文本行为。make test-sandbox、make test-fs-tools、make test-fs-fallback 和 git diff --check 返回成功；正常和回退 grep 测试仍报告 grep race baseline: outside content exposed，回退 repo-map 输出 LEAK function 1 /outside/secret.c 是测试脚本输出，不能仅凭该行断定泄漏。
此接口只是必要基础：src/sandbox/linux.c:562-563 和 src/render/term.c:410-411 仍将 stdout/stderr 合并，错误可伪造 grep FD 标签；超时消息也混入输出，截断可切断记录。src/commands/fs_tools.c:1055 仍使用旧路径和默认捕获；不能据此声称任意文件名已安全重映射。后续需实现分离 stdout/stderr 的有界并发捕获、在子进程按白名单继承文件 FD、仅在完整记录且未超时/截断时解析并映射标签；处理错误与预算，安全枚举并验证目录移动的强制回退契约，再补特殊文件名、竞态及性能测试。Windows integrated shell 此选项行为尚未验证。

## 受控 FD 继承源码核查（未实现）现有 src/sandbox/linux.c 的首次 fork 后 setup 需要握手 FD；共享 namespace 需要 user_fd/net_fd；随后再次 fork 并可能直接运行 integrated shell 的内建命令，因此仅设置 CLOEXEC 不能阻止访问其他 FD。src/render/term.c 的 unleashed 分支也没有关闭其他非 CLOEXEC FD。sandbox 父进程 run_id_mapper 独立 fork/exec 外部程序，同样可继承非 CLOEXEC FD；不能在父进程临时清除白名单的 CLOEXEC，否则并发 fork 或 mapper 可能泄露。内部 pipe、传入 FD 和标准流可能冲突，dup2 和 close 必须检查。最小方案：Linux opt-in 白名单由调用者在整个调用期间保持有效，拒绝小于 3、重复及失效的 FD；fork 后用 close_range 按排序白名单间隙关闭其他 >=3 的所有 FD，而非依据 RLIMIT_NOFILE 枚举或仅标记 CLOEXEC；任何失败及不支持均失败关闭。sandbox 首次 fork 额外保留握手及共享 namespace FD，完成 setup 后运行 bash 或 integrated shell 之前再次只保留目标 FD，子进程清除白名单 CLOEXEC；unleashed 在重定向后使用相同清理规则，禁止更改父进程 FD flags。mapper 自身也需关闭 >=3 的所有 FD，确认其无额外依赖。明确 stdin 策略、标准流冲突、fork 后 libc 安全及非 Linux 显式拒绝。该方案尚未验证，不代表安全接口已实现。后续专项测试：两个 CLOEXEC 文件、非白名单非 CLOEXEC 文件和管道、高编号 FD，覆盖 unleashed、sandbox 新建与共享 namespace、integrated shell，断言仅白名单可读、父进程 flags 不变、重命名后读原 FD；检查空列表、失效重复 FD、标准 FD 冲突、close_range ENOSYS/EPERM、setup/exec 失败、并发启动、mapper 及资源清理。sandbox 不可用时不可将跳过视为通过。仍须解决 stderr 混流标签伪造、总时限预算及强制回退目录移动边界后再连接 grep。本轮未改生产代码，保留既有未提交工作。make test-sandbox test-fs-tools test-fs-fallback 及 git diff --check 返回 0，sandbox 测试实际执行通过；正常与回退 grep 都仍报告 grep race baseline: outside content exposed，不能视为安全通过。回退 repo-map 输出 LEAK function 1 /outside/secret.c 为测试脚本输出，不足以单独证明泄露。任务继续 in-progress。

## 本轮验证与未完成事项补充 tests/test_sandbox.c：分别在 unleashed 和实际可用的 Linux sandbox 中验证未截断 a\0b 捕获及流式回调；make test-sandbox 实际运行通过，git diff --check 通过。没有实现 FD 白名单，也没有将其接入 grep；安全问题仍未解决。代码级阻碍：src/sandbox/linux.c 的 run_child 在首次 fork 后依赖握手管道和共享 namespace FD，后续 fork 可直接调用 integrated shell；仅依赖 CLOEXEC 无法阻止内建命令读到无关 FD。run_id_mapper 另行 fork/exec，也需隔离 FD；而 src/render/term.c 的非沙箱子进程同样不关闭无关非 CLOEXEC FD。必须在子进程中保留正确的临时 setup FD，setup 完成后再次关闭非白名单 FD，并处理 close_range 不可用、标准 FD 和管道冲突及失败状态；父进程临时更改 CLOEXEC 会与并发 fork 竞态。当前 stdout/stderr 合流也不能安全用于 grep 的标签解析；强制回退目录移动边界仍未解决。未经过这些条件的实现不应作为安全完成交付。

## 安全断言红测（仍未修复）在 tests/test_fs_tools.c 的现有 PATH 替换复现上增加 CLAY_ASSERT_GREP_RACE_SAFETY=1 可选断言：确认起始目录已替换为符号链接，再断言输出不含 race-secret。默认测试仅保留基线输出。make test-fs-tools test-fs-fallback 和 git diff --check 通过；分别运行 CLAY_ASSERT_GREP_RACE_SAFETY=1 bin/test_fs_tools 与 bin-fs-fallback/test_fs_tools 均以 134 退出，泄漏断言失败。此红测仍依赖 PATH 包装器和 /usr/bin/grep，不能覆盖将来不调用外部 grep 的实现、sandbox 或中间目录交换；必须用独立于 PATH 的同步测试替换，缺少 grep 时不得跳过安全验收。当前没有安全生产修复：回退 parent_fd 可在固定的中间目录被移出根目录后继续打开文件；sandbox 的 FD 继承、混合 stderr、全局超时和输出预算仍须完整解决。为避免改变 grep 功能或削弱 sandbox，没有加入不完整生产补丁。

## 不依赖 PATH 的确定性红测（仍未修复）新增 __wrap_clay_sandbox_exec，在 grep 真正执行前把检查过的起始目录换成指向工作区外目录的符号链接；tests/test_fs_tools.c 的 test_grep_directory_race_baseline 断言交换已发生，并在 CLAY_ASSERT_GREP_RACE_SAFETY=1 时要求输出不含外部秘密。默认 make test-fs-tools 与 make test-fs-fallback 均通过并打印两条 baseline；开启断言后两构建都在新红测处失败，证明修复尚未落地。沙箱与中间目录交换仍未覆盖。生产阻塞不变：目录候选需安全枚举并逐文件固定 FD，执行器需定向继承白名单、分离 stderr、共享超时/输出预算，且 CLAY_NO_OPENAT2 回退的目录移出边界必须定义；在这些完成前不把不完整补丁接入生产路径。

---

## 迁移来源（lif）

- 原 ID: `clay-u610`
- 原类型: `task`
- 原状态: `in-progress`
- 原优先级: `normal`
- 原标签: 无
- 原父项: `clay-1m1e` → #15
- 原阻塞于: `clay-07dm` → #16, `clay-uxeh` → #25
- 原阻塞: 无
- 原创建时间: `2026-11-15T01:38:37Z`
- 原更新时间: `2026-11-16T02:14:14Z`
- 原文件路径: `.lif/clay-u610--加固-linux-grep-的-fd-读取.md`
