---
# clay-m0er
title: 修复 subagent 终端取消事件归属
status: in-progress
type: bug
priority: normal
created_at: 2026-11-15T07:29:20Z
updated_at: 2026-11-16T02:14:14Z
---

修复 Ctrl-O 阻塞取消及跨批次取消残留，增加可控终端输入测试。

审查结果：尚未修复。subagent 的 should_abort 在 batch 已取消后停止读取；clay_term_take_escape 被 pending Ctrl-O 短路，且 Escape 双击可留在 g_pending_escape，Escape 与 SIGINT 的短路求值会留下中断。简单批末清空输入会吞掉下一次用户输入，需先定义批次内事件归属与结束边界，并使用真实 PTY 覆盖。现有 test_subagent 和 test-pty 通过，但不覆盖这些场景。

已局部修复：subagent 按主请求语义消费 Ctrl-O，PTY 验证随后 Escape 可取消，取消后输入未被批末清空。双 Escape/SIGINT 跨批次归属仍未解决；现有终端轮询会丢弃运行期间的普通按键，尚未修复。

## 继续修复记录

批次运行时持锁轮询取消，不因已取消或 Escape 短路跳过 SIGINT；立即消费同次解码出的第二个 Escape。所有 worker 的 finished 均在锁内观察到后停止轮询，不做批末输入清空。PTY 覆盖双 Escape、Escape+SIGINT、取消后再输入 Escape/SIGINT、下批无残留和批次结束后普通输入。

尚未完成：终端字节与 SIGINT 没有可供归属的发生时间戳；最后一次轮询与 worker 结束并发时无法区分新旧输入，Escape 解析也可能读取其后到达的普通字符。当前轮询还会丢弃运行期间普通按键，因此不能保证不吞新输入，需设计可保留输入及明确归属边界后再关闭。验证：make test-subagent test-pty 通过，git diff --check 通过。
