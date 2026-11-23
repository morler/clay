#!/usr/bin/env python3
"""How many rounds does a turn take, and what does each round spend them on?

#148 wants the 64-round ceiling to become a budget that work rarely reaches,
which needs a number to move. The numbers in that issue came from a one-off
hand count; this reads the same logs again after every change.

Two sources, both read-only:

- `requests.jsonl` in each chat directory holds one record per provider
  request (`kind`, `round`, token counts, duration). A `round` that does not
  increase starts a new turn.
- `chat.json` holds the turns and the tool calls each assistant message made,
  which is what "calls per round", the tool mix and multi-target adoption
  are measured from.

    scripts/round_report.py                  # ~/.clay/chats
    scripts/round_report.py --root DIR
    scripts/round_report.py --root DIR --json
    scripts/round_report.py --self-test      # synthetic fixture, asserts the numbers

Only aggregate counts are printed: tool names and percentages, never message
text, command arguments, or chat ids. Damaged journals and unreadable lines
are counted and skipped, not fatal.
"""

import argparse
import collections
import glob
import json
import os
import re
import sys
import tempfile

# The provider loops from src/commands/context.h: one record per request, so
# a 64-round segment logs 64 records. A turn can run two segments plus one
# final summary, so a continued turn logs up to 129 rounds and `round` stays
# continuous across the segment boundary.
AGENT_MAX_ROUNDS = 64
AGENT_SOFT_ROUNDS = 48
# Commands a turn should need only a handful of: look up the next task, check
# what changed, run the suite. Counted per assistant message, not per call.
FIXED_COMMANDS = ("issues", "git", "make")
COMMAND_SEPARATORS = re.compile(r"&&|\|\||[;|\n]")
REQUESTS_FILE = "requests.jsonl"
CHAT_FILE = "chat.json"


def read_records(path):
    """Every JSON object in a JSONL file, plus the lines that were not one."""
    records, unreadable = [], 0
    try:
        with open(path, encoding="utf-8", errors="replace") as file:
            lines = file.read().splitlines()
    except OSError:
        return records, unreadable
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except ValueError:
            unreadable += 1
            continue
        if isinstance(record, dict):
            records.append(record)
        else:
            unreadable += 1
    return records, unreadable


