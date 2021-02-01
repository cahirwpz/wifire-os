#!/usr/bin/env python3

import argparse
import pexpect
import signal
import sys
import random
import os
from launcher import RandomPort, getvar, setvar, setboard


N_SIMPLE = 10
TIMEOUT = 60
RETRIES_MAX = 5
REPEAT = 5


# Tries to decode binary output as ASCII, as hard as it can.
def safe_decode(data):
    s = data.decode('unicode_escape', errors='replace')
    return s.replace('\r', '')


def send_command(gdb, cmd):
    try:
        gdb.expect_exact(['(gdb)'], timeout=3)
    except pexpect.exceptions.TIMEOUT:
        gdb.kill(signal.SIGINT)
    gdb.sendline(cmd)
    print(safe_decode(gdb.before), end='', flush=True)
    print(safe_decode(gdb.after), end='', flush=True)


# Tries to start gdb in order to investigate kernel state on deadlock or crash.
def gdb_inspect(interactive):
    gdb_cmd = getvar('gdb.binary')
    gdb_port = getvar('config.gdbport')
    if interactive:
        gdb_opts = ['-iex=set auto-load safe-path {}/'.format(os.getcwd()),
                    '-ex=target remote localhost:%u' % gdb_port,
                    '--silent', getvar('config.kernel')]
    else:
        # Note: These options are different than .gdbinit.
        gdb_opts = ['-ex=target remote localhost:%u' % gdb_port,
                    '-ex=python import os, sys',
                    '-ex=python sys.path.append(os.getcwd() + "/sys")',
                    '-ex=python import debug',
                    '-ex=set pagination off',
                    '--nh', '--nx', '--silent', getvar('config.kernel')]
    gdb = pexpect.spawn(gdb_cmd, gdb_opts, timeout=3)
    if interactive:
        send_command(gdb, 'backtrace full')
        send_command(gdb, 'kthread')
        gdb.interact()
    else:
        send_command(gdb, 'info registers')
        send_command(gdb, 'cpu tlb')
        send_command(gdb, 'backtrace full')
        # following commands may fail
        send_command(gdb, 'kthread')
        send_command(gdb, 'klog')
        # we need dummy command - otherwise previous one won't appear
        send_command(gdb, 'quit')


def test_seed(seed, interactive=True, repeat=1, retry=0):
    if retry == RETRIES_MAX:
        print("Maximum retries reached, still not output received. "
              "Test inconclusive.")
        sys.exit(1)

    print("Testing seed %u..." % seed)
    child = pexpect.spawn('./launch',
                          ['--board', getvar('board'),
                           '--port', str(getvar('config.gdbport')),
                           '-t', 'test=all', 'klog-quiet=1',
                           'seed=%u' % seed, 'repeat=%d' % repeat])
    index = child.expect_exact(
        ['[TEST PASSED]', '[TEST FAILED]', '[PANIC]', '[TEST TIMEOUT]',
         pexpect.EOF, pexpect.TIMEOUT], timeout=TIMEOUT)
    if index == 0:
        child.terminate(True)
        return
    elif index in [1, 2, 3]:
        print("Test failure reported!\n")
        message = safe_decode(child.before)
        message += safe_decode(child.buffer)
        try:
            while len(message) < 20000:
                message += safe_decode(child.read_nonblocking(timeout=1))
        except pexpect.exceptions.TIMEOUT:
            pass
        print(message)
        gdb_inspect(interactive)
        sys.exit(1)
    elif index == 4:
        message = safe_decode(child.before)
        message += safe_decode(child.buffer)
        print(message)
        print("EOF reached without success report. This may indicate "
              "a problem with the testing framework or QEMU. "
              "Retrying (%d)..." % (retry + 1))
        test_seed(seed, interactive, repeat, retry + 1)
    elif index == 5:
        print("Timeout reached!\n")
        message = safe_decode(child.buffer)
        print(message)
        if len(message) < 100:
            print("It looks like kernel did not even start within the time "
                  "limit. Retrying (%d)..." % (retry + 1))
            child.terminate(True)
            test_seed(seed, interactive, repeat, retry + 1)
        else:
            gdb_inspect(interactive)
            print("No test result reported within timeout. Unable to verify "
                  "test success. Seed was: %u, repeat: %d" % (seed, repeat))
            sys.exit(1)


def sigterm_handler(_signo, _stack_frame):
    sys.exit(1)


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, sigterm_handler)
    signal.signal(signal.SIGINT, sigterm_handler)
    signal.signal(signal.SIGHUP, sigterm_handler)

    parser = argparse.ArgumentParser(
        description='Automatically performs kernel tests.')
    parser.add_argument('--times', type=int, default=N_SIMPLE,
                        help='Run tests given number of times.')
    parser.add_argument('--non-interactive', action='store_true',
                        help='Do not run gdb session if tests fail.')
    parser.add_argument('--board', default='malta', choices=['malta', 'rpi3'],
                        help='Emulated board.')
    args = parser.parse_args()

    setboard(args.board)
    setvar('config.gdbport', RandomPort())

    interactive = not args.non_interactive

    # Run tests using n random seeds
    for i in range(0, args.times):
        seed = random.randint(0, 2**32)
        test_seed(seed, interactive, REPEAT)

    print("Tests successful!")
    sys.exit(0)
