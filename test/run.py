#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the uvrgw test suite.

  test/run.py [--valgrind] [--slow] [-k PATTERN] [--keep] [UVRGW]

Runs every test_* function of the test_*.py modules in this directory
against the uvrgw binary (default: ../uvrgw).  Needs gcc, mosquitto and,
for the CAN tests, a vcan0 interface that is up:

  ip link add vcan0 type vcan && ip link set vcan0 up

--valgrind runs uvrgw under valgrind (memcheck, leaks) with longer
timeouts, --slow adds long running tests.  The logs of every uvrgw run
are kept in the temp directory if a test fails or with --keep.
"""
import argparse
import importlib
import inspect
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import uvrgwtest  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description='uvrgw test suite')
    ap.add_argument('binary', nargs='?', default=os.path.join(HERE, '..', 'uvrgw'))
    ap.add_argument('--valgrind', action='store_true', help='run uvrgw under valgrind')
    ap.add_argument('--slow', action='store_true', help='include long running tests')
    ap.add_argument('-k', metavar='PATTERN', help='only tests whose name contains PATTERN')
    ap.add_argument('--keep', action='store_true', help='keep the temp directory')
    args = ap.parse_args()

    s = uvrgwtest.S
    s.binary = os.path.abspath(args.binary)
    s.valgrind = args.valgrind
    s.slow = args.slow
    s.tmp = tempfile.mkdtemp(prefix='uvrgw-test-')
    s.shim = os.path.join(s.tmp, 'shim.so')
    subprocess.run(['gcc', '-shared', '-fPIC', '-o', s.shim, os.path.join(HERE, 'shim.c'), '-ldl', '-lpthread'], check=True)

    tests = []
    for name in sorted(f[:-3] for f in os.listdir(HERE) if f.startswith('test_') and f.endswith('.py')):
        mod = importlib.import_module(name)
        funcs = [f for n, f in inspect.getmembers(mod, inspect.isfunction) if n.startswith('test_') and f.__module__ == name]
        for f in sorted(funcs, key=lambda f: f.__code__.co_firstlineno):
            full = '%s.%s' % (name, f.__name__)
            if args.k is None or args.k in full:
                tests.append((full, f))

    print('uvrgw %s%s, %d tests, logs in %s' % (s.binary, ' (valgrind)' if s.valgrind else '', len(tests), s.tmp))
    failed, skipped = [], 0
    for full, f in tests:
        t0 = time.monotonic()
        try:
            f()
            status = 'ok'
        except uvrgwtest.Skip as e:
            status = 'SKIP (%s)' % e
            skipped += 1
        except Exception:
            status = 'FAIL'
            failed.append((full, traceback.format_exc()))
        print('%-55s %-6s %5.1f s' % (full, status, time.monotonic() - t0), flush=True)

    for full, tb in failed:
        print('\n==== %s\n%s' % (full, tb))
    print('\n%d passed, %d failed, %d skipped' % (len(tests) - len(failed) - skipped, len(failed), skipped))
    if failed or args.keep:
        print('logs kept in', s.tmp)
    else:
        shutil.rmtree(s.tmp, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