def load_journal(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as file:
            journal = json.load(file)
    except (OSError, ValueError):
        return None
    return journal if isinstance(journal, dict) else None


def main_turns(records):
    """Splits `kind=main` records where `round` stops increasing."""
    turns, current, last = [], [], None
    for record in records:
        if record.get("kind", "main") != "main":
            continue
        round_number = record.get("round")
        if not isinstance(round_number, int):
            continue
        if current and round_number <= last:
            turns.append(current)
            current = []
        current.append(record)
        last = round_number
    if current:
        turns.append(current)
    return turns


def turn_cost(records):
    """Wall clock is first request start to last request end; the record's ts
    is when the request finished."""
    first, last = records[0], records[-1]
    started = first.get("ts", 0) - (first.get("duration_ms") or 0) / 1000.0
    ended = last.get("ts", 0)
    return {
        "rounds": len(records),
        "wall_seconds": max(0.0, ended - started),
        "input_tokens": sum(int(r.get("input_tokens") or 0) for r in records),
        "output_tokens": sum(int(r.get("output_tokens") or 0) for r in records),
    }


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def call_arguments(call):
    """The parsed arguments object of a tool call, or None when it has none."""
    arguments = call.get("arguments")
    if isinstance(arguments, str):
        try:
            arguments = json.loads(arguments)
        except ValueError:
            return None
    return arguments if isinstance(arguments, dict) else None


def shell_command(call):
    """The command string of a shell_exec call, or None when it has none."""
    arguments = call_arguments(call)
    if not isinstance(arguments, dict):
        return None
    command = arguments.get("command")
    return command if isinstance(command, str) else None


def multi_target(call):
    """Whether one call asked a tool to handle several targets at once."""
    arguments = call_arguments(call)
    if not isinstance(arguments, dict):
        return False
    for key in ("paths", "edits"):
        value = arguments.get(key)
        if isinstance(value, list) and len(value) > 1:
            return True
    return False


def runs_fixed_command(command):
    for segment in COMMAND_SEPARATORS.split(command):
        words = segment.strip().split()
        if words and words[0] in FIXED_COMMANDS:
            return True
    return False


def call_list(message):
    """The named tool calls of one assistant message."""
    return [
        call
        for call in message.get("calls") or []
        if isinstance(call, dict) and call.get("name")
    ]


def usage_for_calls(calls):
    """Shell habits and multi-target use over one message's calls."""
    shell_total = shell_cd = multi = 0
    fixed = False
    for call in calls:
        if multi_target(call):
            multi += 1
        if call["name"] != "shell_exec":
            continue
        command = shell_command(call)
        if command is None:
            continue
        shell_total += 1
        if command.lstrip().startswith("cd "):
            shell_cd += 1
        fixed = fixed or runs_fixed_command(command)
    return shell_total, shell_cd, fixed, multi


def journal_usage(journal):
    """Calls per assistant message, the tool mix, and shell habits."""
    calls_per_round, tool_names = [], collections.Counter()
    shell_total = shell_cd = fixed_rounds = multi = 0
    for turn in journal.get("turns") or []:
        if not isinstance(turn, dict):
            continue
        for message in turn.get("messages") or []:
            if not isinstance(message, dict) or message.get("role") != "assistant":
                continue
            calls = call_list(message)
            calls_per_round.append(len(calls))
            for call in calls:
                tool_names[call["name"]] += 1
            shell, cd, fixed, here_multi = usage_for_calls(calls)
            shell_total += shell
            shell_cd += cd
            fixed_rounds += fixed
            multi += here_multi
    return calls_per_round, tool_names, shell_total, shell_cd, fixed_rounds, multi


def share(part, whole):
    return round(100.0 * part / whole, 1) if whole else 0.0


def collect(root):
    chats = sorted(
        directory
        for directory in glob.glob(os.path.join(root, "*"))
        if os.path.isdir(directory)
    )
    turns = []
    unreadable_lines = non_main_requests = 0
    journal_errors = journals = 0
    calls_per_round, tool_names = [], collections.Counter()
    shell_total = shell_cd = fixed_rounds = multi_target_calls = 0
    for directory in chats:
        records, unreadable = read_records(os.path.join(directory, REQUESTS_FILE))
        unreadable_lines += unreadable
        non_main_requests += sum(1 for r in records if r.get("kind", "main") != "main")
        turns.extend(turn_cost(turn) for turn in main_turns(records))

        journal = load_journal(os.path.join(directory, CHAT_FILE))
        if journal is None:
            journal_errors += 1
            continue
        journals += 1
        calls, names, shell, cd, fixed, multi = journal_usage(journal)
        calls_per_round += calls
        tool_names += names
        shell_total += shell
        shell_cd += cd
        fixed_rounds += fixed
        multi_target_calls += multi

    rounds = [turn["rounds"] for turn in turns]
    walls = [turn["wall_seconds"] for turn in turns]
    calls_total = sum(calls_per_round)
    per_round = collections.Counter(
        "0" if n == 0 else "1" if n == 1 else "2" if n == 2 else "3+"
        for n in calls_per_round
    )
    return {
        "chats": {
            "scanned": len(chats),
            "with_journal": journals,
            "journal_errors": journal_errors,
        },
        "requests": {
            "turns": len(turns),
            "total": sum(rounds),
            "unreadable_lines": unreadable_lines,
            "non_main": non_main_requests,
            "rounds": {
                "median": percentile(rounds, 0.5),
                "p90": percentile(rounds, 0.9),
                "max": max(rounds, default=0),
            },
            "at_soft_limit": sum(1 for n in rounds if n >= AGENT_SOFT_ROUNDS),
            "at_soft_limit_share": share(
                sum(1 for n in rounds if n >= AGENT_SOFT_ROUNDS), len(rounds)
            ),
            "at_hard_limit": sum(1 for n in rounds if n >= AGENT_MAX_ROUNDS),
            "at_hard_limit_share": share(
                sum(1 for n in rounds if n >= AGENT_MAX_ROUNDS), len(rounds)
            ),
            "continued": sum(1 for n in rounds if n > AGENT_MAX_ROUNDS),
            "wall_seconds": {
                "median": percentile(walls, 0.5),
                "total": round(sum(walls), 1),
            },
            "input_tokens": sum(turn["input_tokens"] for turn in turns),
            "output_tokens": sum(turn["output_tokens"] for turn in turns),
        },
        "journal": {
            "assistant_messages": len(calls_per_round),
            "calls": calls_total,
            "calls_per_message": (
                round(calls_total / len(calls_per_round), 2) if calls_per_round else 0.0
            ),
            "calls_per_round": {
                key: per_round.get(key, 0) for key in ("0", "1", "2", "3+")
            },
            "tools": dict(tool_names.most_common()),
            "multi_target_calls": multi_target_calls,
            "multi_target_share": share(multi_target_calls, calls_total),
            "shell_share": share(tool_names.get("shell_exec", 0), calls_total),
            "shell_cd_share": share(shell_cd, shell_total),
            "fixed_command_rounds": fixed_rounds,
            "fixed_command_rounds_per_chat": (
                round(fixed_rounds / journals, 1) if journals else 0.0
            ),
        },
    }


def format_text(summary):
    chats = summary["chats"]
    requests = summary["requests"]
    journal = summary["journal"]
    counts = journal["calls_per_round"]
    tools = ", ".join(
        f"{name} {share(count, journal['calls']):.1f}%"
        for name, count in list(journal["tools"].items())[:6]
        if name != "shell_exec"
    )
    lines = [
        (
            f"chats: {chats['scanned']} scanned, "
            f"{chats['with_journal']} with a journal, "
            f"{chats['journal_errors']} unreadable"
        ),
        (
            f"requests: {requests['total']} in {requests['turns']} turns "
            f"({requests['unreadable_lines']} unreadable lines, "
            f"{requests['non_main']} non-main)"
        ),
        (
            f"rounds per turn: median {requests['rounds']['median']:.0f}, "
            f"p90 {requests['rounds']['p90']:.0f}, "
            f"max {requests['rounds']['max']}"
        ),
        (
            f"  >={AGENT_SOFT_ROUNDS}: {requests['at_soft_limit']} turns "
            f"({requests['at_soft_limit_share']:.1f}%)"
        ),
        (
            f"  >={AGENT_MAX_ROUNDS}: {requests['at_hard_limit']} turns "
            f"({requests['at_hard_limit_share']:.1f}%)"
        ),
        f"  over 64 requests (continued or retried): {requests['continued']} turns",
        (
            f"wall per turn: median {requests['wall_seconds']['median']:.0f}s, "
            f"total {requests['wall_seconds']['total'] / 3600:.1f}h"
        ),
        (
            f"tokens: input {requests['input_tokens']:,}, "
            f"output {requests['output_tokens']:,}"
        ),
        (
            f"assistant messages: {journal['assistant_messages']}, "
            f"calls {journal['calls']} "
            f"({journal['calls_per_message']:.2f} per message)"
        ),
        "  messages with 0/1/2/3+ calls: "
        + " / ".join(str(counts[key]) for key in ("0", "1", "2", "3+")),
        f"tools: shell_exec {journal['shell_share']:.1f}%, {tools}",
        (
            f"multi-target calls: {journal['multi_target_calls']} "
            f"({journal['multi_target_share']:.1f}%)"
        ),
        f"shell: {journal['shell_cd_share']:.1f}% of calls start with cd",
        (
            f"fixed commands ({'/'.join(FIXED_COMMANDS)}): "
            f"{journal['fixed_command_rounds_per_chat']:.1f} messages per chat"
        ),
    ]
    return "\n".join(lines)


def write(path, text):
    with open(path, "w", encoding="utf-8") as file:
        file.write(text)


def check(condition, detail):
    """The self-test assertions stay real checks under python -O too."""
    if not condition:
        raise SystemExit(f"round_report self-test failed: {detail}")


def check_close(actual, expected, detail):
    check(abs(actual - expected) < 0.001, f"{detail}: {actual} != {expected}")


def self_test():
    with tempfile.TemporaryDirectory(prefix="round-report-") as root:
        chat = os.path.join(root, "chat-one")
        os.makedirs(chat)
        records, timestamp = [], 1000
        for round_number in range(64):
            records.append(
                {
                    "ts": timestamp,
                    "kind": "main",
                    "round": round_number,
                    "input_tokens": 100,
                    "output_tokens": 10,
                    "duration_ms": 1000,
                }
            )
            timestamp += 1
        for round_number in range(4):
            records.append(
                {
                    "ts": timestamp + 50,
                    "kind": "main",
                    "round": round_number,
                    "input_tokens": 50,
                    "output_tokens": 5,
                    "duration_ms": 1000,
                }
            )
            timestamp += 1
        records.append({"ts": timestamp, "kind": "subagent", "round": 0})
        write(
            os.path.join(chat, REQUESTS_FILE),
            "".join(json.dumps(record) + "\n" for record in records) + "not json\n",
        )
        journal = {
            "turns": [
                {
                    "messages": [
                        {"role": "user", "text": "go"},
                        {
                            "role": "assistant",
                            "calls": [
                                {
                                    "name": "shell_exec",
                                    "arguments": json.dumps(
                                        {"command": "cd /w && make test"}
                                    ),
                                }
                            ],
                        },
                        {
                            "role": "assistant",
                            "calls": [
                                {
                                    "name": "read",
                                    "arguments": '{"paths": ["a.c", "b.c"]}',
                                },
                                {"name": "grep", "arguments": '{"pattern": "x"}'},
                                {
                                    "name": "shell_exec",
                                    "arguments": '{"command": "issues next --format json"}',
                                },
                            ],
                        },
                        {"role": "assistant", "calls": []},
                    ]
                }
            ]
        }
        write(os.path.join(chat, CHAT_FILE), json.dumps(journal))

        damaged = os.path.join(root, "chat-two")
        os.makedirs(damaged)
        with open(os.path.join(damaged, CHAT_FILE), "wb") as file:
            file.write(b'{"turns": [\xff\xfe\x00')
        write(os.path.join(damaged, REQUESTS_FILE), '{"round": 0}\n')

        summary = collect(root)
        requests, stats = summary["requests"], summary["journal"]
        check(summary["chats"]["scanned"] == 2, summary)
        check(summary["chats"]["with_journal"] == 1, summary)
        check(summary["chats"]["journal_errors"] == 1, summary)
        check(requests["turns"] == 3, requests)
        check(requests["total"] == 69, requests)
        check(requests["unreadable_lines"] == 1, requests)
        check(requests["non_main"] == 1, requests)
        check(requests["rounds"]["max"] == 64, requests)
        check(requests["at_soft_limit"] == 1, requests)
        check(requests["at_hard_limit"] == 1, requests)
        check(requests["continued"] == 0, requests)
        check_close(requests["at_hard_limit_share"], 33.3, requests)
        check(requests["input_tokens"] == 6600, requests)
        check(stats["assistant_messages"] == 3, stats)
        check(stats["calls"] == 4, stats)
        check(stats["calls_per_round"] == {"0": 1, "1": 1, "2": 0, "3+": 1}, stats)
        check(stats["multi_target_calls"] == 1, stats)
        check_close(stats["multi_target_share"], 25.0, stats)
        check(stats["tools"] == {"shell_exec": 2, "read": 1, "grep": 1}, stats)
        check_close(stats["shell_share"], 50.0, stats)
        check_close(stats["shell_cd_share"], 50.0, stats)
        check(stats["fixed_command_rounds"] == 2, stats)

        encoded = json.dumps(summary, sort_keys=True)
        check("go" not in encoded and "a.c" not in encoded, "no message text in output")
        check("chat-one" not in encoded, "no chat ids in output")
        text = format_text(summary)
        check("2 scanned" in text and ">=" + str(AGENT_MAX_ROUNDS) in text, text)
    print("round_report self-test passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root",
        default="~/.clay/chats",
        help="chat storage root (default: ~/.clay/chats)",
    )
    parser.add_argument("--json", action="store_true", help="print one JSON object")
    parser.add_argument(
        "--self-test", action="store_true", help="run the synthetic fixture and exit"
    )
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    root = os.path.expanduser(arguments.root)
    if not os.path.isdir(root):
        print(f"no chat directory at {root}", file=sys.stderr)
        return 1
    summary = collect(root)
    if arguments.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print(format_text(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
