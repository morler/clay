#!/usr/bin/env python3
"""Exercise the actual interactive prompt through a POSIX pseudo-terminal."""
import os
import pty
import re
import select
import signal
import tempfile
import subprocess
import sys
import time


# The editor ends each redraw by positioning the cursor in its input row.
REDRAW = re.compile(rb'\x1b\[[0-9]+G')
TIMEOUT = 5


def run_case(events, expected):
    master, slave = pty.openpty()
    proc = None
    output = bytearray()
    try:
        proc = subprocess.Popen([sys.argv[1]], stdin=slave, stdout=slave, stderr=slave,
                                close_fds=True)

        def wait_redraw(start):
            deadline = time.monotonic() + TIMEOUT
            while not REDRAW.search(output, start):
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([master], [], [], remaining)[0]:
                    raise AssertionError(('prompt redraw timed out', bytes(output)))
                chunk = os.read(master, 65536)
                if not chunk:
                    raise AssertionError(('prompt closed during redraw', bytes(output)))
                output.extend(chunk)

        # Initial redraw follows raw mode setup, so input cannot be echoed or lost.
        wait_redraw(0)
        for event in events:
            start = len(output)
            if isinstance(event, tuple):
                for part in event:
                    if isinstance(part, float):
                        time.sleep(part)
                    elif isinstance(part, int):
                        os.kill(proc.pid, part)
                    else:
                        os.write(master, part)
                    time.sleep(0.08)
            else:
                os.write(master, event)
            wait_redraw(start)
        os.write(master, b'\r')
        deadline = time.monotonic() + TIMEOUT
        while proc.poll() is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(('prompt did not terminate', bytes(output)))
            if select.select([master], [], [], min(remaining, 0.05))[0]:
                output.extend(os.read(master, 65536))
        while select.select([master], [], [], 0)[0]:
            output.extend(os.read(master, 65536))
        if (proc.returncode != 0 or b'RESULT:' + expected + b'\r\n' not in output
                or b'\x1b[?2004h' not in output or b'\x1b[?2004l' not in output):
            raise AssertionError((proc.returncode, bytes(output)))
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)


def byte_events(text):
    return [bytes([byte]) for byte in text.encode('utf-8')]


# Send UTF-8 bytes separately, then edit across Chinese character boundaries.
run_case(byte_events('你好界') + [b'\x1b[D', b'\x7f'] + byte_events('世') +
         [b'\x1b[C'], '你世界'.encode())
run_case(byte_events('abc') + [b'\x1b[D', b'\x7f'] + byte_events('X'), b'aXc')
run_case([bytes([27]) + b"[200~hello" + bytes([10]) + b"world" + bytes([27]) + b"[201~"], b"hello" + bytes([13, 10]) + b"world")
# Pasted CRLF and lone CR both become LF, matching terminal line breaks.
run_case([b'\x1b[200~a\r\nb\rc\nd\x1b[201~'], b'a\r\nb\r\nc\r\nd')
run_case([(b'\x1b[200~' + '中文'.encode(), signal.SIGWINCH, b'\x1b[20', b'1~')], '中文'.encode())
run_case([b'old', (b'\x1b[200~part', signal.SIGINT, b'\nmalicious\n', b'\x1b[20', b'1~'), b'!'], b'old!')
run_case([b'old', b'\x1b[200~' + b'x' * (16 * 1024 * 1024) +
          b'\nmalicious\n\x1b[201~', b'!'], b'old!')

# Even after multiple interrupts or a long pause, paste bytes remain isolated.
run_case([b'old', (b'\x1b[200~part', signal.SIGINT, signal.SIGINT, 1.2,
                    b'\nmalicious\n', b'\x1b[201~'), b'!'], b'old!')
# NUL rejects the entire paste, including delayed multiline data and split markers.
run_case([b'old', (b'\x1b[200~prefix\x00', 1.2, b'\nmalicious\nsecond\n',
                    b'\x1b[20', b'1~'), b'!'], b'old!')
run_case([b'old', b'\x1b[200~\x1b\x00tail\x1b[201~', b'!'], b'old!')

def eof_case():
    master, slave = pty.openpty()
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, 'result')
        proc = subprocess.Popen([sys.argv[1], path], stdin=slave, stdout=slave, stderr=slave)
        try:
            output = bytearray()
            deadline = time.monotonic() + TIMEOUT
            while not REDRAW.search(output):
                if not select.select([master], [], [], max(0, deadline - time.monotonic()))[0]:
                    raise AssertionError('initial redraw timed out')
                output.extend(os.read(master, 65536))
            os.write(master, b'old')
            time.sleep(0.1)
            os.write(master, b'\x1b[200~partial\x1b[20')
            time.sleep(0.1)
            os.close(master)
            master = -1
            proc.wait(timeout=TIMEOUT)
            assert proc.returncode == 1
            assert not os.path.exists(path), 'incomplete paste was submitted'
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            if master >= 0:
                os.close(master)
            os.close(slave)

eof_case()

print("prompt PTY tests passed")
