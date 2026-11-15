#!/usr/bin/env python3
"""Exercise subagent cancellation using a real terminal."""
import os
import pty
import select
import signal
import subprocess
import sys
import tempfile
import time

for scenario in ('double_escape', 'escape_sigint', 'late_escape', 'late_sigint'):
  master, slave = pty.openpty()
  with tempfile.TemporaryDirectory() as home:
      proc = subprocess.Popen([sys.argv[1]], stdin=slave, stdout=slave, stderr=slave,
                              env={**os.environ, 'HOME': home})
      output = bytearray()

      def wait_for(marker):
          deadline = time.monotonic() + 5
          while marker not in output:
              remaining = deadline - time.monotonic()
              if remaining <= 0 or not select.select([master], [], [], remaining)[0]:
                  raise AssertionError((marker, bytes(output)))
              output.extend(os.read(master, 65536))

      try:
          wait_for(b'READY')
          os.write(master, b'\x0f')
          time.sleep(0.3)
          assert proc.poll() is None and b'CANCELLED' not in output
          os.write(master, b'\x1b\x1b' if scenario == 'double_escape' else b'\x1b')
          if scenario == 'escape_sigint':
              os.kill(proc.pid, signal.SIGINT)
          wait_for(b'CANCELLED')
          if scenario == 'late_escape':
              os.write(master, b'\x1b')
          if scenario == 'late_sigint':
              os.kill(proc.pid, signal.SIGINT)
          wait_for(b'SECOND')
          wait_for(b'INPUT')
          os.write(master, b'x')
          wait_for(b'PRESERVED')
          assert proc.wait(timeout=5) == 0
      finally:
          if proc.poll() is None:
              proc.kill()
              proc.wait()
          os.close(master)
          os.close(slave)
print('subagent PTY tests passed')
