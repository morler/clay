#!/usr/bin/env python3
"""How often does one turn call the same tool with the same arguments?

#88 asks for this before any duplicate-call breaker gets written: killi
blocks an exact repeat on the third call and warns on near repeats, but
those numbers come from a different model and prompt. Without a measured
distribution here, a breaker is a guess that can break working turns.

Reads the chat journals under ~/.clay/chats/*/chat.json (or the directories
given as arguments) and prints, per turn, how many distinct fingerprints
repeated. Arguments are compared after normalizing whitespace, so a command
retyped with different spacing is still one fingerprint.

    scripts/repeat_report.py            # every saved chat
    scripts/repeat_report.py <chat dir> # one or more chat directories

Exit status is 0 regardless of the numbers: this reports, it does not gate.
"""

import collections
import glob
import json
import os
import re
import sys


def chat_dirs(argument_paths):
    if argument_paths:
        return argument_paths
    root = os.path.expanduser("~/.clay/chats")
    return sorted(glob.glob(os.path.join(root, "*")))


def normalize(entry):
    """Tool name plus its arguments with whitespace collapsed, so the same
    call retyped differently hashes the same."""
    arguments = entry.get("arguments")
    if isinstance(arguments, str):
        try:
            arguments = json.loads(arguments)
        except ValueError:
            return (entry.get("name", ""), arguments.strip())
    if isinstance(arguments, dict):
        normalized = {}
        for key in sorted(arguments):
            value = arguments[key]
            if isinstance(value, str):
                value = re.sub(r"\s+", " ", value).strip()
            normalized[key] = value
        arguments = json.dumps(normalized, sort_keys=True, ensure_ascii=False)
    return (entry.get("name", ""), arguments)


def turns_of(directory):
    path = os.path.join(directory, "chat.json")
    try:
        with open(path, encoding="utf-8") as file:
            journal = json.load(file)
    except (OSError, ValueError):
        return []
    return journal.get("turns") or []


def main():
    directories = chat_dirs(sys.argv[1:])
    per_turn = []  # (chat, turn index, counter, total calls)
    for directory in directories:
        for index, turn in enumerate(turns_of(directory)):
            calls = [
                call
                for record in turn.get("messages") or []
                for call in record.get("calls") or []
                if isinstance(call, dict) and call.get("name")
            ]
            if calls:
                counter = collections.Counter(normalize(call) for call in calls)
                per_turn.append((os.path.basename(directory), index, counter, len(calls)))

    if not per_turn:
        print("no tool calls in the given chats")
        return 0

    total_calls = sum(count for _, _, _, count in per_turn)
    distinct = collections.Counter()
    for _, _, counter, _ in per_turn:
        for fingerprint, count in counter.items():
            distinct[fingerprint] = max(distinct[fingerprint], count)

    print(f"{len(per_turn)} turns with tool calls, {total_calls} calls total")
    print(f"{len(distinct)} distinct fingerprints (same turn only)\n")

    print("most repeats of one fingerprint within a turn:")
    for (name, arguments), count in distinct.most_common(5):
        print(f"  {count:3d}x  {name}  {str(arguments)[:100]}")

    print("\nwhat a breaker at each threshold would have stopped:")
    for threshold in (2, 3, 4, 5, 8):
        fingerprints = sum(1 for count in distinct.values() if count >= threshold)
        # Calls a deduplicating breaker would skip: every call but the first.
        saved = sum(max(0, count - 1) for count in distinct.values() if count >= threshold)
        share = 100.0 * saved / total_calls
        print(
            f"  >={threshold}: {fingerprints:3d} fingerprints, "
            f"{saved:3d} calls skipped ({share:.1f}%)"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
