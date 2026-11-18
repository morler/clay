---
id: 143
title: 全局提示支持（用户级 AGENTS.md）
status: closed
priority: medium
labels:
    - type:task
    - area:prompt
created: "2026-11-18"
updated: "2026-11-18"
closed: "2026-11-18"
---

现状：clay_commands_load_project_instructions 只从 cwd 向上读到仓库根，因此能生效的指令最多是当前仓库里的 AGENTS.md/CLAY.md。用户级偏好（回复语言、提交信息风格、个人习惯）无处可放，放进 memory 要靠模型想起去查。

设计：`~/.clay/AGENTS.md`（同层 CLAY.md 作为回退，沿用 AGENTS.md 优先的既有规则）作为全局指令，拼在项目指令之前：全局最通用，项目可覆盖它。预算：全局 8 KB，项目保持 32 KB，各自独立上限；总量预算随之调整。

缓存：system_prompt.json 目前只按基座 hash + cwd 命中，改指令文件 10 分钟内不生效。把全局与项目指令内容一并纳入指纹，编辑后立即生效。

标签：新建 chat 生效，老 chat 需 /new（与 #81/#105 同一约束）。

验收：test_project_instructions 覆盖全局文件缺失/存在/超限/AGENTS.md 与 CLAY.md 同层优先级；test_token_budget 断言全局段不超预算且计入总量；make test 全绿。

## 2026-11-18 完成记录

改动文件：`src/commands/context.c`、`src/commands/context.h`、`tests/test_project_instructions.c`、`tests/test_token_budget.c`、`README.md`、`ARCHITECTURE.md`、`CHANGELOG.md`。

实现：
- `clay_commands_load_global_instructions()`：从 `clay_storage_root()` 读 `AGENTS.md`，没有则读 `CLAY.md`（复用既有的 `read_level_instructions`，优先规则与项目层一致），超过 8 KB 截断并写明 `N bytes of global instructions cut ... read ~/.clay/AGENTS.md for the rest`。
- `build_fresh_system_prompt()` 把全局段拼在项目段之前，标题 `Global user instructions (~/.clay/AGENTS.md):`。
- `ClayCommandsPromptSections` 新增 `global` 字段，`CLAY_PROMPT_BREAKDOWN=1` 的输出也带上。
- 指纹：`base_prompt_fingerprint()` → `prompt_fingerprint(global, project)`，FNV-1a 覆盖基座 + 全局 + 项目。`clay_commands_build_system_prompt()` 只读一次两个文件，同一份字节既进提示也进指纹。

预算：`CLAY_SYSTEM_PROMPT_BUDGET` 52 KB → 60 KB（新增 8 KB 段），实测满额 store 58,724 B。工具预算 18,000 不变（工具未动）。

验证：
- `make test` 全绿；逐个运行 44 个 UNIT_TEST_TARGETS，failures=0。
- `test_project_instructions` 新增：无文件为 NULL、CLAY.md 回退、AGENTS.md 优先、10 KB 截断含提示、以及端到端断言全局段排在项目段之前。
- 反证：把全局段改拼到项目段之后，端到端断言立即失败（`global_pos < project_pos`）。
- 实际运行：`HOME=<tmp> CLAY_PROMPT_BREAKDOWN=1 clay -p hi` 输出 `= base 9172 + global 73 + instructions 64`；改写全局文件后下一次运行立即生效（指纹失效），旧内容不再出现。
- 测试自清理：两处临时目录均无残留（过程中修的 3 个自清理 bug：`mkdtemp` 模板必须在文件作用域、`~/.clay` 子目录需显式 rmdir、`system_prompt.json` 缓存需先 remove）。

已知限制（沿用 #81/#105）：系统提示建 chat 时冻结，老 chat 需 `/new` 才看到新内容。
